/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Shape groups (see include/ldml/shape_groups.h).

#include <ldml/shape_groups.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <llvm/Support/Casting.h>
#include <ldml/transform_plan.h>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include <re/analysis/re_analysis.h>
#include <re/analysis/replacement_analysis.h>
#include <re/transforms/capture_removal.h>
#include "charset_analysis.h"

using namespace llvm;

namespace ldml {

namespace {

// An item of a result: a literal character, a reference to a capture, or a
// function call on a capture.
struct ResultItem {
    enum class Kind {Literal, Reference, Call};
    Kind kind;
    UCD::codepoint_t literal = 0;
    std::string capture;            // the name of the capture
    std::string function;           // the function called
};

bool resultItems(re::RE * r, std::vector<ResultItem> & items) {
    if (r == nullptr) return true;
    if (re::Seq * const seq = dyn_cast<re::Seq>(r)) {
        for (re::RE * e : *seq) {
            if (!resultItems(e, items)) return false;
        }
        return true;
    } else if (re::Alt * const alt = dyn_cast<re::Alt>(r)) {
        return (alt->size() == 1) && resultItems(alt->front(), items);
    } else if (const re::CC * const cc = dyn_cast<re::CC>(r)) {
        if (cc->empty() || (cc->min_codepoint() != cc->max_codepoint())) return false;
        items.push_back(ResultItem{ResultItem::Kind::Literal, cc->min_codepoint(), "", ""});
        return true;
    } else if (re::String * const s = dyn_cast<re::String>(r)) {
        for (const char32_t c : *s) items.push_back(ResultItem{ResultItem::Kind::Literal, c, "", ""});
        return true;
    } else if (re::Reference * const ref = dyn_cast<re::Reference>(r)) {
        items.push_back(ResultItem{ResultItem::Kind::Reference, 0, ref->getName(), ""});
        return true;
    } else if (re::Name * const n = dyn_cast<re::Name>(r)) {
        if (isFunctionCall(n)) {
            std::vector<ResultItem> argument;
            if (!resultItems(n->getDefinition(), argument) || (argument.size() != 1) ||
                    (argument[0].kind != ResultItem::Kind::Reference)) return false;
            items.push_back(ResultItem{ResultItem::Kind::Call, 0, argument[0].capture, getFunctionID(n).getText()});
            return true;
        }
        return (n->getDefinition() != nullptr) && resultItems(n->getDefinition(), items);
    }
    return false;
}

std::string hex(UCD::codepoint_t cp) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%X", static_cast<unsigned>(cp));
    return buf;
}

std::string shapeKey(const std::vector<ResultItem> & items) {
    std::string s;
    for (const ResultItem & item : items) {
        if (!s.empty()) s += " ";
        switch (item.kind) {
            case ResultItem::Kind::Literal: s += "\\x{" + hex(item.literal) + "}"; break;
            case ResultItem::Kind::Reference: s += "$" + item.capture; break;
            case ResultItem::Kind::Call: s += "&" + item.function + "($" + item.capture + ")"; break;
        }
    }
    return s.empty() ? "(empty)" : s;
}

// The last character set of a text (the set of its last item, which must be
// a set of single characters, not repeated), and the characters of the rest.
bool lastItem(re::RE * r, UCD::UnicodeSet & last, UCD::UnicodeSet & rest, CharSetAnalysis & sets) {
    if (re::Seq * const seq = dyn_cast<re::Seq>(r)) {
        if (seq->empty()) return false;
        std::vector<re::RE *> front(seq->begin(), seq->end() - 1);
        rest = rest + patternCharacters(re::makeSeq(front.begin(), front.end()));
        return lastItem(seq->back(), last, rest, sets);
    } else if (re::Capture * const c = dyn_cast<re::Capture>(r)) {
        return lastItem(c->getCapturedRE(), last, rest, sets);
    } else if (re::Alt * const alt = dyn_cast<re::Alt>(r)) {
        if (alt->size() == 1) return lastItem(alt->front(), last, rest, sets);
    }
    if (!CharSetAnalysis::isSet(r)) return false;
    const UCD::UnicodeSet withStrings = sets.setOf(r, true);
    const UCD::UnicodeSet singles = sets.setOf(r, false);
    if (!(withStrings == singles)) return false;     // (strings)
    last = last + singles;
    return true;
}

re::RE * withoutCaptures(re::RE * r) {
    return re::removeUnneededCaptures(r, re::makeSeq());
}

// The capture named in the source side of a rule.
re::Capture * captureNamed(const ConversionRule * rule, const std::string & name) {
    const auto captures = re::capturesByName(rule->getLeftSide()->getText());
    const auto f = captures.find(name);
    return (f == captures.end()) ? nullptr : f->second;
}

// The strings (of two or more characters) of a set, through alternations,
// differences, intersections and variables.
std::set<std::u32string> setStrings(const re::RE * r) {
    std::set<std::u32string> strings;
    if (const re::Alt * const alt = dyn_cast<re::Alt>(r)) {
        for (const re::RE * a : *alt) {
            const std::set<std::u32string> s = setStrings(a);
            strings.insert(s.begin(), s.end());
        }
    } else if (const re::Seq * const seq = dyn_cast<re::Seq>(r)) {
        std::u32string str;
        for (const re::RE * e : *seq) {
            const re::CC * const cc = dyn_cast<re::CC>(e);
            if ((cc == nullptr) || cc->empty() || (cc->min_codepoint() != cc->max_codepoint())) return strings;
            str.push_back(cc->min_codepoint());
        }
        if (str.size() > 1) strings.insert(str);
    } else if (const re::Diff * const d = dyn_cast<re::Diff>(r)) {
        strings = setStrings(d->getLH());
        for (const std::u32string & str : setStrings(d->getRH())) strings.erase(str);
    } else if (const re::Intersect * const x = dyn_cast<re::Intersect>(r)) {
        const std::set<std::u32string> rh = setStrings(x->getRH());
        for (const std::u32string & str : setStrings(x->getLH())) if (rh.count(str)) strings.insert(str);
    } else if (const re::Name * const n = dyn_cast<re::Name>(r)) {
        if (!isFunctionCall(n) && n->getDefinition()) strings = setStrings(n->getDefinition());
    }
    return strings;
}

// A pattern with its sets made plain for the engine: a set with strings
// becomes the alternation of its strings and the class of its characters; a
// difference, intersection, union or variable of sets, the class of its
// characters; a class or a property is kept.  Variables that are not sets
// are replaced by their definitions.
re::RE * plainSets(re::RE * r, CharSetAnalysis & sets) {
    if (isa<re::CC>(r) || isa<re::PropertyExpression>(r) || isBoundary(r)) return r;
    if (!isa<re::Seq>(r) && CharSetAnalysis::isSet(r)) {
        const std::set<std::u32string> strings = setStrings(r);
        re::RE * const chars = re::makeCC(sets.setOf(r, false), &cc::Unicode);
        if (strings.empty()) return chars;
        std::vector<re::RE *> alts;
        for (const std::u32string & str : strings) {
            std::vector<re::RE *> cs;
            for (const char32_t c : str) cs.push_back(re::makeCC(static_cast<UCD::codepoint_t>(c), &cc::Unicode));
            alts.push_back(re::makeSeq(cs.begin(), cs.end()));
        }
        if (!cast<re::CC>(chars)->empty()) alts.push_back(chars);
        return re::makeAlt(alts.begin(), alts.end());
    }
    if (re::Seq * const seq = dyn_cast<re::Seq>(r)) {
        std::vector<re::RE *> items;
        for (re::RE * e : *seq) items.push_back(plainSets(e, sets));
        return re::makeSeq(items.begin(), items.end());
    } else if (re::Alt * const alt = dyn_cast<re::Alt>(r)) {
        std::vector<re::RE *> items;
        for (re::RE * e : *alt) items.push_back(plainSets(e, sets));
        return re::makeAlt(items.begin(), items.end());
    } else if (re::Rep * const rep = dyn_cast<re::Rep>(r)) {
        return re::makeRep(plainSets(rep->getRE(), sets), rep->getLB(), rep->getUB(), rep->getKind());
    } else if (re::Capture * const c = dyn_cast<re::Capture>(r)) {
        return re::makeCapture(c->getName(), plainSets(c->getCapturedRE(), sets));
    } else if (re::Assertion * const a = dyn_cast<re::Assertion>(r)) {
        return re::makeAssertion(plainSets(a->getAsserted(), sets), a->getKind(), a->getSense());
    } else if (re::Name * const n = dyn_cast<re::Name>(r)) {
        if (!isFunctionCall(n) && n->getDefinition()) return plainSets(n->getDefinition(), sets);
    }
    return r;
}

// The characters with which a (plain) pattern may begin, and whether it may
// match the empty string; false if unknown.
bool firstSet(re::RE * r, UCD::UnicodeSet & first, bool & nullable, CharSetAnalysis & sets) {
    nullable = false;
    if (isa<re::CC>(r) || isa<re::PropertyExpression>(r)) {
        first = first + sets.setOf(r, true);
        return true;
    } else if (isa<re::Assertion>(r) || isa<re::Start>(r) || isa<re::End>(r)) {
        nullable = true;
        return true;
    } else if (re::Seq * const seq = dyn_cast<re::Seq>(r)) {
        nullable = true;
        for (auto i = seq->begin(); nullable && (i != seq->end()); ++i) {
            if (!firstSet(*i, first, nullable, sets)) return false;
        }
        return true;
    } else if (re::Alt * const alt = dyn_cast<re::Alt>(r)) {
        for (re::RE * e : *alt) {
            bool n;
            if (!firstSet(e, first, n, sets)) return false;
            nullable = nullable || n;
        }
        return true;
    } else if (re::Rep * const rep = dyn_cast<re::Rep>(r)) {
        if (!firstSet(rep->getRE(), first, nullable, sets)) return false;
        nullable = nullable || (rep->getLB() == 0);
        return true;
    } else if (re::Capture * const c = dyn_cast<re::Capture>(r)) {
        return firstSet(c->getCapturedRE(), first, nullable, sets);
    }
    return false;
}

// A (plain) pattern matched as ICU matches it, with the repetitions that may
// give back characters to what follows them (follow: the characters that may
// follow the pattern; unknown: anything may) made possessive.  A repetition
// whose characters may not begin what follows it is left greedy: it takes
// the same characters.
re::RE * possessiveWhereNeeded(re::RE * r, const UCD::UnicodeSet & follow, bool unknown, CharSetAnalysis & sets) {
    if (re::Seq * const seq = dyn_cast<re::Seq>(r)) {
        std::vector<re::RE *> items(seq->begin(), seq->end());
        UCD::UnicodeSet f = follow;
        bool u = unknown;
        for (size_t i = items.size(); i-- > 0; ) {
            re::RE * const item = items[i];
            items[i] = possessiveWhereNeeded(item, f, u, sets);
            UCD::UnicodeSet first;
            bool nullable;
            if (!firstSet(item, first, nullable, sets)) {
                u = true;
            } else if (nullable) {
                f = f + first;
            } else {
                f = first;
                u = false;
            }
        }
        return re::makeSeq(items.begin(), items.end());
    } else if (re::Alt * const alt = dyn_cast<re::Alt>(r)) {
        std::vector<re::RE *> items;
        for (re::RE * e : *alt) items.push_back(possessiveWhereNeeded(e, follow, unknown, sets));
        return re::makeAlt(items.begin(), items.end());
    } else if (re::Capture * const c = dyn_cast<re::Capture>(r)) {
        return re::makeCapture(c->getName(), possessiveWhereNeeded(c->getCapturedRE(), follow, unknown, sets));
    } else if (re::Rep * const rep = dyn_cast<re::Rep>(r)) {
        if (rep->getLB() == rep->getUB()) return r;
        UCD::UnicodeSet first;
        bool nullable;
        const bool known = firstSet(rep->getRE(), first, nullable, sets);
        const UCD::UnicodeSet chars = patternCharacters(rep->getRE());
        if (known && !unknown && !chars.intersects(follow)) return r;
        return re::makeRep(rep->getRE(), rep->getLB(), rep->getUB(), re::Rep::Kind::Possessive);
    }
    return r;
}

struct RuleInfo {
    const ConversionRule * rule;
    std::vector<ResultItem> items;
    std::string shape;
};

const std::string CharacterMapShape = "(character map)";

// A rule of a character map: a character of a set (without strings) mapped to
// literal text, without contexts.
bool characterMapRule(const RuleInfo & info, CharSetAnalysis & sets) {
    const RuleSide * const src = info.rule->getLeftSide();
    if (src->hasBeforeContext() || src->hasAfterContext()) return false;
    for (const ResultItem & item : info.items) if (item.kind != ResultItem::Kind::Literal) return false;
    re::RE * const text = src->getText();
    if (!CharSetAnalysis::isSet(text) || isa<re::Seq>(text)) return false;
    return setStrings(text).empty() && (sets.setOf(text, true) == sets.setOf(text, false));
}

}

// The largest number of analyses of pairs of rules (RuleOverlapAnalysis) in
// the plan of a group: a few seconds at most.
constexpr size_t MaxShapePairAnalyses = 200000;

ShapeGroupPlan planShapeGroup(const std::vector<const ConversionRule *> & rules, const FunctionEvaluator * evaluate) {
    ShapeGroupPlan plan;
    auto problem = [&](const std::string & p) {plan.problems.push_back(p);};
    CharSetAnalysis sets;
    std::vector<RuleInfo> infos;
    for (const ConversionRule * r : rules) {
        const RuleSide * const src = r->getLeftSide();
        const RuleSide * const res = r->getRightSide();
        RuleInfo info{r, {}, ""};
        if (res->hasCursor() && ((res->getCursorOffset() != 0) || (res->getResultToRevisit() &&
                !(isa<re::Seq>(res->getResultToRevisit()) && cast<re::Seq>(res->getResultToRevisit())->empty())))) {
            problem("a result with text to revisit or a cursor: " + printRule(r));
            continue;
        }
        if (!resultItems(res->getText(), info.items)) {
            problem("a result that is not literal text, references to captures and function calls on them: " + printRule(r));
            continue;
        }
        // The captures, numbered 1 to k in their order; each referred to once.
        const auto captures = re::allCapturesByName(src->getText());
        bool numbered = true;
        for (size_t k = 1; k <= captures.size(); k++) {
            const auto f = captures.find(std::to_string(k));
            numbered = numbered && (f != captures.end()) && (f->second.size() == 1);
        }
        if (!numbered) {
            problem("captures not numbered in their order: " + printRule(r));
            continue;
        }
        std::set<std::string> referred;
        bool once = true;
        for (const ResultItem & item : info.items) {
            if (item.kind == ResultItem::Kind::Literal) continue;
            once = once && referred.insert(item.capture).second && (captures.count(item.capture) != 0);
            if (item.kind == ResultItem::Kind::Call) {
                re::Capture * const c = captureNamed(r, item.capture);
                const auto range = re::getLengthRange(c->getCapturedRE(), &cc::Unicode);
                if ((range.first != 1) || (range.second != 1)) {
                    problem("a function called on a capture that is not of single characters: " + printRule(r));
                    once = false;
                }
            }
        }
        if (!once) {
            problem("a capture referred to more than once: " + printRule(r));
            continue;
        }
        info.shape = shapeKey(info.items) + (captures.empty() ? "" : "  (" + std::to_string(captures.size()) + " captures)");
        if (characterMapRule(info, sets)) info.shape = CharacterMapShape;
        infos.push_back(std::move(info));
    }
    if (!plan.problems.empty()) return plan;

    // The rules must match at different positions.  (The analyses of pairs of
    // rules are limited: see MaxShapePairAnalyses.)
    const size_t n = infos.size();
    if (n * (n - 1) / 2 > MaxShapePairAnalyses) {
        problem("too many rules for the analyses of shape groups: " + std::to_string(n) + " rules");
        return plan;
    }
    RuleOverlapAnalysis analysis;
    size_t analyses = 0;
    bool exhausted = false;
    struct Budgeted {
        RuleOverlapAnalysis & a;
        size_t & count;
        bool & exhausted;
        bool spend() {
            if (++count > MaxShapePairAnalyses) exhausted = true;
            return !exhausted;
        }
        // (Out of budget, the analyses stop: a problem is reported after
        // each stage of the plan, see outOfBudget.)
        bool mayOverlap(const ConversionRule * x, const ConversionRule * y) {return spend() && a.mayOverlap(x, y);}
        bool mayMatchWithin(const ConversionRule * x, const ConversionRule * y) {return spend() && a.mayMatchWithin(x, y);}
    } overlaps{analysis, analyses, exhausted};
    auto outOfBudget = [&] {
        if (exhausted) problem("too many analyses of pairs of rules for shape groups (more than " +
                               std::to_string(MaxShapePairAnalyses) + ")");
        return exhausted;
    };
    for (size_t i = 0; i < infos.size(); i++) {
        for (size_t j = i + 1; j < infos.size(); j++) {
            if (overlaps.mayOverlap(infos[i].rule, infos[j].rule)) {
                problem("rules that may match at the same position: " + printRule(infos[i].rule) + "  |  " +
                        printRule(infos[j].rule));
                return plan;
            }
        }
    }

    // The deferred calls, by function.
    std::map<std::string, size_t> callOf;
    for (const RuleInfo & info : infos) {
        for (const ResultItem & item : info.items) {
            if (item.kind != ResultItem::Kind::Call) continue;
            auto f = callOf.find(item.function);
            if (f == callOf.end()) {
                if (plan.calls.size() == DeferredCallMarks) {
                    problem("more functions called than marks for them");
                    return plan;
                }
                f = callOf.emplace(item.function, plan.calls.size()).first;
                DeferredCall call;
                call.function = TransformID(item.function);
                call.mark = DeferredCallMark + static_cast<UCD::codepoint_t>(plan.calls.size());
                plan.calls.push_back(call);
            }
            plan.calls[f->second].arguments =
                plan.calls[f->second].arguments + patternCharacters(captureNamed(info.rule, item.capture)->getCapturedRE());
        }
    }
    for (DeferredCall & call : plan.calls) {
        if (evaluate == nullptr) continue;
        std::vector<std::pair<UCD::UnicodeSet, std::u32string>> groups;
        UCD::UnicodeSet unchanged;
        std::string reason;
        if (!(*evaluate)(call.function, call.arguments, groups, unchanged, reason)) continue;
        for (const auto & g : groups) {
            for (const auto & range : g.first) {
                for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) call.values[cp] = g.second;
            }
        }
        for (const auto & range : unchanged) {
            for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) call.values[cp] = std::u32string(1, cp);
        }
        call.evaluated = true;
    }

    // The shapes, in the order of their first rules.
    std::vector<ShapePass> shapes;
    std::map<std::string, size_t> shapeOf;
    std::vector<std::vector<const RuleInfo *>> shapeInfos;
    for (const RuleInfo & info : infos) {
        auto f = shapeOf.find(info.shape);
        if (f == shapeOf.end()) {
            f = shapeOf.emplace(info.shape, shapes.size()).first;
            shapes.emplace_back();
            shapes.back().shape = info.shape;
            shapes.back().map = info.shape == CharacterMapShape;
            shapeInfos.emplace_back();
        }
        shapes[f->second].rules.push_back(info.rule);
        shapeInfos[f->second].push_back(&info);
        if (info.rule->getLeftSide()->hasBeforeContext()) shapes[f->second].contexts = true;
    }
    const size_t S = shapes.size();

    // The replacement of each shape (as written by its first rule).
    for (size_t x = 0; x < S; x++) {
        if (shapes[x].map) continue;
        const RuleInfo & first = *shapeInfos[x].front();
        std::vector<re::RE *> items;
        for (const ResultItem & item : first.items) {
            if (item.kind == ResultItem::Kind::Literal) {
                items.push_back(re::makeCC(item.literal, &cc::Unicode));
                continue;
            }
            if (item.kind == ResultItem::Kind::Call) {
                items.push_back(re::makeCC(plan.calls[callOf[item.function]].mark, &cc::Unicode));
            }
            items.push_back(re::makeReference(item.capture, captureNamed(first.rule, item.capture), 0));
        }
        shapes[x].replacement = re::makeSeq(items.begin(), items.end());
    }

    // The text written by a rule (marks for the calls), as the text of a rule
    // (for the overlap analysis).
    auto written = [&](const RuleInfo & info) {
        std::vector<re::RE *> items;
        for (const ResultItem & item : info.items) {
            if (item.kind == ResultItem::Kind::Literal) {
                items.push_back(re::makeCC(item.literal, &cc::Unicode));
                continue;
            }
            if (item.kind == ResultItem::Kind::Call) {
                items.push_back(re::makeCC(plan.calls[callOf[item.function]].mark, &cc::Unicode));
            }
            items.push_back(withoutCaptures(captureNamed(info.rule, item.capture)->getCapturedRE()));
        }
        return makeConversionRule(makeRuleSide(re::makeSeq(items.begin(), items.end())), Direction::Forward,
                                  makeRuleSide(re::makeSeq()));
    };

    // The shapes without before contexts: the yields between them.
    size_t contextShape = S;
    std::vector<size_t> plain;
    for (size_t x = 0; x < S; x++) {
        if (!shapes[x].contexts) {
            plain.push_back(x);
        } else if (contextShape != S) {
            problem("more than one shape with before contexts: " + shapes[contextShape].shape + "  |  " + shapes[x].shape);
            return plan;
        } else {
            contextShape = x;
        }
    }
    auto within = [&](size_t x, size_t y) {      // a match of x may begin within one of y
        for (const RuleInfo * a : shapeInfos[x]) {
            for (const RuleInfo * b : shapeInfos[y]) {
                if (overlaps.mayMatchWithin(a->rule, b->rule)) return true;
            }
        }
        return false;
    };
    std::vector<std::vector<size_t>> yieldsTo(S);
    std::vector<std::vector<size_t>> appliedAfter(S);   // (the character map, after the shapes it may lie within)
    for (size_t x : plain) {
        for (size_t y : plain) {
            if ((x == y) || !within(x, y)) continue;
            if (shapes[x].map) {
                appliedAfter[x].push_back(y);
                continue;
            }
            if (within(y, x)) {
                problem("shapes whose matches may begin within each other's: " + shapes[x].shape + "  |  " + shapes[y].shape);
                continue;
            }
            // The matches of y containing the start of a match of x must end where it ends.
            UCD::UnicodeSet last, rest;
            bool ok = true;
            for (size_t z : {x, y}) {
                for (const RuleInfo * info : shapeInfos[z]) {
                    ok = ok && !info->rule->getLeftSide()->hasAfterContext() &&
                         lastItem(info->rule->getLeftSide()->getText(), last, rest, sets);
                }
            }
            if (!ok || last.intersects(rest)) {
                problem("a shape whose matches may begin within those of another, which may not end where they end: " +
                        shapes[x].shape + "  within  " + shapes[y].shape);
                continue;
            }
            yieldsTo[x].push_back(y);
        }
    }
    if (outOfBudget() || !plan.problems.empty()) return plan;
    // The order of the passes: a shape before those it yields to (in the
    // order of the first rules, as far as possible).
    std::vector<size_t> order;
    std::vector<bool> placed(S, false);
    while (order.size() < plain.size()) {
        bool progress = false;
        for (size_t x : plain) {
            if (placed[x]) continue;
            // x may be placed when no unplaced shape yields to it, nor must
            // be applied before it.
            bool ready = true;
            for (size_t z : plain) {
                if (!placed[z] && (z != x) && ((std::find(yieldsTo[z].begin(), yieldsTo[z].end(), x) != yieldsTo[z].end()) ||
                                              (std::find(appliedAfter[x].begin(), appliedAfter[x].end(), z) != appliedAfter[x].end()))) {
                    ready = false;
                }
            }
            if (!ready) continue;
            order.push_back(x);
            placed[x] = true;
            progress = true;
            break;
        }
        if (!progress) {
            problem("shapes yielding to one another in a cycle");
            return plan;
        }
    }
    // A shape applied after another may not match the text it writes, and
    // reads no text after its matches.
    for (size_t i = 0; i < order.size(); i++) {
        for (size_t j = i + 1; j < order.size(); j++) {
            for (const RuleInfo * b : shapeInfos[order[j]]) {
                if (b->rule->getLeftSide()->hasAfterContext()) {
                    problem("a shape applied after another, with an after context: " + printRule(b->rule));
                }
                for (const RuleInfo * a : shapeInfos[order[i]]) {
                    const ConversionRule * const w = written(*a);
                    if (overlaps.mayOverlap(b->rule, w) || overlaps.mayMatchWithin(b->rule, w) ||
                            overlaps.mayMatchWithin(w, b->rule)) {
                        problem("a shape that may match the text written by a shape applied before it: " +
                                printRule(b->rule) + "  in the text of  " + printRule(a->rule));
                    }
                }
            }
        }
    }

    if (outOfBudget() || !plan.problems.empty()) return plan;
    // The search patterns.
    auto alternative = [&](const ConversionRule * r, re::RE * before) {
        std::vector<re::RE *> items;
        if (before) items.push_back(re::makeLookBehindAssertion(before));
        // The text, as ICU matches it: possessively (where that matters),
        // followed by the after context (or, without one, by anything).
        re::RE * after = nullptr;
        UCD::UnicodeSet follow;
        bool unknown = true;
        if (r->getLeftSide()->hasAfterContext()) {
            after = plainSets(engineContext(r->getLeftSide()->getAfterContext(), true), sets);
            bool nullable;
            unknown = !firstSet(after, follow, nullable, sets) || nullable;
        }
        items.push_back(possessiveWhereNeeded(plainSets(r->getLeftSide()->getText(), sets), follow, unknown, sets));
        if (after) items.push_back(re::makeLookAheadAssertion(possessiveContext(after, true)));
        return re::makeSeq(items.begin(), items.end());
    };
    auto searchOf = [&](size_t x, const std::vector<re::RE *> & befores) {
        std::vector<re::RE *> alts;
        for (size_t k = 0; k < shapes[x].rules.size(); k++) alts.push_back(alternative(shapes[x].rules[k], befores[k]));
        return re::makeAlt(alts.begin(), alts.end());
    };
    for (size_t x : plain) shapes[x].search = searchOf(x, std::vector<re::RE *>(shapes[x].rules.size(), nullptr));
    for (size_t x : plain) {
        if (yieldsTo[x].empty()) continue;
        std::vector<re::RE *> items{shapes[x].search};
        for (size_t y : yieldsTo[x]) items.push_back(re::makeNegativeLookBehindAssertion(withoutCaptures(shapes[y].search)));
        shapes[x].search = re::makeSeq(items.begin(), items.end());
    }

    // The shape with before contexts, after the others.
    if (contextShape != S) {
        ShapePass & c = shapes[contextShape];
        // The characters deleted: by each deferred call (after its mark), and
        // by the shape itself (its rules of one character whose result is
        // empty, or a call on it).
        std::vector<UCD::UnicodeSet> deletedBy(plan.calls.size());
        bool known = true;
        for (size_t k = 0; k < plan.calls.size(); k++) {
            if (!plan.calls[k].evaluated) {
                known = false;
                continue;
            }
            for (const auto & v : plan.calls[k].values) if (v.second.empty()) deletedBy[k].insert(v.first);
        }
        if (!known) {
            problem("the values of the functions called are not known: the before contexts of " + c.shape +
                    " depend on them");
            return plan;
        }
        UCD::UnicodeSet deletedOwn;
        // The last characters of the texts written into the converted text
        // where a context may see them: the values of the calls, and the
        // shape's own results.
        UCD::UnicodeSet lastWritten, marks;
        for (size_t k = 0; k < plan.calls.size(); k++) {
            marks.insert(plan.calls[k].mark);
            for (const auto & v : plan.calls[k].values) if (!v.second.empty()) lastWritten.insert(v.second.back());
        }
        for (const RuleInfo * info : shapeInfos[contextShape]) {
            const re::RE * const text = info->rule->getLeftSide()->getText();
            if (info->rule->getLeftSide()->hasAfterContext()) {
                problem("a shape with before contexts, applied after others, with an after context: " + printRule(info->rule));
            }
            const auto range = re::getLengthRange(const_cast<re::RE *>(text), &cc::Unicode);
            const UCD::UnicodeSet textChars = patternCharacters(const_cast<re::RE *>(text));
            if (info->items.empty()) {
                if ((range.first == 1) && (range.second == 1)) deletedOwn = deletedOwn + textChars;
                else problem("a rule of a shape with before contexts deleting more than one character: " + printRule(info->rule));
                continue;
            }
            const ResultItem & lastItem = info->items.back();
            if (lastItem.kind == ResultItem::Kind::Literal) {
                lastWritten.insert(lastItem.literal);
            } else if (lastItem.kind == ResultItem::Kind::Reference) {
                lastWritten = lastWritten + patternCharacters(captureNamed(info->rule, lastItem.capture)->getCapturedRE());
            } else if (info->items.size() == 1) {
                // A call on a capture of the whole text of one character: the
                // characters its value deletes.
                if ((range.first == 1) && (range.second == 1)) {
                    deletedOwn = deletedOwn + (deletedBy[callOf[lastItem.function]] & textChars);
                } else {
                    problem("a rule of a shape with before contexts deleting part of its text: " + printRule(info->rule));
                }
            }
            // (A call ending a longer result writes its value, or deletes its
            // argument after the rest of the result, which is then last.)
            if ((lastItem.kind == ResultItem::Kind::Call) && (info->items.size() > 1)) {
                const ResultItem & before = info->items[info->items.size() - 2];
                if (before.kind == ResultItem::Kind::Literal) lastWritten.insert(before.literal);
                else lastWritten = lastWritten + patternCharacters(captureNamed(info->rule, before.capture)->getCapturedRE());
            }
        }
        std::vector<re::RE *> befores;
        std::vector<const ConversionRule *> asApplied;
        for (const ConversionRule * r : c.rules) {
            re::RE * const before = r->getLeftSide()->getBeforeContext();
            if (before == nullptr) {
                befores.push_back(nullptr);
                asApplied.push_back(r);
                continue;
            }
            if (!CharSetAnalysis::isSet(before)) {
                problem("a before context of more than one set, matched against the converted text: " + printRule(r));
                befores.push_back(nullptr);
                continue;
            }
            const UCD::UnicodeSet P = sets.setOf(before, true);
            if (P.intersects(lastWritten) || P.intersects(marks)) {
                problem("a before context that may match a character written by the conversions: " + printRule(r));
            }
            // P (?: M [E] | [E'])*
            std::vector<re::RE *> skips;
            for (size_t k = 0; k < plan.calls.size(); k++) {
                if (deletedBy[k].empty()) continue;
                skips.push_back(re::makeSeq({re::makeCC(plan.calls[k].mark, &cc::Unicode), re::makeCC(deletedBy[k], &cc::Unicode)}));
            }
            if (!deletedOwn.empty()) skips.push_back(re::makeCC(deletedOwn, &cc::Unicode));
            re::RE * context = plainSets(possessiveContext(engineContext(before, false), false), sets);
            if (!skips.empty()) {
                context = re::makeSeq({context, re::makeRep(re::makeAlt(skips.begin(), skips.end()), 0, re::Rep::UNBOUNDED_REP)});
            }
            befores.push_back(context);
            asApplied.push_back(makeConversionRule(makeRuleSide(context, r->getLeftSide()->getText(), nullptr),
                                                   Direction::Forward, r->getRightSide()));
        }
        // Its matches may not contain the start of a match of an earlier pass,
        // nor match the text the earlier passes write.
        for (const ConversionRule * b : asApplied) {
            for (size_t x : plain) {
                for (const RuleInfo * a : shapeInfos[x]) {
                    const ConversionRule * const w = written(*a);
                    if (overlaps.mayMatchWithin(a->rule, b)) {
                        problem("a match of an earlier pass may begin within " + printRule(b));
                    }
                    if (overlaps.mayOverlap(b, w) || overlaps.mayMatchWithin(b, w) || overlaps.mayMatchWithin(w, b)) {
                        problem("a shape with before contexts that may match the text written by an earlier pass: " +
                                printRule(b) + "  in the text of  " + printRule(a->rule));
                    }
                }
            }
        }
        if (outOfBudget()) return plan;
        c.search = searchOf(contextShape, befores);
        order.push_back(contextShape);
    }

    // The passes, in order, with the yields as indices of passes.
    std::vector<size_t> passOf(S);
    for (size_t i = 0; i < order.size(); i++) passOf[order[i]] = i;
    for (size_t x : order) {
        ShapePass p = shapes[x];
        p.guards.clear();
        for (size_t y : yieldsTo[x]) p.guards.push_back(passOf[y]);
        plan.passes.push_back(std::move(p));
    }
    return plan;
}

std::vector<ShapeGroupPlan> planShapeGroups(const std::vector<Rule *> & rules, const FunctionEvaluator * evaluate) {
    std::vector<ShapeGroupPlan> plans;
    std::vector<const ConversionRule *> group;
    auto flush = [&] {
        if (!group.empty()) plans.push_back(planShapeGroup(group, evaluate));
        group.clear();
    };
    for (const Rule * r : rules) {
        if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            group.push_back(c);
        } else if (!isa<VariableDefinitionRule>(r)) {
            flush();
        }
    }
    flush();
    return plans;
}

//
// Printing in the syntax of the regular expression parser.
//

namespace {

std::string escaped(UCD::codepoint_t cp) {
    if (((cp >= '0') && (cp <= '9')) || ((cp >= 'A') && (cp <= 'Z')) || ((cp >= 'a') && (cp <= 'z'))) {
        return std::string(1, static_cast<char>(cp));
    }
    return "\\x{" + hex(cp) + "}";
}

struct SyntaxPrinter {
    std::string reason;

    // A set as a bracket expression.
    bool set(const re::RE * r, std::string & out) {
        if (const re::CC * const cc = dyn_cast<re::CC>(r)) {
            if (cc->getAlphabet() != &cc::Unicode) return fail("a class of another alphabet");
            if (cc->empty()) {
                out += "[\\x{0}--\\x{0}]";
                return true;
            }
            out += "[";
            for (const auto & range : *cc) {
                out += escaped(range.first);
                if (range.second != range.first) out += "-" + escaped(range.second);
            }
            out += "]";
            return true;
        } else if (const re::PropertyExpression * const pe = dyn_cast<re::PropertyExpression>(r)) {
            if (pe->getKind() != re::PropertyExpression::Kind::Codepoint) return fail("a boundary property");
            const bool neg = pe->getOperator() == re::PropertyExpression::Operator::NEq;
            out += std::string(neg ? "\\P{" : "\\p{") + pe->getPropertyIdentifier() +
                   (pe->getValueString().empty() ? "" : "=" + pe->getValueString()) + "}";
            return true;
        } else if (isa<re::Any>(r)) {
            out += "[\\x{0}-\\x{10FFFF}]";
            return true;
        } else if (const re::Diff * const d = dyn_cast<re::Diff>(r)) {
            out += "[";
            if (!set(d->getLH(), out)) return false;
            out += "--";
            if (!set(d->getRH(), out)) return false;
            out += "]";
            return true;
        } else if (const re::Intersect * const x = dyn_cast<re::Intersect>(r)) {
            out += "[";
            if (!set(x->getLH(), out)) return false;
            out += "&&";
            if (!set(x->getRH(), out)) return false;
            out += "]";
            return true;
        } else if (const re::Alt * const alt = dyn_cast<re::Alt>(r)) {
            out += "[";
            for (const re::RE * e : *alt) {
                if (!set(e, out)) return false;
            }
            out += "]";
            return true;
        } else if (const re::Name * const n = dyn_cast<re::Name>(r)) {
            if (n->getDefinition()) return set(n->getDefinition(), out);
        }
        return fail("not a set");
    }

    bool fail(const std::string & why) {
        if (reason.empty()) reason = why;
        return false;
    }

    static bool isSetItem(const re::RE * r) {
        if (isa<re::CC>(r) || isa<re::PropertyExpression>(r) || isa<re::Any>(r) || isa<re::Diff>(r) || isa<re::Intersect>(r)) {
            return true;
        }
        if (const re::Name * const n = dyn_cast<re::Name>(r)) {
            return !isFunctionCall(n) && n->getDefinition() && isSetItem(n->getDefinition());
        }
        if (const re::Alt * const alt = dyn_cast<re::Alt>(r)) {
            if (alt->empty()) return false;
            for (const re::RE * e : *alt) if (!isSetItem(e)) return false;
            return true;
        }
        return false;
    }

    static bool hasCaptures(re::RE * r) {
        return !re::capturesByName(r).empty();
    }

    // An RE; atom: whether it is a single item (to be repeated as it is).
    bool print(re::RE * r, std::string & out, bool & atom) {
        atom = true;
        if (isSetItem(r)) {
            const re::CC * const cc = dyn_cast<re::CC>(r);
            if (cc && !cc->empty() && (cc->min_codepoint() == cc->max_codepoint())) {
                out += escaped(cc->min_codepoint());
                return true;
            }
            return set(r, out);
        } else if (re::Seq * const seq = dyn_cast<re::Seq>(r)) {
            if (seq->size() == 1) return print(seq->front(), out, atom);
            atom = false;
            for (re::RE * e : *seq) {
                bool a;
                if (!item(e, out, a, true)) return false;
            }
            return true;
        } else if (re::String * const s = dyn_cast<re::String>(r)) {
            for (const char32_t c : *s) out += escaped(c);
            atom = s->size() == 1;
            return true;
        } else if (re::Alt * const alt = dyn_cast<re::Alt>(r)) {
            if (alt->size() == 1) return print(alt->front(), out, atom);
            if (alt->empty()) {
                out += "(?!)";
                return true;
            }
            out += hasCaptures(r) ? "(?|" : "(?:";
            bool first = true;
            for (re::RE * e : *alt) {
                if (!first) out += "|";
                first = false;
                bool a;
                if (!print(e, out, a)) return false;
            }
            out += ")";
            return true;
        } else if (re::Rep * const rep = dyn_cast<re::Rep>(r)) {
            bool a;
            if (!item(rep->getRE(), out, a, false)) return false;
            const int lb = rep->getLB(), ub = rep->getUB();
            if ((lb == 0) && (ub == re::Rep::UNBOUNDED_REP)) out += "*";
            else if ((lb == 1) && (ub == re::Rep::UNBOUNDED_REP)) out += "+";
            else if ((lb == 0) && (ub == 1)) out += "?";
            else if (ub == re::Rep::UNBOUNDED_REP) out += "{" + std::to_string(lb) + ",}";
            else if (lb == ub) out += "{" + std::to_string(lb) + "}";
            else out += "{" + std::to_string(lb) + "," + std::to_string(ub) + "}";
            if (rep->getKind() == re::Rep::Kind::Possessive) out += "+";
            return true;
        } else if (re::Capture * const c = dyn_cast<re::Capture>(r)) {
            out += "(";
            bool a;
            if (!print(c->getCapturedRE(), out, a)) return false;
            out += ")";
            return true;
        } else if (re::Assertion * const a = dyn_cast<re::Assertion>(r)) {
            const bool behind = a->getKind() == re::Assertion::Kind::LookBehind;
            const bool positive = a->getSense() == re::Assertion::Sense::Positive;
            if (!behind && (a->getKind() != re::Assertion::Kind::LookAhead)) return fail("a boundary assertion");
            out += std::string("(?") + (behind ? "<" : "") + (positive ? "=" : "!");
            bool x;
            if (!print(a->getAsserted(), out, x)) return false;
            out += ")";
            return true;
        } else if (isa<re::Start>(r)) {
            out += "^";
            return true;
        } else if (isa<re::End>(r)) {
            out += "$";
            return true;
        } else if (re::Name * const n = dyn_cast<re::Name>(r)) {
            if (!isFunctionCall(n) && n->getDefinition()) return print(n->getDefinition(), out, atom);
            return fail("a name without definition: " + n->getFullName());
        }
        return fail("a construct without syntax");
    }

    // An item of a sequence (an alternation grouped), or the body of a
    // repetition (grouped unless an atom).
    bool item(re::RE * r, std::string & out, bool & atom, bool inSeq) {
        std::string s;
        if (!print(r, s, atom)) return false;
        const bool group = inSeq ? false : !atom;
        out += group ? "(?:" + s + ")" : s;
        atom = true;
        return true;
    }
};

}

std::string regexSyntax(re::RE * re, std::string * reason) {
    SyntaxPrinter p;
    std::string out;
    bool atom;
    if (!p.print(re, out, atom)) {
        if (reason) *reason = p.reason;
        return "";
    }
    return out;
}

std::string replacementSyntax(re::RE * replacement) {
    std::vector<ResultItem> items;
    std::string out;
    std::vector<re::RE *> parts;
    if (re::Seq * const seq = dyn_cast<re::Seq>(replacement)) parts.assign(seq->begin(), seq->end());
    else parts.push_back(replacement);
    for (re::RE * r : parts) {
        if (re::Reference * const ref = dyn_cast<re::Reference>(r)) {
            out += "${" + ref->getName() + "}";
        } else if (const re::CC * const cc = dyn_cast<re::CC>(r)) {
            if (cc->empty() || (cc->min_codepoint() != cc->max_codepoint())) return "";
            out += escaped(cc->min_codepoint());
        } else if (re::String * const s = dyn_cast<re::String>(r)) {
            for (const char32_t c : *s) out += escaped(c);
        } else {
            return "";
        }
    }
    return out;
}

}

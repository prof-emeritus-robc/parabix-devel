/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_plan.h>
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include "charset_analysis.h"

using namespace llvm;

namespace ldml {

//  Does a set have strings (e.g. [{ch}]), or is it something other than a set
//  of single characters?
static bool hasStrings(const re::RE * re) {
    if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        return seq->size() != 1 || hasStrings(seq->front());
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * a : *alt) {
            if (hasStrings(a)) return true;
        }
        return false;
    } else if (const re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return true;
        return hasStrings(name->getDefinition());
    } else if (const re::Diff * diff = dyn_cast<re::Diff>(re)) {
        return hasStrings(diff->getLH()) || hasStrings(diff->getRH());
    } else if (const re::Intersect * x = dyn_cast<re::Intersect>(re)) {
        return hasStrings(x->getLH()) || hasStrings(x->getRH());
    }
    return isa<re::Capture>(re) || isa<re::Reference>(re) || isa<re::Rep>(re);
}

//  The fixed string of a result (characters, or variables defined as strings).
static bool fixedString(const re::RE * re, std::u32string & out) {
    if (const re::CC * cc = dyn_cast<re::CC>(re)) {
        if (cc->count() != 1) return false;
        out.push_back(static_cast<char32_t>(cc->min_codepoint()));
        return true;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) {
            if (!fixedString(e, out)) return false;
        }
        return true;
    } else if (const re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return false;
        return fixedString(name->getDefinition(), out);
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        return alt->size() == 1 && fixedString(alt->front(), out);
    }
    return false;
}

//  The items of a text to replace, through sequences, variables defined as
//  strings and captures: e.g., a $v [bc] with $v = xy is a, x, y, [bc].
static void textItems(re::RE * re, std::vector<re::RE *> & items) {
    if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (re::RE * e : *seq) textItems(e, items);
    } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
        textItems(c->getCapturedRE(), items);
    } else if (re::Alt * alt = dyn_cast<re::Alt>(re); alt && alt->size() == 1) {
        textItems(alt->front(), items);
    } else if (re::Name * name = dyn_cast<re::Name>(re);
               name && !isFunctionCall(name) && name->getDefinition() && !CharSetAnalysis::isSet(name->getDefinition())) {
        textItems(name->getDefinition(), items);
    } else {
        items.push_back(re);
    }
}

static bool isEmptyText(const re::RE * re) {
    const re::Seq * seq = dyn_cast_or_null<re::Seq>(re);
    return re == nullptr || (seq && seq->empty());
}

UCD::UnicodeSet CharMapSubgroup::characters() const {
    UCD::UnicodeSet all;
    for (const CharMapping & m : mappings) all = all + m.chars;
    return all;
}

TransformPlan planTransform(const std::vector<Rule *> & rules) {
    TransformPlan plan;
    CharSetAnalysis sets;
    TransformStep * group = nullptr;
    for (Rule * r : rules) {
        if (FilterRule * f = dyn_cast<FilterRule>(r)) {
            plan.filter = f->getFilterSet();
            plan.filterSet = sets.setOf(f->getFilterSet(), false);
        } else if (TransformRule * t = dyn_cast<TransformRule>(r)) {
            group = nullptr;
            TransformStep step;
            step.kind = TransformStep::Kind::Transform;
            step.transform = t->getForwardID();
            step.filter = t->getForwardFilter();
            if (step.filter) step.filterSet = sets.setOf(step.filter, false);
            plan.steps.push_back(std::move(step));
        } else if (ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            const RuleSide * source = c->getSourceSide(Direction::Forward);
            const RuleSide * result = c->getResultSide(Direction::Forward);
            re::RE * const text = source->getText();
            std::vector<re::RE *> items;
            textItems(text, items);
            std::string problem;
            std::u32string replacement;
            bool singleChars = !items.empty();
            for (re::RE * item : items) singleChars &= CharSetAnalysis::isSet(item) && !hasStrings(item);
            bool boundary = false;
            for (re::RE * item : items) boundary |= mayIncludeTextBoundary(item);
            if (!singleChars) {
                problem = "the text to replace is not a fixed-length sequence of characters";
            } else if (boundary) {
                problem = "the text to replace includes the text boundary";
            } else if (result->hasCursor() && (!isEmptyText(result->getResultToRevisit()) || result->getCursorOffset() != 0)) {
                problem = "the result has text to revisit";
            } else if (!fixedString(result->getText(), replacement)) {
                problem = "the result is not a fixed string";
            }
            if (!problem.empty()) {
                plan.problems.push_back(printRule(r) + "  (" + problem + ")");
                continue;
            }
            if (group == nullptr) {
                TransformStep step;
                step.kind = TransformStep::Kind::Conversion;
                plan.steps.push_back(std::move(step));
                group = &plan.steps.back();
            }
            re::RE * const before = source->getBeforeContext();
            re::RE * const after = source->getAfterContext();
            group->rules.push_back(c);
            if (items.size() > 1) {
                StringRule s;
                for (re::RE * item : items) s.text.push_back(sets.setOf(item, false));
                s.replacement = replacement;
                s.before = before;
                s.after = after;
                s.engineBefore = before ? engineContext(before, false) : nullptr;
                s.engineAfter = after ? engineContext(after, true) : nullptr;
                s.rule = c;
                group->stringRules.push_back(std::move(s));
                continue;
            }
            const std::string key = (before ? printPattern(before) : "") + " { } " + (after ? printPattern(after) : "");
            CharMapSubgroup * subgroup = nullptr;
            for (CharMapSubgroup & s : group->subgroups) {
                if (s.contextKey == key) {
                    subgroup = &s;
                    break;
                }
            }
            if (subgroup == nullptr) {
                group->subgroups.emplace_back();
                subgroup = &group->subgroups.back();
                subgroup->before = before;
                subgroup->after = after;
                subgroup->engineBefore = before ? engineContext(before, false) : nullptr;
                subgroup->engineAfter = after ? engineContext(after, true) : nullptr;
                subgroup->contextKey = key;
            }
            subgroup->rules++;
            //  The earliest rule for a character takes precedence.
            const UCD::UnicodeSet chars = sets.setOf(items.front(), false) - subgroup->characters();
            if (chars.empty()) continue;
            CharMapping * same = nullptr;
            for (CharMapping & m : subgroup->mappings) {
                if (m.replacement == replacement) same = &m;
            }
            if (same) {
                same->chars = same->chars + chars;
            } else {
                subgroup->mappings.push_back(CharMapping{chars, replacement});
            }
        }
    }
    //  As the rules of a group are applied at all positions at once, a rule may
    //  match within the text of a string rule (where ICU, having replaced the
    //  text, does not apply rules).  A single-character rule matching there is
    //  kept from applying by the coverage of the string rules (see
    //  TransformStep::coverStringRules).  String rules matching within the texts
    //  of string rules are supported if the string rules of the group can be
    //  applied in a single pass (see StringRuleOverlaps): in a topological order,
    //  each applying where it does not begin within an applied match of a rule
    //  preceding it (TransformStep::stringOrder), and a rule of two characters
    //  matching within its own text by the selection of its matches (see
    //  StringRule::selfOverlapping).  Otherwise, the matches that apply are
    //  selected sequentially (TransformStep::sequentialSelection).
    //  A quick check first: a rule may match within the text only if its first
    //  characters may occur at a position of the text other than the first.
    for (TransformStep & step : plan.steps) {
        if (step.stringRules.empty()) continue;
        std::vector<UCD::UnicodeSet> firstChars;
        std::vector<bool> single;
        for (const ConversionRule * r : step.rules) {
            std::vector<re::RE *> items;
            textItems(r->getSourceSide(Direction::Forward)->getText(), items);
            firstChars.push_back(items.empty() ? UCD::UnicodeSet() : sets.setOf(items.front(), false));
            single.push_back(items.size() == 1);
        }
        RuleOverlapAnalysis analysis;
        for (const StringRule & s : step.stringRules) {
            if (step.coverStringRules) break;
            UCD::UnicodeSet within;
            for (size_t k = 1; k < s.text.size(); k++) within = within + s.text[k];
            for (size_t i = 0; i < step.rules.size(); i++) {
                if (!single[i] || (firstChars[i] & within).empty()) continue;
                if (analysis.mayMatchWithin(step.rules[i], s.rule)) {
                    step.coverStringRules = true;
                    break;
                }
            }
        }
        //  String rules within the texts of string rules.
        const StringRuleOverlaps overlaps = analyzeStringRuleOverlaps(step);
        if (overlaps.pairs.empty()) continue;
        if (overlaps.singlePass()) {
            step.stringOrder = overlaps.order;
            step.stringPredecessors = overlaps.predecessors;
            for (const StringRuleOverlaps::Pair & p : overlaps.pairs) {
                if (p.kind == StringRuleOverlaps::Kind::Self) step.stringRules[p.outer].selfOverlapping = true;
            }
            continue;
        }
        //  Otherwise, the matches are selected sequentially.
        step.sequentialSelection = true;
        step.stringOrder.clear();
        for (size_t r = 0; r < step.stringRules.size(); r++) step.stringOrder.push_back(r);
        step.stringPredecessors.assign(step.stringRules.size(), {});
    }
    return plan;
}

StringRuleOverlaps analyzeStringRuleOverlaps(const TransformStep & step) {
    StringRuleOverlaps result;
    const size_t n = step.stringRules.size();
    //  within[a][b]: rule b may match within the text of rule a.  Quick checks
    //  first, of the texts without the contexts (which only restrict matches):
    //  the first characters of b must occur at a position of the text of a other
    //  than the first, and, for some position k of the text of a other than the
    //  first, each character of the text of b up to the end of the text of a
    //  must be one that may occur at its position in the text of a.
    std::vector<std::vector<bool>> within(n, std::vector<bool>(n, false));
    RuleOverlapAnalysis analysis;
    auto textsOverlap = [](const std::vector<UCD::UnicodeSet> & outer, const std::vector<UCD::UnicodeSet> & inner) {
        for (size_t k = 1; k < outer.size(); k++) {
            bool overlap = true;
            for (size_t i = 0; overlap && i < inner.size() && k + i < outer.size(); i++) {
                overlap = outer[k + i].intersects(inner[i]);
            }
            if (overlap) return true;
        }
        return false;
    };
    for (size_t a = 0; a < n; a++) {
        const StringRule & outer = step.stringRules[a];
        UCD::UnicodeSet inside;
        for (size_t k = 1; k < outer.text.size(); k++) inside = inside + outer.text[k];
        for (size_t b = 0; b < n; b++) {
            const StringRule & inner = step.stringRules[b];
            if (!inner.text.front().intersects(inside) || !textsOverlap(outer.text, inner.text)) continue;
            within[a][b] = analysis.mayMatchWithin(inner.rule, outer.rule);
        }
    }
    //  The pairs, and the precedence graph of the one-way pairs and self-overlaps.
    std::vector<std::vector<size_t>> precedes(n);
    for (size_t a = 0; a < n; a++) {
        if (within[a][a]) {
            result.pairs.push_back({a, a, StringRuleOverlaps::Kind::Self});
            result.self++;
            if (step.stringRules[a].text.size() != 2) result.selfSelectable = false;
            precedes[a].push_back(a);
        }
        for (size_t b = 0; b < n; b++) {
            if (b == a || !within[a][b]) continue;
            if (within[b][a]) {
                if (a < b) {
                    result.pairs.push_back({a, b, StringRuleOverlaps::Kind::TwoWay});
                    result.twoWay++;
                }
            } else {
                result.pairs.push_back({a, b, StringRuleOverlaps::Kind::OneWay});
                result.oneWay++;
                precedes[a].push_back(b);
            }
        }
    }
    //  A depth-first search of the graph (with or without the self-overlaps):
    //  a cycle (an edge to a rule on the path), or the number of rules in the
    //  longest chain (0 without edges).
    std::vector<size_t> postorder;
    auto search = [&](bool withSelf, std::vector<size_t> & cycle) -> std::pair<bool, unsigned> {
        enum : unsigned char {Unvisited, OnPath, Done};
        std::vector<unsigned char> state(n, Unvisited);
        std::vector<unsigned> chain(n, 1);
        std::vector<size_t> path;
        std::vector<std::pair<size_t, size_t>> stack;     // (rule, next edge)
        unsigned stages = 0;
        for (size_t root = 0; root < n; root++) {
            if (state[root] != Unvisited) continue;
            stack.emplace_back(root, 0);
            state[root] = OnPath;
            path.push_back(root);
            while (!stack.empty()) {
                auto & top = stack.back();
                const size_t v = top.first;
                if (top.second < precedes[v].size()) {
                    const size_t w = precedes[v][top.second++];
                    if (w == v && !withSelf) continue;
                    if (state[w] == OnPath) {
                        cycle.assign(std::find(path.begin(), path.end(), w), path.end());
                        return {false, 0};
                    } else if (state[w] == Unvisited) {
                        state[w] = OnPath;
                        path.push_back(w);
                        stack.emplace_back(w, 0);
                    }
                } else {
                    bool edges = false;
                    for (size_t w : precedes[v]) {
                        if (w == v) continue;
                        edges = true;
                        chain[v] = std::max(chain[v], chain[w] + 1);
                    }
                    if (edges) stages = std::max(stages, chain[v]);
                    if (!withSelf) postorder.push_back(v);
                    state[v] = Done;
                    path.pop_back();
                    stack.pop_back();
                }
            }
        }
        return {true, stages};
    };
    std::tie(result.acyclic, result.stages) = search(true, result.cycle);
    std::vector<size_t> ignored;
    std::tie(result.acyclicApartFromSelf, result.stagesApartFromSelf) = search(false, ignored);
    //  The strongly connected components of the graph of all the overlaps
    //  (Tarjan's algorithm, iteratively).
    {
        std::vector<std::vector<size_t>> edges(n);
        for (size_t a = 0; a < n; a++) {
            for (size_t b = 0; b < n; b++) {
                if (within[a][b]) edges[a].push_back(b);
            }
        }
        const size_t none = SIZE_MAX;
        std::vector<size_t> index(n, none), low(n, 0);
        std::vector<bool> onStack(n, false);
        std::vector<size_t> stack;
        std::vector<std::pair<size_t, size_t>> work;
        size_t next = 0;
        for (size_t root = 0; root < n; root++) {
            if (index[root] != none) continue;
            work.emplace_back(root, 0);
            while (!work.empty()) {
                const size_t v = work.back().first;
                size_t & e = work.back().second;
                if (e == 0 && index[v] == none) {
                    index[v] = low[v] = next++;
                    stack.push_back(v);
                    onStack[v] = true;
                }
                if (e < edges[v].size()) {
                    const size_t w = edges[v][e++];
                    if (index[w] == none) {
                        work.emplace_back(w, 0);
                    } else if (onStack[w]) {
                        low[v] = std::min(low[v], index[w]);
                    }
                    continue;
                }
                if (low[v] == index[v]) {
                    std::vector<size_t> component;
                    size_t w;
                    do {
                        w = stack.back();
                        stack.pop_back();
                        onStack[w] = false;
                        component.push_back(w);
                    } while (w != v);
                    if (component.size() > 1 || within[v][v]) {
                        result.cyclicComponents++;
                        result.largestComponent = std::max(result.largestComponent, component.size());
                        bool pairs = true;
                        for (size_t r : component) pairs &= step.stringRules[r].text.size() == 2;
                        if (pairs) result.pairComponents++; else result.pairSelectable = false;
                    }
                }
                work.pop_back();
                if (!work.empty()) {
                    const size_t u = work.back().first;
                    low[u] = std::min(low[u], low[v]);
                }
            }
        }
    }
    if (result.acyclicApartFromSelf) {
        //  Each rule follows the rules it is reached from.
        result.order.assign(postorder.rbegin(), postorder.rend());
        result.predecessors.resize(n);
        for (size_t a = 0; a < n; a++) {
            for (size_t b : precedes[a]) {
                if (b != a) result.predecessors[b].push_back(a);
            }
        }
    }
    return result;
}

static re::RE * expandContext(re::RE * re, const std::map<unsigned, UCD::UnicodeSet> & insertions,
                              UCD::codepoint_t filler, CharSetAnalysis & sets) {
    if (isa<re::CC>(re) || isa<re::PropertyExpression>(re) || isa<re::Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)) {
        UCD::UnicodeSet chars = sets.setOf(re, false);
        chars = chars - UCD::UnicodeSet(filler);
        //  The characters by the number of filler codepoints following them.
        std::vector<re::RE *> alts;
        UCD::UnicodeSet plain = chars;
        for (const auto & k : insertions) {
            if (k.first == 0) continue;
            const UCD::UnicodeSet followed = chars & k.second;
            if (followed.empty()) continue;
            plain = plain - followed;
            std::vector<re::RE *> seq{re::makeCC(followed, &cc::Unicode)};
            for (unsigned i = 0; i < k.first; i++) seq.push_back(re::makeCC(filler, &cc::Unicode));
            alts.push_back(re::makeSeq(seq.begin(), seq.end()));
        }
        alts.insert(alts.begin(), re::makeCC(plain, &cc::Unicode));
        return alts.size() == 1 ? alts[0] : re::makeAlt(alts.begin(), alts.end());
    } else if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
        std::vector<re::RE *> items;
        for (re::RE * e : *seq) items.push_back(expandContext(e, insertions, filler, sets));
        return re::makeSeq(items.begin(), items.end());
    } else if (re::Alt * alt = dyn_cast<re::Alt>(re)) {
        std::vector<re::RE *> items;
        for (re::RE * a : *alt) items.push_back(expandContext(a, insertions, filler, sets));
        return re::makeAlt(items.begin(), items.end());
    } else if (re::Rep * rep = dyn_cast<re::Rep>(re)) {
        return re::makeRep(expandContext(rep->getRE(), insertions, filler, sets), rep->getLB(), rep->getUB());
    } else if (re::Assertion * a = dyn_cast<re::Assertion>(re)) {
        return re::makeAssertion(expandContext(a->getAsserted(), insertions, filler, sets), a->getKind(), a->getSense());
    } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
        return expandContext(c->getCapturedRE(), insertions, filler, sets);
    } else if (re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return re;
        return expandContext(name->getDefinition(), insertions, filler, sets);
    }
    return re;
}

re::RE * expandedContext(re::RE * context, const std::map<unsigned, UCD::UnicodeSet> & insertions,
                         UCD::codepoint_t filler) {
    CharSetAnalysis sets;
    return expandContext(context, insertions, filler, sets);
}

static void collectItems(re::RE * re, bool repeated, std::vector<PatternItem> & items, CharSetAnalysis & sets) {
    if (isa<re::CC>(re) || isa<re::PropertyExpression>(re) || isa<re::Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)
            || ((isa<re::Alt>(re) || isa<re::Name>(re)) && CharSetAnalysis::isSet(re))) {
        items.push_back(PatternItem{sets.setOf(re, true), repeated});
    } else if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (re::RE * e : *seq) collectItems(e, repeated, items, sets);
    } else if (re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (re::RE * a : *alt) collectItems(a, repeated, items, sets);
    } else if (re::Rep * rep = dyn_cast<re::Rep>(re)) {
        collectItems(rep->getRE(), true, items, sets);
    } else if (re::Assertion * a = dyn_cast<re::Assertion>(re)) {
        collectItems(a->getAsserted(), repeated, items, sets);
    } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
        collectItems(c->getCapturedRE(), repeated, items, sets);
    } else if (re::Name * name = dyn_cast<re::Name>(re)) {
        if (!isFunctionCall(name) && name->getDefinition()) collectItems(name->getDefinition(), repeated, items, sets);
    }
}

std::vector<PatternItem> patternItems(re::RE * pattern) {
    CharSetAnalysis sets;
    std::vector<PatternItem> items;
    collectItems(pattern, false, items, sets);
    return items;
}

UCD::UnicodeSet patternCharacters(re::RE * pattern) {
    UCD::UnicodeSet chars;
    for (const PatternItem & item : patternItems(pattern)) chars = chars + item.chars;
    return chars;
}

}

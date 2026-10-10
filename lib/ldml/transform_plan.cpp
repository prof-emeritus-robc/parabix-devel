/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_plan.h>
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
            const UCD::UnicodeSet chars = sets.setOf(text, false) - subgroup->characters();
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
    //  As the rules of a group are applied at all positions at once, no rule may
    //  match within the text of a string rule (where ICU, having replaced the
    //  text, does not apply rules).
    //  A quick check first: a rule may match within the text only if its first
    //  characters may occur at a position of the text other than the first.  The
    //  first rule found that may match within a text is reported.
    for (const TransformStep & step : plan.steps) {
        if (step.stringRules.empty()) continue;
        std::vector<UCD::UnicodeSet> firstChars;
        for (const ConversionRule * r : step.rules) {
            std::vector<re::RE *> items;
            textItems(r->getSourceSide(Direction::Forward)->getText(), items);
            firstChars.push_back(items.empty() ? UCD::UnicodeSet() : sets.setOf(items.front(), false));
        }
        RuleOverlapAnalysis analysis;
        for (const StringRule & s : step.stringRules) {
            UCD::UnicodeSet within;
            for (size_t k = 1; k < s.text.size(); k++) within = within + s.text[k];
            for (size_t i = 0; i < step.rules.size(); i++) {
                const ConversionRule * const r = step.rules[i];
                if ((firstChars[i] & within).empty()) continue;
                if (analysis.mayMatchWithin(r, s.rule)) {
                    plan.problems.push_back(printRule(r) + "  (may match within the text of " + printRule(s.rule) + ")");
                    break;
                }
            }
        }
    }
    return plan;
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

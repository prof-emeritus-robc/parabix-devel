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

static bool isEmptyText(const re::RE * re) {
    const re::Seq * seq = dyn_cast_or_null<re::Seq>(re);
    return re == nullptr || (seq && seq->empty());
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
            std::string problem;
            std::u32string replacement;
            if (!CharSetAnalysis::isSet(text) || hasStrings(text)) {
                problem = "the text to replace is not a single character";
            } else if (mayIncludeTextBoundary(text)) {
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
            for (const auto & range : sets.setOf(text, false)) {
                for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) {
                    subgroup->charMap.emplace(cp, replacement);   // the earliest rule takes precedence
                }
            }
        }
    }
    return plan;
}

}

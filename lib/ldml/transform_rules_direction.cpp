/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules.h>
#include <re/adt/adt.h>
#include <algorithm>
#include <set>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

// A side matched against the input: contexts, but no cursor.
RuleSide * sourceSide(const RuleSide * s) {
    return RuleSide::Create(s->getBeforeContext(), s->getText(), false, nullptr, 0, s->getAfterContext());
}

// A side giving the result: cursor, but no contexts.
RuleSide * resultSide(const RuleSide * s) {
    return RuleSide::Create(nullptr, s->getCompletedResult(), s->hasCursor(), s->getResultToRevisit(), s->getCursorOffset(), nullptr);
}

//  The extracted rules, other than the filter rule, as a list of units.
//  A group unit holds consecutive conversion rules (and any variable
//  definitions among them); a transform unit holds one transform rule;
//  a separator unit marks a transform rule not applying in the direction.
struct Unit {
    enum class Kind {Group, Transform, Separator} kind;
    std::vector<Rule *> rules;
    bool hasConversion;
};

class UnitList {
public:
    void addToGroup(Rule * r) {
        if (mUnits.empty() || mUnits.back().kind != Unit::Kind::Group) {
            mUnits.push_back(Unit{Unit::Kind::Group, {}, false});
        }
        mUnits.back().rules.push_back(r);
        mUnits.back().hasConversion |= isa<ConversionRule>(r);
    }
    void addTransform(Rule * r) {
        mUnits.push_back(Unit{Unit::Kind::Transform, {r}, false});
    }
    void addSeparator() {
        mUnits.push_back(Unit{Unit::Kind::Separator, {}, false});
    }
    void reverse() {
        std::reverse(mUnits.begin(), mUnits.end());
    }
    void emit(std::vector<Rule *> & result) const;
private:
    // Is there a conversion rule after unit i and before the next transform?
    bool conversionFollows(size_t i) const {
        for (size_t j = i + 1; j < mUnits.size(); j++) {
            if (mUnits[j].kind == Unit::Kind::Transform) return false;
            if (mUnits[j].hasConversion) return true;
        }
        return false;
    }
    std::vector<Unit> mUnits;
};

void UnitList::emit(std::vector<Rule *> & result) const {
    // Whether conversion rules have been emitted since the last transform rule.
    bool pendingConversions = false;
    for (size_t i = 0; i < mUnits.size(); i++) {
        const Unit & u = mUnits[i];
        switch (u.kind) {
            case Unit::Kind::Group:
                result.insert(result.end(), u.rules.begin(), u.rules.end());
                pendingConversions |= u.hasConversion;
                break;
            case Unit::Kind::Transform:
                result.push_back(u.rules[0]);
                pendingConversions = false;
                break;
            case Unit::Kind::Separator:
                if (pendingConversions && conversionFollows(i)) {
                    result.push_back(makeTransformRule(TransformID("Null")));
                    pendingConversions = false;
                }
                break;
        }
    }
}

//  Collect the variables used in a pattern, including the variables used
//  in the definitions of used variables.
class VariableUseCollector {
public:
    VariableUseCollector(const std::set<const Name *> & variables) : mVariables(variables) {}
    void collect(const RE * re);
    void collect(const RuleSide * side) {
        if (side->hasBeforeContext()) collect(side->getBeforeContext());
        collect(side->getCompletedResult());
        collect(side->getResultToRevisit());
        if (side->hasAfterContext()) collect(side->getAfterContext());
    }
    bool isUsed(const Name * variable) const {return mUsed.count(variable) != 0;}
private:
    const std::set<const Name *> & mVariables;
    std::set<const Name *> mUsed;
};

void VariableUseCollector::collect(const RE * re) {
    if (re == nullptr) return;
    if (const Name * n = dyn_cast<Name>(re)) {
        // A use of a resolved copy of a variable is a use of the variable.
        const Name * const variable = originalVariable(n);
        if (mVariables.count(variable) != 0) {
            if (!mUsed.insert(variable).second && variable == n) return;  // already collected
        }
        // A variable definition, or the argument of a function call.
        collect(n->getDefinition());
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collect(e);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) collect(e);
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        collect(rep->getRE());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        collect(d->getLH());
        collect(d->getRH());
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        collect(x->getLH());
        collect(x->getRH());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        collect(c->getCapturedRE());
    }
}

//  Remove the definitions of variables not used in any other rule.
std::vector<Rule *> removeUnusedVariables(const std::vector<Rule *> & rules) {
    std::set<const Name *> variables;
    for (const Rule * r : rules) {
        if (const VariableDefinitionRule * v = dyn_cast<VariableDefinitionRule>(r)) {
            variables.insert(v->getVariable());
        }
    }
    if (variables.empty()) return rules;
    VariableUseCollector uses(variables);
    for (const Rule * r : rules) {
        if (const FilterRule * f = dyn_cast<FilterRule>(r)) {
            uses.collect(f->getFilterSet());
        } else if (const TransformRule * t = dyn_cast<TransformRule>(r)) {
            uses.collect(t->getForwardFilter());
            uses.collect(t->getBackwardFilter());
        } else if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            uses.collect(c->getLeftSide());
            uses.collect(c->getRightSide());
        }
    }
    std::vector<Rule *> result;
    for (Rule * r : rules) {
        if (const VariableDefinitionRule * v = dyn_cast<VariableDefinitionRule>(r)) {
            if (!uses.isUsed(v->getVariable())) continue;
        }
        result.push_back(r);
    }
    return result;
}

} // end anonymous namespace

std::vector<Rule *> ExtractForwardRules(const std::vector<Rule *> & rules) {
    std::vector<Rule *> result;
    UnitList units;
    for (Rule * r : rules) {
        if (const FilterRule * f = dyn_cast<FilterRule>(r)) {
            if (!f->isInverse()) result.push_back(makeFilterRule(f->getFilterSet()));
        } else if (const TransformRule * t = dyn_cast<TransformRule>(r)) {
            if (t->hasForward()) {
                units.addTransform(makeTransformRule(t->getForwardID(), t->getForwardFilter()));
            } else {
                units.addSeparator();
            }
        } else if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            if (appliesForward(c->getDirection())) {
                units.addToGroup(makeConversionRule(sourceSide(c->getLeftSide()), Direction::Forward, resultSide(c->getRightSide())));
            }
        } else {
            units.addToGroup(r);  // variable definition
        }
    }
    units.emit(result);
    return removeUnusedVariables(result);
}

std::vector<Rule *> ExtractReverseBackwardRules(const std::vector<Rule *> & rules) {
    std::vector<Rule *> result;
    std::vector<Rule *> variables;
    UnitList units;
    for (Rule * r : rules) {
        if (const FilterRule * f = dyn_cast<FilterRule>(r)) {
            if (f->isInverse()) result.push_back(makeFilterRule(f->getFilterSet()));
        } else if (const TransformRule * t = dyn_cast<TransformRule>(r)) {
            if (t->hasBackward()) {
                units.addTransform(makeTransformRule(t->getBackwardID(), t->getBackwardFilter()));
            } else {
                units.addSeparator();
            }
        } else if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            if (appliesBackward(c->getDirection())) {
                units.addToGroup(makeConversionRule(sourceSide(c->getRightSide()), Direction::Forward, resultSide(c->getLeftSide())));
            }
        } else {
            // Variable definitions must precede their uses in the reversed rules.
            variables.push_back(r);
        }
    }
    result.insert(result.end(), variables.begin(), variables.end());
    units.reverse();
    units.emit(result);
    return removeUnusedVariables(result);
}

}

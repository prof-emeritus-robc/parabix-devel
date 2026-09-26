/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules.h>
#include <algorithm>

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
    return result;
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
    return result;
}

}

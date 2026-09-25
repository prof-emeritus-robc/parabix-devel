/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules.h>
#include <re/adt/adt.h>
#include <re/adt/re_utility.h>
#include <algorithm>
#include <cctype>

using namespace llvm;
using namespace re;

namespace ldml {

static std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){return std::tolower(c);});
    return s;
}

TransformID::TransformID(const std::string & id)
: mText(id) {
    std::string body = id;
    const auto slash = body.find('/');
    if (slash != std::string::npos) {
        mVariant = body.substr(slash + 1);
        body = body.substr(0, slash);
    }
    // BCP47 form: target-t-source, e.g., und-Latn-t-und-grek-m0-ungegn.
    const auto t = lowercase(body).find("-t-");
    if (t != std::string::npos) {
        mTarget = body.substr(0, t);
        mSource = body.substr(t + 3);
        return;
    }
    const auto hyphen = body.find('-');
    if (hyphen == std::string::npos) {
        mSource = "Any";
        mTarget = body;
    } else {
        mSource = body.substr(0, hyphen);
        mTarget = body.substr(hyphen + 1);
    }
}

TransformID::TransformID(std::string source, std::string target, std::string variant)
: mSource(std::move(source)), mTarget(std::move(target)), mVariant(std::move(variant)) {
    mText = getCanonicalName();
}

std::string TransformID::getCanonicalName() const {
    std::string name = mSource + "-" + mTarget;
    if (hasVariant()) name += "/" + mVariant;
    return name;
}

static bool isAnySource(const std::string & s) {
    const std::string lc = lowercase(s);
    return lc == "any" || lc == "und";
}

bool TransformID::isBuiltIn() const {
    if (!isAnySource(mSource) || hasVariant()) return false;
    const std::string t = lowercase(mTarget);
    return t == "nfc" || t == "nfd" || t == "nfkc" || t == "nfkd"
        || t == "lower" || t == "upper" || t == "title"
        || t == "null" || t == "remove";
}

TransformID TransformID::inverse() const {
    if (empty()) return TransformID();
    if (isBuiltIn()) {
        const std::string t = lowercase(mTarget);
        std::string inv;
        if (t == "nfc") inv = "NFD";
        else if (t == "nfd") inv = "NFC";
        else if (t == "nfkc") inv = "NFKD";
        else if (t == "nfkd") inv = "NFKC";
        else if (t == "upper") inv = "Lower";
        else if (t == "lower" || t == "title") inv = "Upper";
        else inv = "Null";   // Null and Remove
        return TransformID(inv);
    }
    return TransformID(mTarget, mSource, mVariant);
}

Direction TransformRule::getDirection() const {
    if (hasForward() && hasBackward()) return Direction::Both;
    return hasForward() ? Direction::Forward : Direction::Backward;
}

TransformRule * makeTransformRule(TransformID forward, re::RE * filter) {
    TransformID backward = forward.inverse();
    return TransformRule::Create(std::move(forward), filter, false, std::move(backward), filter);
}

TransformRule * makeTransformRule(TransformID forward, re::RE * forwardFilter,
                                  TransformID backward, re::RE * backwardFilter) {
    return TransformRule::Create(std::move(forward), forwardFilter, true, std::move(backward), backwardFilter);
}

std::string VariableDefinitionRule::getName() const {
    return mVariable->getName();
}

re::RE * VariableDefinitionRule::getDefinition() const {
    return mVariable->getDefinition();
}

VariableDefinitionRule * makeVariableDefinitionRule(const std::string & name, re::RE * definition) {
    return VariableDefinitionRule::Create(makeName(name, definition));
}

static RE * emptyIfNull(RE * re) {
    return re ? re : makeSeq();
}

RuleSide::RuleSide(RE * before, RE * completed, bool hasCursor, RE * revisit, int cursorOffset, RE * after)
: mBeforeContext(before)
, mCompletedResult(emptyIfNull(completed))
, mResultToRevisit(emptyIfNull(revisit))
, mText(makeSeq({mCompletedResult, mResultToRevisit}))
, mAfterContext(after)
, mHasCursor(hasCursor)
, mCursorOffset(cursorOffset) {

}

RuleSide * makeRuleSide(RE * text) {
    return RuleSide::Create(nullptr, text, false, nullptr, 0, nullptr);
}

RuleSide * makeRuleSide(RE * before, RE * text, RE * after) {
    return RuleSide::Create(before, text, false, nullptr, 0, after);
}

RuleSide * makeRuleSide(RE * before, RE * completed, RE * revisit, int cursorOffset, RE * after) {
    return RuleSide::Create(before, completed, true, revisit, cursorOffset, after);
}

RE * makeTextBoundary() {
    return makeAlt({makeStart(), makeEnd()});
}

bool isTextBoundary(const RE * re) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        return alt->size() == 2 && includesTextBoundary(re);
    }
    return false;
}

bool includesTextBoundary(const RE * set) {
    if (const Alt * alt = dyn_cast<Alt>(set)) {
        bool start = false;
        bool end = false;
        for (const RE * a : *alt) {
            start |= isa<Start>(a);
            end |= isa<End>(a);
        }
        return start && end;
    }
    return false;
}

static CC * dotExclusions() {
    return makeCC({{0x0A, 0x0A}, {0x0D, 0x0D}});
}

RE * makeDotSet() {
    return makeComplement(makeAlt({makePropertyExpression("zp"), makePropertyExpression("zl"), dotExclusions()}));
}

static bool isPlainProperty(const RE * re, const std::string & ident) {
    if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        return pe->getPropertyIdentifier() == ident && pe->getValueString().empty();
    }
    return false;
}

bool isDotSet(const RE * re) {
    const Diff * d = dyn_cast<Diff>(re);
    if (!d || !isa<Any>(d->getLH())) return false;
    const Alt * alt = dyn_cast<Alt>(d->getRH());
    if (!alt || alt->size() != 3) return false;
    return isPlainProperty((*alt)[0], "zp") && isPlainProperty((*alt)[1], "zl")
        && isa<CC>((*alt)[2]) && *cast<CC>((*alt)[2]) == *dotExclusions();
}

static const std::string FunctionNamespace = "&";

Name * makeFunctionCall(const TransformID & id, RE * argument) {
    return makeName(FunctionNamespace, id.getText(), argument);
}

bool isFunctionCall(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return n->hasNamespace() && n->getNamespace() == FunctionNamespace;
    }
    return false;
}

TransformID getFunctionID(const Name * call) {
    return TransformID(call->getName());
}

bool appliesInDirection(const Rule * r, Direction d) {
    if (const FilterRule * f = dyn_cast<FilterRule>(r)) {
        return f->getDirection() == d;
    } else if (const TransformRule * t = dyn_cast<TransformRule>(r)) {
        return d == Direction::Backward ? t->hasBackward() : t->hasForward();
    } else if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
        return d == Direction::Backward ? appliesBackward(c->getDirection()) : appliesForward(c->getDirection());
    }
    return true; // variable definitions
}

}

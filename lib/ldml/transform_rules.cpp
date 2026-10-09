/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules.h>
#include <re/adt/adt.h>
#include <re/adt/re_utility.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_transformer.h>
#include <algorithm>
#include <map>
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

static const std::string TextBoundaryNamespace = "$";

RE * makeTextBoundary() {
    static Name * const marker = makeName(TextBoundaryNamespace, "$", nullptr);
    return marker;
}

RE * makeBoundarySet(bool beforeContext) {
    RE * const boundary = beforeContext ? static_cast<RE *>(makeStart()) : static_cast<RE *>(makeEnd());
    return Alt::Create({makeCC(), boundary});
}

// The context with the text boundary dropped from the sets (alternations)
// that include it: beyond is true if the rest of the context beyond re (after
// it, or before it in a before context) must match a character, so that the
// boundary cannot be matched; last is true if nothing of the context lies
// beyond re.  A negated set [^X] that is the last item (outermost, at the end
// of an after context or the beginning of a before context) becomes the
// negative assertion (?!X) or (?<!X), which holds at the boundary.
static RE * dropBoundary(RE * re, bool after, bool beyond, bool last) {
    if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> items(seq->begin(), seq->end());
        bool changed = false;
        const int n = static_cast<int>(items.size());
        bool b = beyond;
        for (int k = 0; k < n; ++k) {
            const int i = after ? (n - 1 - k) : k;
            RE * const x = dropBoundary(items[i], after, b, last && (k == 0));
            changed |= (x != items[i]);
            b = b || (minMatchLength(items[i]) > 0);
            items[i] = x;
        }
        return changed ? makeSeq(items.begin(), items.end()) : re;
    } else if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> alts;
        bool boundary = false;
        bool changed = false;
        for (RE * a : *alt) {
            if (after ? isa<End>(a) : isa<Start>(a)) {
                boundary = true;
                continue;
            }
            RE * const x = dropBoundary(a, after, beyond, last);
            changed |= (x != a);
            alts.push_back(x);
        }
        if (boundary && last && (alts.size() == 1)) {
            if (Diff * d = dyn_cast<Diff>(alts[0])) {
                if (isa<Any>(d->getLH())) {
                    return makeAssertion(d->getRH(), after ? Assertion::Kind::LookAhead : Assertion::Kind::LookBehind,
                                         Assertion::Sense::Negative);
                }
            }
        }
        if (boundary && !beyond) {
            alts.push_back(after ? static_cast<RE *>(makeEnd()) : static_cast<RE *>(makeStart()));
        } else {
            changed |= boundary;
        }
        if (!changed) return re;
        return (alts.size() == 1) ? alts[0] : makeAlt(alts.begin(), alts.end());
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        // Each repetition is followed by another or by the rest of the context.
        RE * const x = dropBoundary(rep->getRE(), after, beyond, false);
        return (x == rep->getRE()) ? re : makeRep(x, rep->getLB(), rep->getUB());
    } else if (Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n) || (n->getDefinition() == nullptr)) return re;
        RE * const x = dropBoundary(n->getDefinition(), after, beyond, last);
        return (x == n->getDefinition()) ? re : x;
    }
    // (Captures are kept, for the references to them.)
    return re;
}

RE * engineContext(RE * context, bool afterContext) {
    return dropBoundary(context, afterContext, false, true);
}

namespace {

class ForwardPossessive final : public RE_Transformer {
public:
    ForwardPossessive() : RE_Transformer("ForwardPossessive") {}
    RE * transformRep(Rep * rep) override {
        return Rep::Create(transform(rep->getRE()), rep->getLB(), rep->getUB(), Rep::Kind::Possessive);
    }
};

class BackwardPossessive final : public RE_Transformer {
public:
    BackwardPossessive() : RE_Transformer("BackwardPossessive") {}
    RE * transformRep(Rep * rep) override {
        RE * const e = transform(rep->getRE());
        const int lb = rep->getLB();
        const int ub = rep->getUB();
        if (ub == Rep::UNBOUNDED_REP) {
            return makeSeq({makeNegativeLookBehindAssertion(e), makeRep(e, lb, ub)});
        } else if (lb == ub) {
            return makeRep(e, ub, ub);
        }
        return makeAlt({makeSeq({makeNegativeLookBehindAssertion(e), makeRep(e, lb, ub - 1)}),
                        makeRep(e, ub, ub)});
    }
};

}

RE * possessiveContext(RE * context, bool afterContext) {
    if (afterContext) return ForwardPossessive().transformRE(context);
    return BackwardPossessive().transformRE(context);
}

bool isTextBoundary(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return n->hasNamespace() && n->getNamespace() == TextBoundaryNamespace;
    }
    return false;
}

bool isBoundary(const RE * re) {
    return isa<Start>(re) || isa<End>(re) || isTextBoundary(re);
}

bool includesTextBoundary(const RE * set) {
    if (const Alt * alt = dyn_cast<Alt>(set)) {
        for (const RE * a : *alt) {
            if (isBoundary(a)) return true;
        }
    }
    return false;
}

namespace {

// Does the RE include the text boundary marker (possibly through variables)?
bool includesMarker(const RE * re) {
    if (re == nullptr) return false;
    if (isTextBoundary(re)) return true;
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && includesMarker(n->getDefinition());
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (includesMarker(e)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (includesMarker(e)) return true;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return includesMarker(rep->getRE());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        return includesMarker(d->getLH()) || includesMarker(d->getRH());
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        return includesMarker(x->getLH()) || includesMarker(x->getRH());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return includesMarker(c->getCapturedRE());
    }
    return false;
}

// The resolved copies of variables, for each resolution, and their originals.
std::map<std::pair<const Name *, BoundaryResolution>, Name *> & resolvedCopies() {
    static std::map<std::pair<const Name *, BoundaryResolution>, Name *> copies;
    return copies;
}

std::map<const Name *, const Name *> & copyOriginals() {
    static std::map<const Name *, const Name *> originals;
    return originals;
}

RE * resolve(RE * re, const BoundaryResolution before, std::map<Capture *, Capture *> * captures) {
    if (re == nullptr || !includesMarker(re)) return re;
    if (isTextBoundary(re)) {
        switch (before) {
            case BoundaryResolution::Start: return makeStart();
            case BoundaryResolution::End: return makeEnd();
            default: return makeAlt();
        }
    } else if (Name * n = dyn_cast<Name>(re)) {
        // A variable whose definition includes the marker.
        auto & copies = resolvedCopies();
        auto f = copies.find(std::make_pair(n, before));
        if (f != copies.end()) return f->second;
        Name * copy = makeName(n->getName(), resolve(n->getDefinition(), before, nullptr));
        copies.emplace(std::make_pair(n, before), copy);
        copyOriginals().emplace(copy, originalVariable(n));
        return copy;
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> elems;
        for (RE * e : *seq) elems.push_back(resolve(e, before, captures));
        return makeSeq(elems.begin(), elems.end());
    } else if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> elems;
        for (RE * e : *alt) elems.push_back(resolve(e, before, captures));
        return makeAlt(elems.begin(), elems.end());
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        return makeRep(resolve(rep->getRE(), before, captures), rep->getLB(), rep->getUB());
    } else if (Diff * d = dyn_cast<Diff>(re)) {
        return makeDiff(resolve(d->getLH(), before, captures), resolve(d->getRH(), before, captures));
    } else if (Intersect * x = dyn_cast<Intersect>(re)) {
        return makeIntersect(resolve(x->getLH(), before, captures), resolve(x->getRH(), before, captures));
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        Capture * const rebuilt = makeCapture(c->getName(), resolve(c->getCapturedRE(), before, captures));
        if (captures) captures->emplace(c, rebuilt);
        return rebuilt;
    }
    return re;
}

}

RE * resolveTextBoundary(RE * re, BoundaryResolution resolution, std::map<Capture *, Capture *> * captures) {
    return resolve(re, resolution, captures);
}

bool mayIncludeTextBoundary(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        if (isTextBoundary(n)) return true;
        if (isFunctionCall(n)) return false;
        const Name * const original = originalVariable(n);
        return includesMarker(original->getDefinition()) || (n->getDefinition() && mayIncludeTextBoundary(n->getDefinition()));
    }
    if (isBoundary(re)) return true;
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (mayIncludeTextBoundary(a)) return true;
        }
    }
    return false;
}

const Name * originalVariable(const Name * n) {
    auto f = copyOriginals().find(n);
    return f == copyOriginals().end() ? n : f->second;
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

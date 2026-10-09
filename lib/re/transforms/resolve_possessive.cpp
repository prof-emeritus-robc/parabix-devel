#include <re/transforms/resolve_possessive.h>
#include <re/transforms/re_transformer.h>
#include <re/adt/adt.h>
#include <re/analysis/re_analysis.h>

using namespace llvm;

namespace re {

//
//  A possessive repetition e{lb,ub}+ takes as many repetitions as it can (up
//  to ub), without backtracking.  In general, it is resolved with a negative
//  lookahead: e{lb,}+ is e{lb,} (?!e), and e{lb,ub}+ is e{lb,ub-1} (?!e) | e{ub}.
//
//  Where possible, the repetitions of a character class S are resolved without
//  assertions, which the lookahead implementation does not support everywhere:
//  - at the end of a lookahead body, S{lb,ub}+ matches where S{lb,ub} does (only
//    whether the lookahead holds matters);
//  - followed by $, or by items whose first characters are not in S, S{lb,ub}+
//    is S{lb,ub};
//  - followed by a character class B, S{lb,}+ B is S{lb,} (B - S), and
//    S{lb,ub}+ B is S{lb,ub-1} (B - S) | S{ub} B.
//  The end of a top-level RE or of a lookbehind body is not treated as such:
//  there, a possessive repetition determines the match end or must reach the
//  position of the lookbehind.
//
class ResolvePossessive final : public RE_Transformer {
public:
    ResolvePossessive() : RE_Transformer("ResolvePossessive") {}
protected:
    RE * transformRep(Rep * rep) override;
    RE * transformSeq(Seq * seq) override;
    RE * transformAssertion(Assertion * a) override;
private:
    RE * resolve(Rep * rep, const std::vector<RE *> & rest, bool atLookaheadEnd);
    // Is the RE being transformed at the end of a lookahead body?
    bool mAtLookaheadEnd = false;
};

//  The general resolution of e{lb,ub}+.
static RE * withAssertion(RE * e, const int lb, const int ub) {
    RE * const notE = makeNegativeLookAheadAssertion(e);
    if (ub == Rep::UNBOUNDED_REP) {
        return makeSeq({makeRep(e, lb, ub), notE});
    }
    return makeAlt({makeSeq({makeRep(e, lb, ub - 1), notE}), makeRep(e, ub, ub)});
}

//  The possessive repetition rep (with its body transformed), followed by the
//  items rest (transformed), within the same sequence; returns the items of the
//  sequence that replace rep and rest.
RE * ResolvePossessive::resolve(Rep * rep, const std::vector<RE *> & rest, bool atLookaheadEnd) {
    RE * const e = rep->getRE();
    const int lb = rep->getLB();
    const int ub = rep->getUB();
    if (lb == ub) {
        return makeSeq({makeRep(e, lb, ub), makeSeq(rest.begin(), rest.end())});
    }
    CC * const S = resolveCharClass(e);
    if (S) {
        if (rest.empty()) {
            if (atLookaheadEnd) return makeRep(e, lb, ub);
        } else if (isa<End>(rest.front())) {
            return makeSeq({makeRep(e, lb, ub), makeSeq(rest.begin(), rest.end())});
        } else {
            RE * const following = makeSeq(rest.begin(), rest.end());
            CC * const first = firstCharClass(following);
            if (first && first->getAlphabet() == S->getAlphabet()) {
                if (intersectCC(first, S)->empty()) {
                    return makeSeq({makeRep(e, lb, ub), following});
                }
                if (CC * const B = resolveCharClass(rest.front())) {
                    RE * const after = makeSeq(rest.begin() + 1, rest.end());
                    RE * const notS = makeSeq({subtractCC(B, S), after});
                    if (ub == Rep::UNBOUNDED_REP) {
                        return makeSeq({makeRep(e, lb, ub), notS});
                    }
                    return makeAlt({makeSeq({makeRep(e, lb, ub - 1), notS}),
                                    makeSeq({makeRep(e, ub, ub), B, after})});
                }
            }
        }
    }
    return makeSeq({withAssertion(e, lb, ub), makeSeq(rest.begin(), rest.end())});
}

RE * ResolvePossessive::transformRep(Rep * rep) {
    const bool atEnd = mAtLookaheadEnd;
    mAtLookaheadEnd = false;
    RE * const e = transform(rep->getRE());
    mAtLookaheadEnd = atEnd;
    if (rep->getKind() == Rep::Kind::Possessive) {
        return resolve(cast<Rep>(Rep::Create(e, rep->getLB(), rep->getUB(), Rep::Kind::Possessive)), {}, atEnd);
    }
    if (e == rep->getRE()) return rep;
    return makeRep(e, rep->getLB(), rep->getUB());
}

RE * ResolvePossessive::transformSeq(Seq * seq) {
    const bool atEnd = mAtLookaheadEnd;
    std::vector<RE *> items(seq->begin(), seq->end());
    //  Transform the items from the last, so that each possessive repetition is
    //  resolved with the transformed items following it.
    std::vector<RE *> rest;
    for (size_t i = items.size(); i-- > 0; ) {
        mAtLookaheadEnd = atEnd && rest.empty();
        Rep * const rep = dyn_cast<Rep>(items[i]);
        if (rep && rep->getKind() == Rep::Kind::Possessive) {
            mAtLookaheadEnd = false;
            RE * const e = transform(rep->getRE());
            Rep * const resolvedBody = cast<Rep>(Rep::Create(e, rep->getLB(), rep->getUB(), Rep::Kind::Possessive));
            RE * const replaced = resolve(resolvedBody, rest, atEnd && rest.empty());
            rest.clear();
            rest.push_back(replaced);
        } else {
            rest.insert(rest.begin(), transform(items[i]));
        }
    }
    mAtLookaheadEnd = atEnd;
    return makeSeq(rest.begin(), rest.end());
}

RE * ResolvePossessive::transformAssertion(Assertion * a) {
    const bool atEnd = mAtLookaheadEnd;
    mAtLookaheadEnd = (a->getKind() == Assertion::Kind::LookAhead);
    RE * const asserted = transform(a->getAsserted());
    mAtLookaheadEnd = atEnd;
    if (asserted == a->getAsserted()) return a;
    return makeAssertion(asserted, a->getKind(), a->getSense());
}

RE * resolvePossessiveQuantifiers(RE * re) {
    return ResolvePossessive().transformRE(re);
}

}

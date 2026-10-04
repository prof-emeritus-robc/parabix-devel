/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/transforms/assertion_transformations.h>

#include <llvm/Support/Casting.h>
#include <re/adt/adt.h>
#include <re/analysis/nullable.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/remove_nullable.h>
#include <re/transforms/variable_alt_promotion.h>

using namespace llvm;

namespace re {
//
// Standardizing assertions primarily has a role in
// transforming lookahead assertions to structures supported
// by the regular expresion compilers as well as applying
// some simplifications for both lookahead and lookbehind
// assertions.
//
class AssertionStandardizer : public RE_Transformer {
public:
    AssertionStandardizer(const cc::Alphabet * lengthAlpha) :
        RE_Transformer("AssertionStandardizer"), mLengthAlphabet(lengthAlpha) {}
    RE * transformAssertion(Assertion * a) override {
        RE * const asserted0 = a->getAsserted();
        auto aKind = a->getKind();
        auto aSense = a->getSense();
        RE * asserted = transform(asserted0);
        if (isNullable(asserted)) {
            // Lookahead/behind of a nullable RE always holds; a negative one never does.
            if (aSense == Assertion::Sense::Positive) return makeSeq();
            return makeAlt();
        }
        if (aKind== Assertion::Kind::LookBehind) {
            asserted = removeNullablePrefix(asserted);
        }
        if (aKind == Assertion::Kind::LookAhead) {
            // Try to transform into alternations of fixed length assertions.
            asserted = removeNullableSuffix(asserted);
            asserted = separateFinalStar(asserted);
            // A chain such as B*C D*E is compiled directly (see
            // parseLookaheadChain); zero bound elimination would split it
            // into forms that are not.
            std::vector<LookaheadSegment> segments;
            if (!hasUniquePrefix(asserted) && parseLookaheadChain(asserted, mLengthAlphabet, segments)) {
                if (asserted == asserted0) return a;
                return makeAssertion(asserted, aKind, aSense);
            }
            asserted = zeroBoundElimination(asserted);
            asserted = variableAltPromotion(asserted, mLengthAlphabet);
            // Now if the asserted RE is an alternation, we can transform
            // a positive lookahead of alternatives to an alternation of
            // positive lookaheads, or a negative lookahead of alternatives
            // to a sequence of negative lookaheads of each alternative
            // (each must be false).
            if (Alt * alt = dyn_cast<Alt>(asserted)) {
                std::vector<RE *> alt_lookaheads;
                for (auto e : *alt) {
                    RE * distributed = distributeNestedLookahead(e, aSense);
                    alt_lookaheads.push_back(distributed ? distributed : makeAssertion(e, aKind, aSense));
                }
                if (aSense == Assertion::Sense::Positive) {
                    return makeAlt(alt_lookaheads.begin(), alt_lookaheads.end());
                } else {
                    return makeSeq(alt_lookaheads.begin(), alt_lookaheads.end());
                }
            }
            if (RE * distributed = distributeNestedLookahead(asserted, aSense)) {
                return distributed;
            }
        }
        if (asserted == asserted0) return a;
        return makeAssertion(asserted, aKind, aSense);
    }
private:
    //
    // A lookahead body ending X{lb,} F, where F is a character class that
    // overlaps the character class X, is equivalent to one ending
    // X{lb} (X-F)* F: after the first lb characters, the first character
    // of F or not in X decides the match.  The run of X-F is then maximal,
    // as a lookahead chain requires, e.g. (?=.*c) becomes (?=[^c]*c).
    // (This needs F to be the last item: (?=a*ab) is not (?=ab).)
    //
    static RE * separateFinalStar(RE * body) {
        std::vector<RE *> elems;
        flatten(body, elems);
        const auto n = elems.size();
        if (n < 2) return body;
        Rep * const rep = dyn_cast<Rep>(elems[n - 2]);
        if ((rep == nullptr) || (rep->getUB() != Rep::UNBOUNDED_REP)) return body;
        CC * const X = resolveCharClass(rep->getRE());
        CC * const F = resolveCharClass(elems[n - 1]);
        if ((X == nullptr) || (F == nullptr) || (X->getAlphabet() != F->getAlphabet())) return body;
        if (intersectCC(X, F)->empty()) return body;
        std::vector<RE *> result(elems.begin(), elems.end() - 2);
        if (rep->getLB() > 0) {
            result.push_back(makeRep(rep->getRE(), rep->getLB(), rep->getLB()));
        }
        CC * const remaining = subtractCC(X, F);
        if (!remaining->empty()) {
            result.push_back(makeRep(remaining, 0, Rep::UNBOUNDED_REP));
        }
        result.push_back(elems[n - 1]);
        return makeSeq(result.begin(), result.end());
    }

    // The elements of r as a sequence, seeing through nested sequences and
    // single alternatives.
    static void flatten(RE * r, std::vector<RE *> & elems) {
        if (Alt * alt = dyn_cast<Alt>(r)) {
            if (alt->size() == 1) {
                flatten(alt->front(), elems);
                return;
            }
        } else if (Seq * seq = dyn_cast<Seq>(r)) {
            for (RE * e : *seq) {
                flatten(e, elems);
            }
            return;
        }
        elems.push_back(r);
    }

    //
    // A lookahead whose body contains a lookahead is not directly compilable.
    // If the body is P A S, where A is the first lookahead (on X) and P has a
    // fixed length (so that P matches at most one string at any position):
    //   (?=P A S)  ==>  A' (?=P S)
    //   (?!P A S)  ==>  ¬A' | (?!P S)
    // where A' is a lookahead of P X with the sense of A, and ¬A' has the
    // opposite sense.  A lookbehind A is handled in the same way when P is
    // empty, with A' = A.  If instead an alternation containing assertions is
    // found first, the body is expanded over its alternatives,
    //   P (E1|E2) S  ==>  P E1 S | P E2 S,
    // so that the lookahead is split by alternative.  The result is
    // standardized in turn.  Returns nullptr if no rule applies.
    //
    RE * distributeNestedLookahead(RE * body, Assertion::Sense sense) {
        std::vector<RE *> elems;
        if (Seq * s = dyn_cast<Seq>(body)) {
            elems.assign(s->begin(), s->end());
        } else {
            elems.push_back(body);
        }
        for (unsigned i = 0; i < elems.size(); i++) {
            if (Alt * alt = dyn_cast<Alt>(elems[i])) {
                if (!hasTopLevelAssertion(alt)) continue;
                std::vector<RE *> alts;
                for (RE * e : *alt) {
                    std::vector<RE *> PES(elems.begin(), elems.begin() + i);
                    PES.push_back(e);
                    PES.insert(PES.end(), elems.begin() + i + 1, elems.end());
                    alts.push_back(makeSeq(PES.begin(), PES.end()));
                }
                return transform(makeAssertion(makeAlt(alts.begin(), alts.end()), Assertion::Kind::LookAhead, sense));
            }
            Assertion * inner = dyn_cast<Assertion>(elems[i]);
            if (inner == nullptr) continue;
            const auto innerKind = inner->getKind();
            if ((innerKind == Assertion::Kind::LookBehind) && (i > 0)) continue;
            std::vector<RE *> P(elems.begin(), elems.begin() + i);
            auto pRange = getLengthRange(makeSeq(P.begin(), P.end()), mLengthAlphabet);
            if (pRange.first != pRange.second) return nullptr;
            std::vector<RE *> PX(P);
            PX.push_back(inner->getAsserted());
            std::vector<RE *> PS(P);
            PS.insert(PS.end(), elems.begin() + i + 1, elems.end());
            RE * PX_seq = makeSeq(PX.begin(), PX.end());
            RE * PS_seq = makeSeq(PS.begin(), PS.end());
            const auto innerSense = inner->getSense();
            RE * result;
            if (sense == Assertion::Sense::Positive) {
                result = makeSeq({makeAssertion(PX_seq, innerKind, innerSense),
                                  makeAssertion(PS_seq, Assertion::Kind::LookAhead, Assertion::Sense::Positive)});
            } else {
                const auto flipped = (innerSense == Assertion::Sense::Positive) ? Assertion::Sense::Negative
                                                                                 : Assertion::Sense::Positive;
                result = makeAlt({makeAssertion(PX_seq, innerKind, flipped),
                                  makeAssertion(PS_seq, Assertion::Kind::LookAhead, Assertion::Sense::Negative)});
            }
            return transform(result);
        }
        return nullptr;
    }

    // Is some alternative an assertion, or a sequence with an assertion element?
    static bool hasTopLevelAssertion(Alt * alt) {
        for (RE * e : *alt) {
            if (isa<Assertion>(e)) return true;
            if (Seq * s = dyn_cast<Seq>(e)) {
                for (RE * f : *s) {
                    if (isa<Assertion>(f)) return true;
                }
            }
        }
        return false;
    }

    const cc::Alphabet * mLengthAlphabet;
};

RE * standardizeAssertions(RE * r, const cc::Alphabet * lengthAlpha) {
    return AssertionStandardizer(lengthAlpha).transformRE(r);
}

} // namespace re

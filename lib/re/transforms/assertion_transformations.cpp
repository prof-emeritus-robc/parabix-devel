/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/transforms/assertion_transformations.h>

#include <llvm/Support/Casting.h>
#include <re/adt/adt.h>
#include <re/analysis/nullable.h>
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
                    alt_lookaheads.push_back(makeAssertion(e, aKind, aSense));
                }
                if (aSense == Assertion::Sense::Positive) {
                    return makeAlt(alt_lookaheads.begin(), alt_lookaheads.end());
                } else {
                    return makeSeq(alt_lookaheads.begin(), alt_lookaheads.end());
                }
            }
        }
        if (asserted == asserted0) return a;
        return makeAssertion(asserted, aKind, aSense);
    }
private:
    const cc::Alphabet * mLengthAlphabet;
};

RE * standardizeAssertions(RE * r, const cc::Alphabet * lengthAlpha) {
    return AssertionStandardizer(lengthAlpha).transformRE(r);
}

} // namespace re

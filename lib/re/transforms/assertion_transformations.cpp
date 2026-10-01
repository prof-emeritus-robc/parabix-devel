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

class AssertionNormalizer : public RE_Transformer {
public:
    AssertionNormalizer(const cc::Alphabet * lengthAlpha) :
        RE_Transformer("AssertionNormalizer"), mLengthAlphabet(lengthAlpha) {}
    RE * transformAssertion(Assertion * a) override {
        RE * const asserted0 = a->getAsserted();
        RE * asserted = transform(asserted0);
        if (isNullable(asserted)) {
            // Lookahead/behind of a nullable RE always holds; a negative one never does.
            if (a->getSense() == Assertion::Sense::Positive) return makeSeq();
            return makeAlt();
        }
        if (a->getKind() == Assertion::Kind::LookAhead) {
            asserted = removeNullableSuffix(asserted);
            asserted = zeroBoundElimination(asserted);
            asserted = variableAltPromotion(asserted, mLengthAlphabet);
        }
        if (a->getKind() == Assertion::Kind::LookBehind) {
            asserted = removeNullablePrefix(asserted);
        }
        if (asserted == asserted0) return a;
        return makeAssertion(asserted, a->getKind(), a->getSense());
    }
private:
    const cc::Alphabet * mLengthAlphabet;
};

RE * standardizeAssertions(RE * r, const cc::Alphabet * lengthAlpha) {
    return AssertionNormalizer(lengthAlpha).transformRE(r);
}

} // namespace re

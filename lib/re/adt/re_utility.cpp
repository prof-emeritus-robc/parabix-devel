/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/adt/re_utility.h>
#include <re/adt/adt.h>

namespace re {
    
RE * makeComplement(RE * s) {
  return makeDiff(makeAny(), s);
}

RE * makeZerowidthComplement(RE * s) {
    return makeDiff(makeSeq({}), s);
}

RE * makeWordBoundary() {
    return makePropertyExpression(PropertyExpression::Kind::Boundary, "word");
}

RE * makeWordNonBoundary() {
    return makeZerowidthComplement(makeWordBoundary());
}

RE * makeWordBegin() {
    auto wordC = makePropertyExpression("word");
    return makeNegativeLookBehindAssertion(wordC);
}

RE * makeWordEnd() {
    auto wordC = makePropertyExpression("word");
    return makeNegativeLookAheadAssertion(wordC);
}

    
}

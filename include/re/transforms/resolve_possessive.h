#pragma once

namespace re {

class RE;

//
// Eliminate Possessive Quantifiers, transforming to equivalent
// repetitions using standard quantifiers.
RE * resolvePossessiveQuantifiers(RE * re);

}


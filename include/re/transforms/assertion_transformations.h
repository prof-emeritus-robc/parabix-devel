/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once
#include <re/alphabet/alphabet.h>

namespace re {

class RE;

RE * standardizeAssertions(RE * r, const cc::Alphabet * lengthAlpha = &cc::Unicode);

// For the starts of the matches: a positive lookahead (?=C) of variable length
// at the end of an alternative R (?=C) is matched as part of it, as the starts
// of R (?=C) are those of RC.  (A lookahead of fixed length is kept.)
RE * foldTrailingLookaheads(RE * r, const cc::Alphabet * lengthAlpha = &cc::Unicode);

}

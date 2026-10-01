/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once
#include <re/alphabet/alphabet.h>

namespace re {

class RE;

RE * standardizeAssertions(RE * r, const cc::Alphabet * lengthAlpha = &cc::Unicode);

}

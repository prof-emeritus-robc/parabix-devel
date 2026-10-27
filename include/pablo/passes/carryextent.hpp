/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <cstdint>

namespace pablo {

class PabloKernel;
class Statement;

//  The CarryExtent of a Pablo kernel whose only carry-generating operations are
//  Advances is the largest sum of Advance amounts along any path through it: the
//  effect of the input at any position can reach at most this many positions forward.
//  If-blocks do not add to it, since a skipped If-body would have produced zeroes.
//  Any other carry-generating or accumulating statement disqualifies the kernel.

struct CarryExtentResult {
    bool Qualifies = true;
    uint64_t Extent = 0;
    const Statement * Disqualifier = nullptr;
};

class CarryExtent {
public:
    static CarryExtentResult analyze(const PabloKernel * const kernel);
};

}

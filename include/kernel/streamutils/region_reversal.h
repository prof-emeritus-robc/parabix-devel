/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/core/kernel.h>

namespace kernel {

//
// RegionReversal: each region of the streams of Source reversed in place.  The
// regions are the maximal runs of positions that are not marked in Barriers
// (and the end of the text ends the last one): for a region [s, f),
// Reversed[i] = Source[s + f - 1 - i] for s <= i < f.  The positions of the
// barriers keep their bits.  Reversal is its own inverse, and the regions of
// the reversed streams are those of the source.
//
// A region may have at most limit positions: the kernel looks ahead by limit
// positions, and keeps the part of the region open at the end of each stride
// (at most limit positions of each stream) in its state.  A longer region is
// a fatal error at run time, naming the option --reverse-region-limit (see
// re::ReverseRegionLimit), which gives the limit of the regular expression
// engine's reversals.
//
class RegionReversal final : public MultiBlockKernel {
public:
    RegionReversal(LLVMTypeSystemInterface & ts, StreamSet * Barriers, StreamSet * Source, StreamSet * Reversed,
                   unsigned limit);
protected:
    void linkExternalMethods(KernelBuilder & b) override;
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
private:
    const unsigned mStreams;
    const unsigned mLimit;
};

}

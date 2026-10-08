/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/compiler/pablo_kernel.h>
namespace kernel { class PipelineBuilder; }
namespace kernel { class StreamSet; }

using namespace kernel;

//   SwapBack_N exchanges the items of Source at positions i - n and i, for each
//   position i marked in SwapMarks.
class SwapBack_N : public pablo::PabloKernel {
public:
    SwapBack_N(LLVMTypeSystemInterface & ts, unsigned n, StreamSet * SwapMarks, StreamSet * Source, StreamSet * Swapped);
protected:
    void generatePabloMethod() override;
private:
    unsigned mN;
};

//
//   OddEvenCompareStep implements one comparison step of Batcher's odd-even merge sort,
//   in the merge of sorted blocks of size p into sorted blocks of size 2p.  Items at
//   index distance k within a block are compared; all comparisons are ascending.
//   Inputs:
//     distance: the distance k between compared items
//     block_size:  the size p of the sorted blocks being merged
//     Runs: the runs to be sorted
//     SeqIndex:  a bixnum sequentially numbering items in each instance to be sorted
//     Basis: a bixnum defining the sort order, i.e., the values to be compared.
//   Output:
//     SwapMarks:  a bitstream indicating positions for swapping, i.e. positions i such that
//     the values to be sorted at positions i - distance and i are to be exchanged.
//
class OddEvenCompareStep : public pablo::PabloKernel {
public:
    OddEvenCompareStep(LLVMTypeSystemInterface & ts,
                       unsigned distance, unsigned block_size,
                       StreamSet * Runs, StreamSet * SeqIndex, StreamSet * Basis, StreamSet * SwapMarks);
protected:
    void generatePabloMethod() override;
private:
    unsigned mCompareDistance;
    unsigned mBlockSize;
};

using StreamSets = std::vector<StreamSet *>;

//   Sort each run of 1 bits in Runs, in instances of up to instance_size (a power of 2)
//   positions: runs longer than instance_size are sorted in consecutive chunks of
//   instance_size positions.  ToSort[0] is the sort key; the remaining stream sets are
//   permuted along with it.  The sort is stable.
//
//   The sort uses Batcher's odd-even merge sort network, with
//   ceil_log2(n)(ceil_log2(n) + 1)/2 comparison steps for instance size n.  A run shorter
//   than its power-of-2 block has missing items at the front of the block, and the last
//   chunk of a longer run has missing items at the end of its instance; comparisons with
//   missing items are omitted, which is correct because all comparisons are ascending.
StreamSets OddEvenMergeSortRuns(PipelineBuilder & P, unsigned instance_size, StreamSet * Runs, StreamSets & ToSort);

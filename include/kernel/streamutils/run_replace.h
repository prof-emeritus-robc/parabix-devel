/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/core/kernel.h>
#include <kernel/scan/base.h>

namespace kernel {

class PipelineBuilder;

/*  RunReplace - reintegrate transformed work regions with unchanged source bytes.

    A work mask over the source marks the bytes to be replaced; a placement mask
    over the output marks where the transformed bytes go.  The transformed bytes of
    all regions are given in order in a single stream.  The output consists of the
    unchanged source bytes and the transformed bytes, merged by the placement mask:

    Source:         abcDEfgHij        WorkMask:       ...11..1..
    Transformed:    XYZw              PlacementMask:  ...111..1..
    Output:         abcXYZfgwij

    The unchanged bytes of the source are filtered with RunFilterKernel and merged
    with the transformed bytes by RunMergeKernel.  Both copy whole runs of bytes
    (memcpy) found by scanning their masks for transitions, so their cost is
    proportional to the number of runs plus the data copied, and both are data
    parallel.
*/
void RunReplace(PipelineBuilder & P,
                StreamSet * WorkMask, StreamSet * PlacementMask,
                StreamSet * SourceBytes, StreamSet * TransformedBytes,
                StreamSet * OutputBytes);

/*  RunFilter and RunMerge are byte-stream equivalents of FilterByMask and MergeByMask
    that copy whole runs; each adds the run boundary stream its kernel scans.  */
void RunFilter(PipelineBuilder & P, StreamSet * mask, StreamSet * source, StreamSet * filtered);

void RunMerge(PipelineBuilder & P, StreamSet * mask, StreamSet * marked, StreamSet * unmarked, StreamSet * merged);

/*  FillRunGaps - set to 1 each run of at most maxGap 0 bits that lies between 1 bits.

    Mask:     ..11.111...1....11..
    Filled:   ..111111...1....11..       (maxGap = 2)

    Filling small gaps between work regions merges them, so that there are fewer,
    longer runs to filter, transform and reintegrate, at the cost of transforming the
    gap items.  */
void FillRunGaps(PipelineBuilder & P, StreamSet * mask, unsigned maxGap, StreamSet * filled);

/*  Base of RunFilterKernel and RunMergeKernel: scans the run boundaries of a mask
    (the last position of each run of 0 or 1 bits) with a two-level scan, which skips
    any number of 64-bit words without boundaries, and copies each run with memcpy.  */
class RunCopyKernel : public TwoLevelScanKernel {
public:
    static const unsigned ScanWordWidth = 64;
protected:
    RunCopyKernel(LLVMTypeSystemInterface & ts, std::string && name, bool merge,
                  StreamSet * boundaries, StreamSet * mask);
    void initialize(KernelBuilder & b) override;
    void wordPrologueLogic(KernelBuilder & b, llvm::Value * absWordPos,
                           std::vector<llvm::Value *> indexWord,
                           std::vector<llvm::Value *> & loopVars) override;
    void generateProcessingLogic(KernelBuilder & b, llvm::Value * absItemPos,
                                 std::vector<llvm::Value *> & loopVars) override;
    void finalize(KernelBuilder & b, std::vector<llvm::Value *> & loopVarFinalValues) override;
private:
    void copyRun(KernelBuilder & b, llvm::Value * start, llvm::Value * end);
    const bool mMerge;
    llvm::Value * mStart = nullptr;
    llvm::Value * mEnd = nullptr;
    llvm::Value * mPosA = nullptr;
    llvm::Value * mPosB = nullptr;
};

/*  Copies the bytes of source at the positions marked by mask, as FilterByMask.
    boundaries marks the last position of each run of the mask.  */
class RunFilterKernel final : public RunCopyKernel {
public:
    RunFilterKernel(LLVMTypeSystemInterface & ts, StreamSet * boundaries, StreamSet * mask,
                    StreamSet * source, StreamSet * filtered);
};

/*  Merges marked and unmarked bytes: each position marked by mask takes the next
    byte of marked, each other position the next byte of unmarked, as MergeByMask.
    boundaries marks the last position of each run of the mask.  */
class RunMergeKernel final : public RunCopyKernel {
public:
    RunMergeKernel(LLVMTypeSystemInterface & ts, StreamSet * boundaries, StreamSet * mask,
                   StreamSet * marked, StreamSet * unmarked, StreamSet * merged);
};

}

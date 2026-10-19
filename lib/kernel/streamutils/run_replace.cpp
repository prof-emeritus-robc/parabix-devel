/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/run_replace.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/bitwise/bixlogic.h>
#include <pablo/pablo.h>

using namespace llvm;
using namespace pablo;

namespace kernel {

/*  Marks the last position of each run of 0 or 1 bits in a mask.  It uses a lookahead
    rather than an advance, so that it has no carry state and is data parallel.  */
class RunBoundaries final : public PabloKernel {
public:
    RunBoundaries(LLVMTypeSystemInterface & ts, StreamSet * mask, StreamSet * boundaries)
    : PabloKernel(ts, "RunBoundaries",
                  {Binding{"mask", mask, FixedRate(1), LookAhead(1)}},
                  {Binding{"boundaries", boundaries}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const mask = getInputStreamSet("mask")[0];
        PabloAST * const runEnds = pb.createXor(mask, pb.createLookahead(mask, 1), "runEnds");
        pb.createAssign(pb.createExtract(getOutputStreamVar("boundaries"), pb.getInteger(0)), runEnds);
    }
};

class FillRunGapsKernel final : public PabloKernel {
public:
    FillRunGapsKernel(LLVMTypeSystemInterface & ts, StreamSet * mask, const unsigned maxGap, StreamSet * filled)
    : PabloKernel(ts, "FillRunGaps" + std::to_string(maxGap),
                  {Binding{"mask", mask, FixedRate(1), LookAhead(maxGap)}},
                  {Binding{"filled", filled}})
    , mMaxGap(maxGap) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const mask = getInputStreamSet("mask")[0];
        PabloAST * const gaps = pb.createNot(mask);
        // a gap starting after a 1 bit is short if a 1 bit follows within maxGap positions
        PabloAST * oneFollows = pb.createLookahead(mask, 1);
        for (unsigned i = 2; i <= mMaxGap; ++i) {
            oneFollows = pb.createOr(oneFollows, pb.createLookahead(mask, i));
        }
        PabloAST * const gapStarts = pb.createAnd(pb.createAdvance(mask, 1), gaps);
        PabloAST * const shortGapStarts = pb.createAnd(gapStarts, oneFollows, "shortGapStarts");
        // the positions of each short gap
        PabloAST * const shortGaps = pb.createAnd(pb.createMatchStar(shortGapStarts, gaps), gaps, "shortGaps");
        pb.createAssign(pb.createExtract(getOutputStreamVar("filled"), pb.getInteger(0)), pb.createOr(mask, shortGaps));
    }
private:
    const unsigned mMaxGap;
};

void FillRunGaps(PipelineBuilder & P, StreamSet * mask, const unsigned maxGap, StreamSet * filled) {
    P.CreateKernelCall<FillRunGapsKernel>(mask, maxGap, filled);
}

RunCopyKernel::RunCopyKernel(LLVMTypeSystemInterface & ts, std::string && name, const bool merge,
                             StreamSet * boundaries, StreamSet * mask)
: TwoLevelScanKernel(ts, std::move(name), ScanWordWidth, "boundaries",
                     {LoopVar("runStart", ts.getSizeTy())})
, mMerge(merge) {
    assert (boundaries->getNumElements() == 1 && boundaries->getFieldWidth() == 1);
    assert (mask->getNumElements() == 1 && mask->getFieldWidth() == 1);
    mInputStreamSets.push_back(Binding{"boundaries", boundaries, FixedRate(1), Principal()});
    mInputStreamSets.push_back(Binding{"mask", mask, FixedRate(1)});
    // Each invocation writes exactly its own output items, so segments may be
    // processed in parallel.
    addAttribute(Statefree());
}

void RunCopyKernel::initialize(KernelBuilder & b) {
    // The runs of an invocation end at the end of its mask items (fewer on the final
    // invocation).  Positions are absolute item counts.
    IntegerType * const sizeTy = b.getSizeTy();
    Value * const start = b.getProcessedItemCount("mask");
    Value * const strideItems = b.CreateMul(b.getNumOfStrides(), b.getSize(getStride()));
    Value * const accessible = b.getAccessibleItemCount("mask");
    Value * const items = b.CreateSelect(b.isFinal(), b.CreateUMin(accessible, strideItems), strideItems);
    mStart = start;
    mEnd = b.CreateAdd(start, items);
    // Item positions of the data copied from (merge: marked and unmarked; filter:
    // the filtered output position).
    mPosA = b.CreateAllocaAtEntryPoint(sizeTy);
    mPosB = b.CreateAllocaAtEntryPoint(sizeTy);
    if (mMerge) {
        b.CreateStore(b.getProcessedItemCount("marked"), mPosA);
        b.CreateStore(b.getProcessedItemCount("unmarked"), mPosB);
    } else {
        b.CreateStore(b.getProducedItemCount("filtered"), mPosA);
    }
    mLoopVarInitialValues = {start};
}

void RunCopyKernel::wordPrologueLogic(KernelBuilder &, Value *, std::vector<Value *>, std::vector<Value *> &) {
}

void RunCopyKernel::generateProcessingLogic(KernelBuilder & b, Value * absItemPos, std::vector<Value *> & loopVars) {
    // a run ends after this position
    Value * const boundary = b.CreateUMin(b.CreateAdd(absItemPos, b.getSize(1)), mEnd);
    copyRun(b, loopVars[0], boundary);
    loopVars[0] = boundary;
}

void RunCopyKernel::finalize(KernelBuilder & b, std::vector<Value *> & loopVarFinalValues) {
    copyRun(b, loopVarFinalValues[0], mEnd);
}

void RunCopyKernel::copyRun(KernelBuilder & b, Value * const start, Value * const end) {
    IntegerType * const sizeTy = b.getSizeTy();
    Type * const int8Ty = b.getInt8Ty();
    Constant * const ZERO = b.getSize(0);
    Value * const nonEmpty = b.CreateICmpULT(start, end);
    Value * const length = b.CreateSelect(nonEmpty, b.CreateSub(end, start), ZERO);
    // the value of the run is the mask bit at its start
    Value * const bitPos = b.CreateSelect(nonEmpty, start, mStart);
    Value * const maskByte = b.CreateLoad(int8Ty, b.getRawInputPointer("mask", bitPos));
    Value * const shift = b.CreateTrunc(b.CreateAnd(bitPos, b.getSize(7)), int8Ty);
    Value * const marked = b.CreateTrunc(b.CreateLShr(maskByte, shift), b.getInt1Ty());
    Value * const posA = b.CreateLoad(sizeTy, mPosA);
    if (mMerge) {
        // output positions are mask positions
        Value * const posB = b.CreateLoad(sizeTy, mPosB);
        Value * const source = b.CreateSelect(marked, b.getRawInputPointer("marked", posA),
                                                      b.getRawInputPointer("unmarked", posB));
        b.CreateMemCpy(b.getRawOutputPointer("merged", start), source, length, 1);
        b.CreateStore(b.CreateAdd(posA, b.CreateSelect(marked, length, ZERO)), mPosA);
        b.CreateStore(b.CreateAdd(posB, b.CreateSelect(marked, ZERO, length)), mPosB);
    } else {
        // source positions are mask positions; posA is the filtered output position
        Value * const copied = b.CreateSelect(marked, length, ZERO);
        b.CreateMemCpy(b.getRawOutputPointer("filtered", posA), b.getRawInputPointer("source", start), copied, 1);
        b.CreateStore(b.CreateAdd(posA, copied), mPosA);
    }
}

RunFilterKernel::RunFilterKernel(LLVMTypeSystemInterface & ts, StreamSet * boundaries, StreamSet * mask,
                                 StreamSet * source, StreamSet * filtered)
: RunCopyKernel(ts, "RunFilter", false, boundaries, mask) {
    assert (source->getFieldWidth() == 8 && source->getNumElements() == 1);
    mInputStreamSets.push_back(Binding{"source", source, FixedRate(1)});
    mOutputStreamSets.push_back(Binding{"filtered", filtered, PopcountOf("mask")});
}

RunMergeKernel::RunMergeKernel(LLVMTypeSystemInterface & ts, StreamSet * boundaries, StreamSet * mask,
                               StreamSet * marked, StreamSet * unmarked, StreamSet * merged)
: RunCopyKernel(ts, "RunMerge", true, boundaries, mask) {
    assert (merged->getFieldWidth() == 8 && merged->getNumElements() == 1);
    mInputStreamSets.push_back(Binding{"marked", marked, PopcountOf("mask")});
    mInputStreamSets.push_back(Binding{"unmarked", unmarked, PopcountOfNot("mask")});
    mOutputStreamSets.push_back(Binding{"merged", merged, FixedRate(1)});
}

void RunFilter(PipelineBuilder & P, StreamSet * mask, StreamSet * source, StreamSet * filtered) {
    StreamSet * const boundaries = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<RunBoundaries>(mask, boundaries);
    P.CreateKernelCall<RunFilterKernel>(boundaries, mask, source, filtered);
}

void RunMerge(PipelineBuilder & P, StreamSet * mask, StreamSet * marked, StreamSet * unmarked, StreamSet * merged) {
    StreamSet * const boundaries = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<RunBoundaries>(mask, boundaries);
    P.CreateKernelCall<RunMergeKernel>(boundaries, mask, marked, unmarked, merged);
}

void RunReplace(PipelineBuilder & P,
                StreamSet * WorkMask, StreamSet * PlacementMask,
                StreamSet * SourceBytes, StreamSet * TransformedBytes,
                StreamSet * OutputBytes) {
    StreamSet * const UnchangedMask = P.CreateStreamSet(1, 1);
    Invert(P, WorkMask, UnchangedMask);
    StreamSet * const UnchangedBytes = P.CreateStreamSet(1, 8);
    RunFilter(P, UnchangedMask, SourceBytes, UnchangedBytes);
    RunMerge(P, PlacementMask, TransformedBytes, UnchangedBytes, OutputBytes);
}

}

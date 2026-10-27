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

/*  Marks the first position of each run of 0 or 1 bits in a mask, except a run of 0
    bits at the start.  (An earlier version marked the last position of each run with a
    lookahead, which made it a ProvisionalLookAheadStride kernel; RunCopyKernel, which
    also reads the mask, could then see boundaries computed from provisional mask data in
    the final stride.  The advance needs a carry, but the kernel is cheap.)  */
class RunBoundaries final : public PabloKernel {
public:
    RunBoundaries(LLVMTypeSystemInterface & ts, StreamSet * mask, StreamSet * boundaries)
    : PabloKernel(ts, "RunStarts",
                  {Binding{"mask", mask}},
                  {Binding{"boundaries", boundaries}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const mask = getInputStreamSet("mask")[0];
        PabloAST * const runStarts = pb.createXor(mask, pb.createAdvance(mask, 1), "runStarts");
        pb.createAssign(pb.createExtract(getOutputStreamVar("boundaries"), pb.getInteger(0)), runStarts);
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
    // All data are addressed relative to the block pointers of this invocation, not
    // with raw item pointers: on the final invocation the pipeline may present an
    // input in a different (e.g., zero-extended) copy of its last blocks.  Each base
    // is the address of the item at the processed (or produced) position.
    Type * const int8Ty = b.getInt8Ty();
    Constant * const BLOCK_MASK = b.getSize(b.getBitBlockWidth() - 1);
    auto inputBase = [&](StringRef name) {
        Value * const offset = b.CreateAnd(b.getProcessedItemCount(name), BLOCK_MASK);
        return b.CreateGEP(int8Ty, b.getInputStreamBlockPtr(name, b.getSize(0)), offset);
    };
    auto outputBase = [&](StringRef name) {
        Value * const offset = b.CreateAnd(b.getProducedItemCount(name), BLOCK_MASK);
        return b.CreateGEP(int8Ty, b.getOutputStreamBlockPtr(name, b.getSize(0)), offset);
    };
    // the mask is processed in whole blocks, so its processed count is block aligned
    mMaskBase = b.getInputStreamBlockPtr("mask", b.getSize(0));
    if (mMerge) {
        mBaseA = inputBase("marked");
        mBaseB = inputBase("unmarked");
        mOutBase = outputBase("merged");
    } else {
        mBaseA = inputBase("source");
        mOutBase = outputBase("filtered");
    }
    // Item positions of the data copied from (merge: marked and unmarked; filter:
    // the filtered output position).
    // Items of the data copied from (merge: marked and unmarked; filter: the
    // filtered output) relative to the bases.
    mPosA = b.CreateAllocaAtEntryPoint(sizeTy);
    mPosB = b.CreateAllocaAtEntryPoint(sizeTy);
    b.CreateStore(b.getSize(0), mPosA);
    b.CreateStore(b.getSize(0), mPosB);
    mLoopVarInitialValues = {start};
}

void RunCopyKernel::wordPrologueLogic(KernelBuilder &, Value *, std::vector<Value *>, std::vector<Value *> &) {
}

void RunCopyKernel::generateProcessingLogic(KernelBuilder & b, Value * absItemPos, std::vector<Value *> & loopVars) {
    // a run starts at this position, so the current run ends before it
    Value * const boundary = b.CreateUMin(absItemPos, mEnd);
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
    // relative to the start of the mask items of this invocation
    Value * const relStart = b.CreateSub(b.CreateSelect(nonEmpty, start, mStart), mStart);
    Value * const maskByte = b.CreateLoad(int8Ty, b.CreateGEP(int8Ty, mMaskBase, b.CreateLShr(relStart, b.getSize(3))));
    Value * const shift = b.CreateTrunc(b.CreateAnd(relStart, b.getSize(7)), int8Ty);
    Value * const marked = b.CreateTrunc(b.CreateLShr(maskByte, shift), b.getInt1Ty());
    Value * const posA = b.CreateLoad(sizeTy, mPosA);
    if (mMerge) {
        // output positions are mask positions
        Value * const posB = b.CreateLoad(sizeTy, mPosB);
        Value * const source = b.CreateSelect(marked, b.CreateGEP(int8Ty, mBaseA, posA),
                                                      b.CreateGEP(int8Ty, mBaseB, posB));
        b.CreateMemCpy(b.CreateGEP(int8Ty, mOutBase, relStart), source, length, 1);
        b.CreateStore(b.CreateAdd(posA, b.CreateSelect(marked, length, ZERO)), mPosA);
        b.CreateStore(b.CreateAdd(posB, b.CreateSelect(marked, ZERO, length)), mPosB);
    } else {
        // source positions are mask positions; posA is the filtered output position
        Value * const copied = b.CreateSelect(marked, length, ZERO);
        b.CreateMemCpy(b.CreateGEP(int8Ty, mOutBase, posA), b.CreateGEP(int8Ty, mBaseA, relStart), copied, 1);
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

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/sorting.h>
#include <kernel/streamutils/run_index.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <boost/intrusive/detail/math.hpp>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/Support/ErrorHandling.h>
#include <functional>

using boost::intrusive::detail::ceil_log2;
using namespace kernel;
using namespace pablo;
using namespace llvm;

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)


//
//  Given a bit stream marking Runs to be sorted and a
//  SortOrder BixNum to be used within each Run, find any
//  violations within the Runs and mark them at the Run end
//  position.
//
class Misorder_Check : public pablo::PabloKernel {
public:
    Misorder_Check(LLVMTypeSystemInterface & ts, StreamSet * Runs, StreamSet * SortOrder, StreamSet * Misordered);
protected:
    void generatePabloMethod() override;
};

Misorder_Check::Misorder_Check (LLVMTypeSystemInterface & ts, StreamSet * Runs, StreamSet * SortOrder, StreamSet * Misordered)
: PabloKernel(ts, "Misorder_Check_" + SortOrder->shapeString(),
// inputs
{Binding{"Runs", Runs, FixedRate(1), LookAhead(1)}, Binding{"SortOrder", SortOrder, FixedRate(1), LookAhead(1)}},
// output
{Binding{"Misordered", Misordered}}) {
}

void Misorder_Check::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    PabloAST * Runs = getInputStreamSet("Runs")[0];
    BixNum SortOrder = getInputStreamSet("SortOrder");
    BixNum SortOrder_ahead(SortOrder.size());
    for (unsigned i = 0; i < SortOrder.size(); i++) {
        SortOrder_ahead[i] = pb.createLookahead(SortOrder[i], 1);
    }
    PabloAST * RunEnds = pb.createAnd(Runs, pb.createNot(pb.createLookahead(Runs, 1)));
    PabloAST * violation = pb.createAnd(bnc.UGT(SortOrder, SortOrder_ahead), pb.createNot(RunEnds));
    PabloAST * violationInSeq = pb.createAnd(pb.createMatchStar(violation, Runs), RunEnds);
    pb.createAssign(pb.createExtract(getOutputStreamVar("Misordered"), pb.getInteger(0)), violationInSeq);
}

class AdjustRunsAndIndexes : public pablo::PabloKernel {
public:
    AdjustRunsAndIndexes(LLVMTypeSystemInterface & ts,
                         StreamSet * Runs, StreamSet * Misordered, StreamSet * SeqIndex,
                         StreamSet * FilteredRuns, StreamSet * AdjustedIndex);
protected:
    void generatePabloMethod() override;
    unsigned mLgth;
};

AdjustRunsAndIndexes::AdjustRunsAndIndexes (LLVMTypeSystemInterface & ts,
                                            StreamSet * Runs, StreamSet * Misordered, StreamSet * SeqIndex,
                                            StreamSet * FilteredRuns, StreamSet * AdjustedIndex)
: PabloKernel(ts, "AdjustRunsAndIndexes_" + SeqIndex->shapeString(),
// inputs
{Binding{"Runs", Runs, FixedRate(1), LookAhead(64)}, Binding{"Misordered", Misordered, FixedRate(1), LookAhead(64)}, Binding{"SeqIndex", SeqIndex, FixedRate(1), LookAhead(64)}},
// output
{Binding{"FilteredRuns", FilteredRuns}, Binding{"AdjustedIndex", AdjustedIndex}}) {
}

void AdjustRunsAndIndexes::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    PabloAST * Runs = getInputStreamSet("Runs")[0];
    PabloAST * RunStart = pb.createAnd(Runs, pb.createNot(pb.createAdvance(Runs, 1)));
    PabloAST * Misordered = getInputStreamSet("Misordered")[0];
    BixNum SeqIndex = getInputStreamSet("SeqIndex");
    unsigned maxLgth = 1<<SeqIndex.size();
    BixNum SeqIndexAhead(SeqIndex.size());
    PabloAST * FilteredRuns = Runs;
    BixNum AdjustedIndex = SeqIndex;
    for (unsigned lgth = 3; lgth < maxLgth; lgth++) {
        for (unsigned i = 0; i < SeqIndex.size(); i++) {
            SeqIndexAhead[i] = pb.createLookahead(SeqIndex[i], lgth-1);
        }
        PabloAST * atRunStart = pb.createAnd(RunStart, pb.createNot(pb.createLookahead(Runs, lgth)));
        atRunStart = pb.createAnd(bnc.EQ(SeqIndexAhead, lgth-1), atRunStart);
        PabloAST * MisorderedAtStart = pb.createAnd(atRunStart, pb.createLookahead(Misordered, lgth-1), "MisorderedAtStart");
        PabloAST * OrderedRunStart = pb.createAnd(atRunStart, pb.createNot(MisorderedAtStart));
        PabloAST * OrderedRun = pb.createAnd(pb.createMatchStar(OrderedRunStart, Runs), Runs);
        FilteredRuns = pb.createAnd(FilteredRuns, pb.createNot(OrderedRun));
        unsigned lgth_ceil = 1 << ceil_log2(lgth);
        unsigned lgth_offset = lgth_ceil - lgth;
        if (lgth_offset != 0) {
            PabloAST * lgthRun = pb.createAnd(pb.createMatchStar(atRunStart, Runs), Runs);
            AdjustedIndex = bnc.Select(lgthRun, bnc.AddModular(AdjustedIndex, lgth_offset), AdjustedIndex);
        }
    }
    for (unsigned i = 0; i < SeqIndex.size(); i++) {
        AdjustedIndex[i] = pb.createAnd(AdjustedIndex[i], FilteredRuns);
    }
    writeOutputStreamSet("FilteredRuns", std::vector<PabloAST *>{FilteredRuns});
    writeOutputStreamSet("AdjustedIndex", AdjustedIndex);
}

//
//  The bitonic network sorts the blocks of each level alternately ascending and
//  descending.  A run shorter than its power-of-2 block occupies the top indexes
//  of the block, and the last chunk of a run longer than the instance size
//  occupies the bottom indexes of its instance; either way, comparisons with the
//  missing items are omitted, which is correct only for ascending comparisons
//  (missing items at the front act as minimal keys, at the back as maximal keys).
//  Any merge only needs the two blocks of a pair to be sorted in opposite
//  directions, so the direction of a block at level k is taken as bit k of the
//  index XOR bit k of a boundary b: the index of the first item of a short run,
//  or the number of items in the last chunk of a long run (0 elsewhere).  Then
//  every block containing both items and missing items contains index b and is
//  sorted ascending.
//
SwapBack_N::SwapBack_N(LLVMTypeSystemInterface & ts, unsigned n, StreamSet * SwapMarks, StreamSet * Source, StreamSet * Swapped)
: PabloKernel(ts, "SwapBack" + std::to_string(n) + "_" + Source->shapeString(),
// inputs
{Binding{"SwapMarks", SwapMarks, FixedRate(1), LookAhead(n)},
 Binding{"Source", Source, FixedRate(1), LookAhead(n)}},
// output
{Binding{"Swapped", Swapped}}), mN(n) {
}

void SwapBack_N::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * SwapMarks = getInputStreamSet("SwapMarks")[0];
    PabloAST * PriorMark = pb.createLookahead(SwapMarks, mN);
    std::vector<PabloAST *> SourceSet = getInputStreamSet("Source");
    std::vector<Var *> SwappedVar(SourceSet.size());
    for (unsigned i = 0; i < SourceSet.size(); i++) {
        SwappedVar[i] = pb.createVar("SwapVar" + std::to_string(i), SourceSet[i]);
    }
    auto nested = pb.createScope();
    pb.createIf(pb.createOr(PriorMark, SwapMarks), nested);
    for (unsigned i = 0; i < SourceSet.size(); i++) {
        PabloAST * compare = nested.createXor(nested.createLookahead(SourceSet[i], mN), SourceSet[i]);
        compare = nested.createAnd(compare, PriorMark);
        PabloAST * flip = nested.createOr(compare, nested.createAdvance(compare, pb.getInteger(mN)));
        nested.createAssign(SwappedVar[i], nested.createXor(SourceSet[i], flip));
    }
    writeOutputStreamSet("Swapped", SwappedVar);
}

class AppendStreamSets : public PabloKernel {
public:
    AppendStreamSets(LLVMTypeSystemInterface & ts, StreamSet * A, StreamSet * B, StreamSet * Combined);
protected:
    void generatePabloMethod() override;
};

AppendStreamSets::AppendStreamSets(LLVMTypeSystemInterface & ts, StreamSet * A, StreamSet * B, StreamSet * Combined)
: PabloKernel(ts, "AppendStreamSets_" + A->shapeString() + "_" + B->shapeString(),
// inputs
{Binding{"A", A}, Binding{"B", B}},
// output
{Binding{"Combined", Combined}}) {
}

void AppendStreamSets::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> A = getInputStreamSet("A");
    std::vector<PabloAST *> B = getInputStreamSet("B");
    std::vector<PabloAST *> Combined(A.size() + B.size());
    for (unsigned i = 0; i < A.size(); i++) {
        Combined[i + B.size()] = A[i];
    }
    for (unsigned i = 0; i < B.size(); i++) {
        Combined[i] = B[i];
    }
    writeOutputStreamSet("Combined", Combined);
}

class RunTails : public PabloKernel {
public:
    RunTails(LLVMTypeSystemInterface & ts, unsigned lgth, StreamSet * Runs, StreamSet * SeqIndex, StreamSet * Tails);
protected:
    void generatePabloMethod() override;
private:
    unsigned mLgth;
};

RunTails::RunTails(LLVMTypeSystemInterface & ts, unsigned lgth, StreamSet * Runs, StreamSet * SeqIndex, StreamSet * Tails)
: PabloKernel(ts, "RunTail" + std::to_string(lgth) + "_" + SeqIndex->shapeString(),
// inputs
{Binding{"Runs", Runs, FixedRate(1), LookAhead(lgth+1)},
 Binding{"SeqIndex", SeqIndex, FixedRate(1), LookAhead(lgth)}},
// output
{Binding{"Tails", Tails}}), mLgth(lgth) {
}

void RunTails::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * Runs = getInputStreamSet("Runs")[0];
    std::vector<PabloAST *> SeqIndex = getInputStreamSet("SeqIndex");
    Var * tailVar = pb.createVar("Tails", pb.createZeroes());
    BixNumCompiler bnc0(pb);
    // First determine a potential tail candidate start position.
    PabloAST * ahead = pb.createLookahead(Runs, mLgth);
    PabloAST * tail_prior = pb.createAnd(Runs, ahead);
    auto nested = pb.createScope();
    pb.createIf(tail_prior, nested);
    BixNumCompiler bnc(nested);
    std::vector<PabloAST *> SeqIndexAhead(SeqIndex.size());
    for (unsigned i = 0; i < SeqIndex.size(); i++) {
        SeqIndexAhead[i] = nested.createLookahead(SeqIndex[i], mLgth);
    }
    PabloAST * confirm = bnc.EQ(bnc.AddModular(SeqIndex, mLgth), SeqIndexAhead);
    PabloAST * tail1 = nested.createAdvance(nested.createAnd(tail_prior, confirm), 1);
    tail1 = nested.createAnd(tail1, nested.createNot(ahead));
    PabloAST * tails = nested.createAnd(nested.createMatchStar(tail1, Runs), Runs);
    nested.createAssign(tailVar, tails);
    pb.createAssign(pb.createExtract(getOutputStreamVar("Tails"), pb.getInteger(0)), tailVar);
}

OddEvenCompareStep::OddEvenCompareStep(LLVMTypeSystemInterface & ts, unsigned distance, unsigned block_size,
                                       StreamSet * Runs, StreamSet * SeqIndex, StreamSet * Basis, StreamSet * SwapMarks)
: PabloKernel(ts, "OddEvenCompareStep<" + std::to_string(block_size) + "," + std::to_string(distance) + ">" +
              SeqIndex->shapeString() + "_" + Basis->shapeString(),
// inputs
{Binding{"Runs", Runs}, Binding{"SeqIndex", SeqIndex}, Binding{"Basis", Basis}},
// output
{Binding{"SwapMarks", SwapMarks}}), mCompareDistance(distance), mBlockSize(block_size) {
}

void OddEvenCompareStep::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * Runs = getInputStreamSet("Runs")[0];
    BixNum SeqIndex = getInputStreamSet("SeqIndex");
    BixNum Basis = getInputStreamSet("Basis");
    BixNumCompiler bnc(pb);
    // Swap marks are placed at the higher item y of each compared pair (x, y),
    // x = y - k, where k is the compare distance.
    BixNum Forward_Basis(Basis.size());
    for (unsigned i = 0; i < Basis.size(); i++) {
        Forward_Basis[i] = pb.createAdvance(Basis[i], mCompareDistance, "Fwd_basis" + std::to_string(i));
    }
    // In merging blocks of size p into blocks of size 2p, Batcher's network compares
    // x and x + k when k = p and bit log2(k) of x is 0, or when k < p, bit log2(k)
    // of x is 1 and x + k is in the same 2p block as x.  In terms of y = x + k:
    // for k = p, bit log2(k) of y is 1; for k < p, bit log2(k) of y is 0 and the bits
    // log2(k) + 1 through log2(p) of y are not all 0.
    const unsigned k_bit = ceil_log2(mCompareDistance);
    PabloAST * compared = nullptr;
    if (mCompareDistance == mBlockSize) {
        compared = SeqIndex[k_bit];
    } else {
        PabloAST * higher = pb.createZeroes();
        for (unsigned b = k_bit + 1; b <= ceil_log2(mBlockSize); b++) {
            higher = pb.createOr(higher, SeqIndex[b]);
        }
        compared = pb.createAnd(pb.createNot(SeqIndex[k_bit]), higher);
    }
    // Both items, and everything between them, must be in the same run.
    PabloAST * inRun = Runs;
    for (unsigned covered = 1; covered <= mCompareDistance; ) {
        const unsigned shift = std::min(covered, mCompareDistance + 1 - covered);
        inRun = pb.createAnd(inRun, pb.createAdvance(inRun, shift));
        covered += shift;
    }
    PabloAST * swap_mark = pb.createAnd3(bnc.UGT(Forward_Basis, Basis, "gt_forward"), compared, inRun);
    pb.createAssign(pb.createExtract(getOutputStreamVar("SwapMarks"), pb.getInteger(0)), swap_mark);
}

//  Number the positions of each run, find the runs that need sorting, and append the
//  (adjusted) position index to the sort key, which makes the sort stable.
static StreamSet * PrepareRunsForSorting(PipelineBuilder & P, unsigned instance_size, StreamSet * Runs, StreamSets & ToSort, StreamSet *& AdjustedIndex) {
    unsigned steps = ceil_log2(instance_size);
    StreamSet * SeqIndex = P.CreateStreamSet(steps);
    P.CreateKernelCall<RunIndex>(Runs, SeqIndex);
    SHOW_BIXNUM(SeqIndex);
    StreamSet * Misordered = P.CreateStreamSet(1);
    P.CreateKernelCall<Misorder_Check>(Runs, ToSort[0], Misordered);
    SHOW_STREAM(Misordered);
    StreamSet * FilteredRuns = P.CreateStreamSet(1);
    AdjustedIndex = P.CreateStreamSet(SeqIndex->getNumElements());
    P.CreateKernelCall<AdjustRunsAndIndexes>(Runs, Misordered, SeqIndex, FilteredRuns, AdjustedIndex);
    SHOW_STREAM(FilteredRuns);
    SHOW_BIXNUM(AdjustedIndex);
    StreamSet * SortOrder = P.CreateStreamSet(SeqIndex->getNumElements() + ToSort[0]->getNumElements());
    P.CreateKernelCall<AppendStreamSets>(ToSort[0], AdjustedIndex, SortOrder);
    ToSort[0] = SortOrder;
    return FilteredRuns;
}

StreamSets OddEvenMergeSortRuns(PipelineBuilder & P, unsigned instance_size, StreamSet * Runs, StreamSets & ToSort) {
    StreamSet * SeqIndex = nullptr;
    StreamSet * FilteredRuns = PrepareRunsForSorting(P, instance_size, Runs, ToSort, SeqIndex);
    // A run shorter than its instance occupies the top indexes of a power-of-2 block,
    // so missing items act as minimal keys at the front, which every (ascending)
    // comparison leaves in place.  Comparisons with them are simply omitted.
    StreamSets Sorted = ToSort;
    for (unsigned p = 1; p < instance_size; p *= 2) {
        for (unsigned k = p; k >= 1; k /= 2) {
            StreamSet * SwapMarks = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<OddEvenCompareStep>(k, p, FilteredRuns, SeqIndex, Sorted[0], SwapMarks);
            SHOW_STREAM(SwapMarks);
            for (unsigned i = 0; i < Sorted.size(); i++) {
                StreamSet * Swapped = P.CreateStreamSet(Sorted[i]->getNumElements(), 1);
                P.CreateKernelCall<SwapBack_N>(k, SwapMarks, Sorted[i], Swapped);
                SHOW_BIXNUM(Swapped);
                Sorted[i] = Swapped;
            }
        }
    }
    return Sorted;
}

SeqRunSort::SeqRunSort(LLVMTypeSystemInterface & ts, StreamSet * Runs, StreamSet * Misordered,
                 StreamSet * Keys, StreamSet * Data, StreamSet * Sorted)
: MultiBlockKernel(ts, "SeqRunSort_" + Keys->shapeString() + "_" + Data->shapeString(),
// inputs
{Binding{"Runs", Runs}, Binding{"Misordered", Misordered},
 Binding{"Keys", Keys, FixedRate(1), Deferred()}, Binding{"Data", Data, FixedRate(1), Deferred()}},
// output
{Binding{"Sorted", Sorted, FixedRate(1), Deferred()}},
{}, {},
// kernel state: the Runs bit at the last position processed
{InternalScalar{ts.getSizeTy(), "PriorRunBit"}}),
mKeyBits(Keys->getNumElements()), mDataBits(Data->getNumElements()) {
    if (LLVM_UNLIKELY(mKeyBits + mDataBits > 64)) {
        llvm::report_fatal_error("SeqRunSort: key and data widths exceed 64 bits");
    }
    assert (Sorted->getNumElements() == mDataBits);
}

//  Each call publishes the items of the region [pending, complete), where pending is
//  the deferred position at entry (the start of any run left incomplete by the prior
//  call) and complete is the end of the newly processed items, or the start of a run
//  that may continue past them.  The region is copied from Data to Sorted, and then
//  each run in it marked as misordered is gathered, sorted and scattered back.
void SeqRunSort::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    const unsigned BW = b.getBitBlockWidth();
    IntegerType * const sizeTy = b.getSizeTy();
    assert (sizeTy->getBitWidth() == 64);
    Type * const blockTy = b.getBitBlockType();
    Constant * const ZERO = b.getSize(0);
    Constant * const ONE = b.getSize(1);
    Constant * const SZ_63 = b.getSize(63);
    Constant * const SZ_64 = b.getSize(64);
    Constant * const SZ_BW = b.getSize(BW);
    Constant * const ALL_ONES = Constant::getAllOnesValue(sizeTy);

    //  New items are [runsPos, end); the Keys, Data and Sorted streams start at pending.
    Value * const runsPos = b.getProcessedItemCount("Runs");
    Value * const runsBlock = b.CreateUDiv(runsPos, SZ_BW);
    Value * const pending = b.getProcessedItemCount("Data");
    Value * const dataBlock = b.CreateUDiv(pending, SZ_BW);
    Value * const keysBlock = b.CreateUDiv(b.getProcessedItemCount("Keys"), SZ_BW);
    Value * const sortedBlock = b.CreateUDiv(b.getProducedItemCount("Sorted"), SZ_BW);
    Value * const isFinal = b.isFinal();
    Value * const strideEnd = b.CreateAdd(runsPos, b.CreateMul(numOfStrides, b.getSize(getStride())));
    Value * const end = b.CreateSelect(isFinal, b.getAvailableItemCount("Runs"), strideEnd);
    Value * const priorRunBit = b.getScalarField("PriorRunBit");

    auto forLoop = [&](Value * const start, Value * const limit, const std::function<void (Value *)> & body) {
        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const loop = b.CreateBasicBlock("loop");
        BasicBlock * const exit = b.CreateBasicBlock("loopExit");
        b.CreateCondBr(b.CreateICmpULT(start, limit), loop, exit);
        b.SetInsertPoint(loop);
        PHINode * const i = b.CreatePHI(sizeTy, 2);
        i->addIncoming(start, entry);
        body(i);
        Value * const next = b.CreateAdd(i, ONE);
        i->addIncoming(next, b.GetInsertBlock());
        b.CreateCondBr(b.CreateICmpULT(next, limit), loop, exit);
        b.SetInsertPoint(exit);
    };
    //  The 64-bit word of a stream containing absolute position pos.
    auto wordPtr = [&](bool output, StringRef name, unsigned k, Value * const pos, Value * const baseBlock) {
        Value * const blockOffset = b.CreateSub(b.CreateUDiv(pos, SZ_BW), baseBlock);
        Value * const blockPtr = output ? b.getOutputStreamBlockPtr(name, b.getSize(k), blockOffset)
                                        : b.getInputStreamBlockPtr(name, b.getSize(k), blockOffset);
        Value * const wordIndex = b.CreateUDiv(b.CreateURem(pos, SZ_BW), SZ_64);
        return b.CreateGEP(sizeTy, blockPtr, wordIndex);
    };
    auto loadBit = [&](StringRef name, unsigned k, Value * const pos, Value * const baseBlock) {
        Value * const word = b.CreateLoad(sizeTy, wordPtr(false, name, k, pos, baseBlock));
        return b.CreateAnd(b.CreateLShr(word, b.CreateURem(pos, SZ_64)), ONE);
    };
    //  The word at position pos (a multiple of 64), with positions at or after limit cleared.
    auto loadMaskedWord = [&](StringRef name, Value * const pos, Value * const baseBlock, Value * const limit) {
        Value * const word = b.CreateLoad(sizeTy, wordPtr(false, name, 0, pos, baseBlock));
        Value * const remaining = b.CreateSub(limit, pos);
        Value * const mask = b.CreateSelect(b.CreateICmpUGE(remaining, SZ_64), ALL_ONES,
                                            b.CreateSub(b.CreateShl(ONE, remaining), ONE));
        return b.CreateAnd(word, mask);
    };
    auto numWords = [&](Value * const from, Value * const to) {
        return b.CreateUDiv(b.CreateAdd(b.CreateSub(to, from), SZ_63), SZ_64);
    };

    Value * const carryVar = b.CreateAllocaAtEntryPoint(sizeTy);
    Value * const startVar = b.CreateAllocaAtEntryPoint(sizeTy);
    Value * const lastMarkVar = b.CreateAllocaAtEntryPoint(sizeTy);

    //  1. Find the end of the complete region: the start of the last run, if the
    //  run reaches the end of the new items without a misordered mark at its end
    //  (which would show the run to be complete).  A run continuing from the prior
    //  call starts at pending.
    b.CreateStore(priorRunBit, carryVar);
    b.CreateStore(pending, startVar);
    b.CreateStore(ZERO, lastMarkVar);
    forLoop(ZERO, numWords(runsPos, end), [&](Value * const w) {
        Value * const pos = b.CreateAdd(runsPos, b.CreateMul(w, SZ_64));
        Value * const runs = loadMaskedWord("Runs", pos, runsBlock, end);
        Value * const marks = loadMaskedWord("Misordered", pos, runsBlock, end);
        Value * const prior = b.CreateOr(b.CreateShl(runs, ONE), b.CreateLoad(sizeTy, carryVar));
        Value * const starts = b.CreateAnd(runs, b.CreateNot(prior));
        Value * const lastStart = b.CreateSub(b.CreateAdd(pos, SZ_63), b.CreateCountReverseZeroes(starts));
        b.CreateStore(b.CreateSelect(b.CreateIsNull(starts), b.CreateLoad(sizeTy, startVar), lastStart), startVar);
        b.CreateStore(b.CreateLShr(runs, SZ_63), carryVar);
        b.CreateStore(b.CreateLShr(marks, SZ_63), lastMarkVar);
    });
    //  At a non-final call, end is a multiple of 64, so these are the bits at end - 1.
    Value * const lastRunBit = b.CreateLoad(sizeTy, carryVar);
    Value * const openRun = b.CreateAnd(lastRunBit, b.CreateXor(b.CreateLoad(sizeTy, lastMarkVar), ONE));
    Value * const complete = b.CreateSelect(b.CreateOr(isFinal, b.CreateIsNull(openRun)), end, b.CreateLoad(sizeTy, startVar));

    //  2. Copy [pending, complete) from Data to Sorted, leaving the positions before
    //  pending in its block as they are (they have already been produced).
    BasicBlock * const copyRegion = b.CreateBasicBlock("copyRegion");
    BasicBlock * const regionCopied = b.CreateBasicBlock("regionCopied");
    b.CreateCondBr(b.CreateICmpULT(pending, complete), copyRegion, regionCopied);
    b.SetInsertPoint(copyRegion);
    Value * const firstBlock = dataBlock;
    Value * const limitBlock = b.CreateAdd(b.CreateUDiv(b.CreateSub(complete, ONE), SZ_BW), ONE);
    forLoop(firstBlock, limitBlock, [&](Value * const blk) {
        Value * const from = b.CreateSelect(b.CreateICmpEQ(blk, firstBlock), b.CreateURem(pending, SZ_BW), ZERO);
        Value * const mask = b.bitblock_mask_from(from);
        for (unsigned m = 0; m < mDataBits; m++) {
            Value * const in = b.CreateBlockAlignedLoad(blockTy, b.getInputStreamBlockPtr("Data", b.getSize(m), b.CreateSub(blk, dataBlock)));
            Value * const outPtr = b.getOutputStreamBlockPtr("Sorted", b.getSize(m), b.CreateSub(blk, sortedBlock));
            Value * const old = b.CreateBlockAlignedLoad(blockTy, outPtr);
            b.CreateBlockAlignedStore(b.CreateOr(b.CreateAnd(old, b.CreateNot(mask)), b.CreateAnd(in, mask)), outPtr);
        }
    });
    b.CreateBr(regionCopied);
    b.SetInsertPoint(regionCopied);

    //  Sort the run [s, e]: gather (key, item) pairs, packed as key << M | item, sort
    //  them with a stable binary radix sort (one stable partition per key bit, low bit
    //  first), and scatter the items to Sorted.
    Value * const indexVar = b.CreateAllocaAtEntryPoint(sizeTy);
    Value * const onesVar = b.CreateAllocaAtEntryPoint(sizeTy);
    auto sortRun = [&](Value * const s, Value * const e) {
        Value * const n = b.CreateAdd(b.CreateSub(e, s), ONE);
        Value * const scratch = b.CreateCacheAlignedMalloc(sizeTy, b.CreateMul(n, b.getSize(2)));
        Value * src = scratch;
        Value * dst = b.CreateGEP(sizeTy, scratch, n);
        forLoop(ZERO, n, [&](Value * const j) {
            Value * const pos = b.CreateAdd(s, j);
            Value * entry = ZERO;
            for (unsigned k = 0; k < mKeyBits; k++) {
                entry = b.CreateOr(entry, b.CreateShl(loadBit("Keys", k, pos, keysBlock), b.getSize(mDataBits + k)));
            }
            for (unsigned m = 0; m < mDataBits; m++) {
                entry = b.CreateOr(entry, b.CreateShl(loadBit("Data", m, pos, dataBlock), b.getSize(m)));
            }
            b.CreateStore(entry, b.CreateGEP(sizeTy, src, j));
        });
        for (unsigned k = 0; k < mKeyBits; k++) {
            Constant * const keyBit = b.getSize(mDataBits + k);
            //  Entries with the bit clear go first, in order, then those with it set.
            b.CreateStore(ZERO, onesVar);
            forLoop(ZERO, n, [&](Value * const j) {
                Value * const bit = b.CreateAnd(b.CreateLShr(b.CreateLoad(sizeTy, b.CreateGEP(sizeTy, src, j)), keyBit), ONE);
                b.CreateStore(b.CreateAdd(b.CreateLoad(sizeTy, onesVar), bit), onesVar);
            });
            Value * const zeros = b.CreateSub(n, b.CreateLoad(sizeTy, onesVar));
            b.CreateStore(ZERO, indexVar);
            b.CreateStore(zeros, onesVar);
            forLoop(ZERO, n, [&](Value * const j) {
                Value * const entry = b.CreateLoad(sizeTy, b.CreateGEP(sizeTy, src, j));
                Value * const bit = b.CreateAnd(b.CreateLShr(entry, keyBit), ONE);
                Value * const zeroIndex = b.CreateLoad(sizeTy, indexVar);
                Value * const oneIndex = b.CreateLoad(sizeTy, onesVar);
                Value * const isOne = b.CreateICmpNE(bit, ZERO);
                b.CreateStore(entry, b.CreateGEP(sizeTy, dst, b.CreateSelect(isOne, oneIndex, zeroIndex)));
                b.CreateStore(b.CreateAdd(oneIndex, bit), onesVar);
                b.CreateStore(b.CreateAdd(zeroIndex, b.CreateXor(bit, ONE)), indexVar);
            });
            std::swap(src, dst);
        }
        forLoop(ZERO, n, [&](Value * const j) {
            Value * const pos = b.CreateAdd(s, j);
            Value * const entry = b.CreateLoad(sizeTy, b.CreateGEP(sizeTy, src, j));
            Value * const offset = b.CreateURem(pos, SZ_64);
            Value * const posMask = b.CreateShl(ONE, offset);
            for (unsigned m = 0; m < mDataBits; m++) {
                Value * const ptr = wordPtr(true, "Sorted", m, pos, sortedBlock);
                Value * const bit = b.CreateShl(b.CreateAnd(b.CreateLShr(entry, b.getSize(m)), ONE), offset);
                Value * const word = b.CreateLoad(sizeTy, ptr);
                b.CreateStore(b.CreateOr(b.CreateAnd(word, b.CreateNot(posMask)), bit), ptr);
            }
        });
        b.CreateFree(scratch);
    };

    //  3. Sort each misordered run of the complete region.  Misordered marks are at
    //  run ends; runs ending before runsPos were handled by earlier calls.
    BasicBlock * const scanMarks = b.CreateBasicBlock("scanMarks");
    BasicBlock * const marksDone = b.CreateBasicBlock("marksDone");
    b.CreateCondBr(b.CreateICmpULT(runsPos, complete), scanMarks, marksDone);
    b.SetInsertPoint(scanMarks);
    b.CreateStore(priorRunBit, carryVar);
    b.CreateStore(pending, startVar);
    forLoop(ZERO, numWords(runsPos, complete), [&](Value * const w) {
        Value * const pos = b.CreateAdd(runsPos, b.CreateMul(w, SZ_64));
        Value * const runs = loadMaskedWord("Runs", pos, runsBlock, complete);
        Value * const marks = loadMaskedWord("Misordered", pos, runsBlock, complete);
        Value * const prior = b.CreateOr(b.CreateShl(runs, ONE), b.CreateLoad(sizeTy, carryVar));
        Value * const starts = b.CreateAnd(runs, b.CreateNot(prior));
        b.CreateStore(b.CreateLShr(runs, SZ_63), carryVar);
        //  Visit run starts and misordered marks in position order.
        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const eventLoop = b.CreateBasicBlock("eventLoop");
        BasicBlock * const eventsDone = b.CreateBasicBlock("eventsDone");
        Value * const events = b.CreateOr(starts, marks);
        b.CreateCondBr(b.CreateIsNull(events), eventsDone, eventLoop);
        b.SetInsertPoint(eventLoop);
        PHINode * const remaining = b.CreatePHI(sizeTy, 2);
        remaining->addIncoming(events, entry);
        Value * const bitPos = b.CreateCountForwardZeroes(remaining, "", true);
        Value * const eventPos = b.CreateAdd(pos, bitPos);
        Value * const isStart = b.CreateICmpNE(b.CreateAnd(b.CreateLShr(starts, bitPos), ONE), ZERO);
        b.CreateStore(b.CreateSelect(isStart, eventPos, b.CreateLoad(sizeTy, startVar)), startVar);
        BasicBlock * const sortIt = b.CreateBasicBlock("sortRun");
        BasicBlock * const nextEvent = b.CreateBasicBlock("nextEvent");
        Value * const isMark = b.CreateICmpNE(b.CreateAnd(b.CreateLShr(marks, bitPos), ONE), ZERO);
        b.CreateCondBr(isMark, sortIt, nextEvent);
        b.SetInsertPoint(sortIt);
        sortRun(b.CreateLoad(sizeTy, startVar), eventPos);
        b.CreateBr(nextEvent);
        b.SetInsertPoint(nextEvent);
        Value * const rest = b.CreateAnd(remaining, b.CreateSub(remaining, ONE));
        remaining->addIncoming(rest, nextEvent);
        b.CreateCondBr(b.CreateIsNull(rest), eventsDone, eventLoop);
        b.SetInsertPoint(eventsDone);
    });
    b.CreateBr(marksDone);
    b.SetInsertPoint(marksDone);

    b.setScalarField("PriorRunBit", lastRunBit);
    b.setProcessedItemCount("Keys", complete);
    b.setProcessedItemCount("Data", complete);
    b.setProducedItemCount("Sorted", complete);
}

void SeqSortRuns(PipelineBuilder & P, StreamSet * Runs, StreamSet * Keys, StreamSet * Data, StreamSet * Sorted) {
    StreamSet * const Misordered = P.CreateStreamSet(1);
    P.CreateKernelCall<Misorder_Check>(Runs, Keys, Misordered);
    SHOW_STREAM(Misordered);
    P.CreateKernelCall<SeqRunSort>(Runs, Misordered, Keys, Data, Sorted);
}

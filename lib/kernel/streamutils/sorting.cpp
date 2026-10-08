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

using boost::intrusive::detail::ceil_log2;
using namespace kernel;
using namespace pablo;

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

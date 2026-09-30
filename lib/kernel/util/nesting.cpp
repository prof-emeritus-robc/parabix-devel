/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/util/nesting.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <vector>
#include <llvm/Support/CommandLine.h>

using namespace llvm;
using namespace pablo;

namespace kernel {

static cl::OptionCategory NestingOptions("Nesting Kernel Flags", "These options control printing for the nesting kernel");
static cl::opt<bool> PrintStreams("print-nesting-streams", cl::desc("Print stream values"), cl::init(false), cl::cat(NestingOptions));

NestingDepth::NestingDepth(LLVMTypeSystemInterface & ts,
                   StreamSet * brackets,
                           StreamSet * depth, StreamSet * errs,
                           unsigned maxDepth)
    : PabloKernel(ts, "NestingDepth" +
                     std::to_string(maxDepth) +
                     (PrintStreams ? "_pk" : ""),
                  {Binding{"brackets", brackets}},
                  {Binding{"nestingDepth", depth},
                   Binding{"errs", errs, FixedRate(), Add1()}}),
    mMaxDepth(maxDepth),
    mNestingDepthBits(ceil_log2(maxDepth + 1)) {}

void NestingDepth::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> brackets = getInputStreamSet("brackets");
    std::vector<Var *> nestingDepthVar(mNestingDepthBits);
    for (unsigned i = 0; i < mNestingDepthBits; i++) {
        std::string vname = "nestingDepth" + std::to_string(i);
        nestingDepthVar[i] = pb.createVar(vname, pb.createZeroes());
    }
    PabloAST * LBrak = brackets[0];
    PabloAST * RBrak = brackets[1];
    PabloAST * all_brackets = pb.createOr(LBrak, RBrak, "all_brackets");
    PabloAST * bscan = pb.createAdvanceThenScanTo(LBrak, all_brackets);
    Var * closed = pb.createVar("closed", pb.createAnd(bscan, RBrak));
    Var * errs = pb.createVar("errs", pb.createAtEOF(bscan));
    Var * pendingL = pb.createVar("pendingL", pb.createAnd(bscan, LBrak));
    PabloAST * span =
      pb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, {LBrak, bscan});
    // initialize nesting to 1 for positions in span
    pb.createAssign(nestingDepthVar[0], span);
    // Set up a while loop, with loop body wb.
    auto wb = pb.createScope();
    BixNumCompiler bnc(wb);
    PabloAST * unmatchedR = wb.createAnd(RBrak, wb.createNot(closed), "unmatchedR");
    PabloAST * inPlay = wb.createOr(pendingL, unmatchedR, "inPlay");
    bscan = wb.createAdvanceThenScanTo(pendingL, inPlay);
    span = wb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, {pendingL, bscan});
    BixNum increment(1, span);
    BixNum nesting(mNestingDepthBits);
    for (unsigned i = 0; i < mNestingDepthBits; i++) {
        nesting[i] = nestingDepthVar[i];
    }
    nesting = bnc.AddModular(nesting, increment);
    PabloAST * atMaxDepth = bnc.EQ(nesting, mMaxDepth);
    for (unsigned i = 0; i < mNestingDepthBits; i++) {
        wb.createAssign(nestingDepthVar[i], nesting[i]);
    }
    wb.createAssign(closed, wb.createOr(closed, wb.createAnd(bscan, RBrak)));
    PabloAST * nextPending = wb.createAnd(bscan, LBrak, "nextPending");
    PabloAST * tooDeep = wb.createAnd(nextPending, atMaxDepth, "tooDeep");
    wb.createAssign(errs, wb.createOr3(errs, tooDeep, wb.createAtEOF(bscan)));
    wb.createAssign(pendingL, wb.createAnd(nextPending, wb.createNot(tooDeep)));
    While * loop = pb.createWhile(pendingL, wb);
    loop->setRegular(false);
    PabloAST * unmatchedR_err = pb.createAnd(RBrak, pb.createNot(closed), "unmatchedR_err");
    Var * ND = getOutputStreamVar("nestingDepth");
    for (unsigned i = 0; i < mNestingDepthBits; i++) {
        if (PrintStreams) {
           pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {nestingDepthVar[i]});
        }
        pb.createAssign(pb.createExtract(ND, pb.getInteger(i)), nestingDepthVar[i]);
    }
    Var * errout = getOutputStreamVar("errs");
    pb.createAssign(pb.createExtract(errout, pb.getInteger(0)),
                    pb.createOr(errs, unmatchedR_err));
}

}

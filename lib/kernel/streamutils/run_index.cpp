/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/run_index.h>
#include <kernel/core/kernel_builder.h>
#include <pablo/pablo.h>

using namespace pablo;

namespace kernel {

Bindings RunIndexOutputBindings(StreamSet * runIndex, StreamSet * overflow) {
    if (overflow == nullptr) return {Binding{"runIndex", runIndex}};
    return {Binding{"runIndex", runIndex}, Binding{"overflow", overflow}};
}
    



RunIndex::RunIndex(LLVMTypeSystemInterface & ts,
                   StreamSet * const runMarks, StreamSet * runIndex, StreamSet * overflow, Kind kind, Numbering n)
    : PabloKernel(ts, [&]() -> std::string {
                        std::string tmp;
                        llvm::raw_string_ostream out(tmp);
                        out << "RunIndex-" << runIndex->getNumElements();
                        if (overflow) {
                            out << 'O';
                        }
                        if (kind == Kind::RunOf1) {
                            out << 'I';
                        }
                        if (n == Numbering::RunPlus1) {
                            out << 'A';
                        }
                        out.flush();
                        return tmp;
                    }(),
           // input
{Binding{"runMarks", runMarks}},
           // output
RunIndexOutputBindings(runIndex, overflow)),
mIndexCount(runIndex->getNumElements()),
mOverflow(overflow != nullptr),
mRunKind(kind),
mNumbering(n) {
    assert(mIndexCount > 0);
    assert(mIndexCount <= 5);
}

void RunIndex::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    Var * runMarksVar = pb.createExtract(getInputStreamVar("runMarks"), pb.getInteger(0));
    PabloAST * runMarks = mRunKind == Kind::RunOf0 ? pb.createInFile(pb.createNot(runMarksVar)) : runMarksVar;
    PabloAST * runStart = pb.createAnd(runMarks, pb.createNot(pb.createAdvance(runMarks, 1)), "runStart");
    PabloAST * selectZero = runMarks;
    PabloAST * outputEnable = runMarks;
    Var * runIndexVar = getOutputStreamVar("runIndex");
    std::vector<PabloAST *> runIndex(mIndexCount);
    PabloAST * even = nullptr;
    PabloAST * overflow = nullptr;
    if (mOverflow) {
        overflow = pb.createAnd(pb.createAdvance(runMarks, 1), runMarks);
    }
    for (unsigned i = 0; i < mIndexCount; i++) {
        switch (i) {
            case 0: even = pb.createRepeat(1, pb.getInteger(0x55, 8)); break;
            case 1: even = pb.createRepeat(1, pb.getInteger(0x33, 8)); break;
            case 2: even = pb.createRepeat(1, pb.getInteger(0x0F, 8)); break;
            case 3: even = pb.createRepeat(1, pb.getInteger(0x00FF, 16)); break;
            case 4: even = pb.createRepeat(1, pb.getInteger(0x0000FFFF, 32)); break;
            case 5: even = pb.createRepeat(1, pb.getInteger(0x00000000FFFFFFFF, 64)); break;
        }
        PabloAST * odd = pb.createNot(even);
        PabloAST * evenStart = pb.createAnd(even, runStart);
        PabloAST * oddStart = pb.createAnd(odd, runStart);
        PabloAST * idx = pb.createOr(pb.createAnd(pb.createMatchStar(evenStart, runMarks), odd),
                                     pb.createAnd(pb.createMatchStar(oddStart, runMarks), even));
        if (mNumbering == Numbering::RunOnly) {
            idx = pb.createAnd(idx, selectZero);
        }
        for (unsigned j = 0; j < i; j++) {
            idx = pb.createOr(idx, pb.createAdvance(idx, 1<<j));
        }
        runIndex[i] = pb.createAnd(idx, outputEnable, "runidx[" + std::to_string(i) + "]");
        pb.createAssign(pb.createExtract(runIndexVar, pb.getInteger(i)), runIndex[i]);
        if (i < mIndexCount - 1) {
            selectZero = pb.createAnd(selectZero, pb.createNot(idx), "selectZero");
            outputEnable = pb.createAnd(outputEnable, pb.createAdvance(outputEnable, 1<<i), "outputEnable");
        }
        if (mOverflow) {
            overflow = pb.createAnd(overflow, pb.createAdvance(overflow, 1<<i), "overflow");
        }
    }
    if (mOverflow) {
        pb.createAssign(pb.createExtract(getOutputStreamVar("overflow"), pb.getInteger(0)), overflow);
    }
}
}

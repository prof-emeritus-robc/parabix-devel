/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/position_map.h>

#include <kernel/bitwise/bixlogic.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>

using namespace pablo;

namespace kernel {

StreamSet * PositionMapper::mapped(StreamSet * stream, const PositionMap & map) {
    for (const PositionMap::Edit & e : map.edits) stream = mapped(stream, e);
    return stream;
}

StreamSet * PositionMapper::mapped(StreamSet * stream, const PositionMap::Edit & e) {
    Op op = Filter;
    if (e.keep == nullptr) {
        switch (e.inserted) {
            case PositionMap::Inserted::Inherit: op = Inherit; break;
            case PositionMap::Inserted::InheritNext: op = InheritNext; break;
            case PositionMap::Inserted::In: op = In; break;
            case PositionMap::Inserted::Out: op = Out; break;
        }
    }
    StreamSet * const by = e.keep ? e.keep : e.spread;
    const unsigned run = (op == InheritNext) ? e.run : 0;
    auto f = mCache.find({stream, by, op, run});
    if (f != mCache.end()) return f->second;
    StreamSet * result = mP.CreateStreamSet(1);
    if (op == Filter) {
        FilterByMask(mP, e.keep, stream, result);
    } else {
        SpreadByMask(mP, e.spread, stream, result);
        if (op == Inherit) {
            StreamSet * const inherited = mP.CreateStreamSet(1);
            mP.CreateKernelCall<kernel::InheritPrevious>(e.spread, result, inherited);
            result = inherited;
        } else if (op == InheritNext) {
            StreamSet * const inherited = mP.CreateStreamSet(1);
            mP.CreateKernelCall<kernel::InheritNext>(e.spread, result, run, inherited);
            result = inherited;
        } else if (op == In) {
            StreamSet * const combined = mP.CreateStreamSet(1);
            OrCombine(mP, result, complement(e.spread), combined);
            result = combined;
        }
    }
    mCache[{stream, by, op, run}] = result;
    return result;
}

StreamSet * PositionMapper::complement(StreamSet * stream) {
    StreamSet *& c = mCache[{stream, nullptr, Complement, 0}];
    if (c == nullptr) {
        c = mP.CreateStreamSet(1);
        Invert(mP, stream, c);
    }
    return c;
}

InheritPrevious::InheritPrevious(LLVMTypeSystemInterface & ts, StreamSet * SpreadMask, StreamSet * Spread, StreamSet * Inherited)
: PabloKernel(ts, "InheritPrevious",
              {Binding{"SpreadMask", SpreadMask}, Binding{"Spread", Spread}}, {Binding{"Inherited", Inherited}}) {}

void InheritPrevious::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const inserted = pb.createInFile(pb.createNot(getInputStreamSet("SpreadMask")[0]));
    PabloAST * const spread = getInputStreamSet("Spread")[0];
    //  The runs of inserted positions after the positions with the bit.
    PabloAST * const follow = pb.createAnd(pb.createAdvance(spread, 1), inserted);
    PabloAST * const runs = pb.createAnd(pb.createMatchStar(follow, inserted), inserted);
    writeOutputStreamSet("Inherited", std::vector<PabloAST *>{pb.createOr(spread, runs)});
}

InheritNext::InheritNext(LLVMTypeSystemInterface & ts, StreamSet * SpreadMask, StreamSet * Spread, unsigned run, StreamSet * Inherited)
: PabloKernel(ts, "InheritNext" + std::to_string(run),
              {Binding{"SpreadMask", SpreadMask, FixedRate(1), LookAhead(run)}, Binding{"Spread", Spread, FixedRate(1), LookAhead(run)}},
              {Binding{"Inherited", Inherited}}),
  mRun(run) {}

void InheritNext::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const spreadMask = getInputStreamSet("SpreadMask")[0];
    PabloAST * const spread = getInputStreamSet("Spread")[0];
    PabloAST * m = spread;
    //  (Inserted at each position up to j - 1 ahead, and the bit j ahead.)
    PabloAST * inserted = pb.createNot(spreadMask);
    for (unsigned j = 1; j <= mRun; j++) {
        m = pb.createOr(m, pb.createAnd(inserted, pb.createLookahead(spread, j)));
        inserted = pb.createAnd(inserted, pb.createNot(pb.createLookahead(spreadMask, j)));
    }
    writeOutputStreamSet("Inherited", std::vector<PabloAST *>{pb.createInFile(m)});
}

}

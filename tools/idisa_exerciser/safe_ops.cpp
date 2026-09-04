/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "safe_ops.h"

#include "idisa_exerciser.h"

using namespace std;
using namespace llvm;
using namespace kernel;

Value *SafeURem(KernelBuilder &b, unsigned fw, Value *vec, unsigned divBy) {
    assert(fw <= 64);
    assert((divBy & (divBy - 1)) == 0);
    unsigned blockFW = max<unsigned>(fw, 8);
    uint64_t blockMask = fw - 1;
    if (fw < 8) {
        blockMask = blockMask | (blockMask << 4);
        if (fw < 4) {
            blockMask = blockMask | (blockMask << 2);
            // If fw < 2, blockMask is degenerate and always 0
        }
    }
    return b.CreateAnd(b.fwCast(blockFW, vec), blockMask);
}

Value *SafeInsertElement(KernelBuilder &b, unsigned fw, Value *vec, Value *elt, uint64_t idx) {
    unsigned lw = b.getLaneWidth();
    if (fw < lw) {
        unsigned lw = b.getLaneWidth();
        assert(lw <= 64);
        assert(lw >= fw);
        assert(lw % fw == 0);
        Value *laneVec = b.fwCast(lw, vec);
        unsigned bitIdx = idx * fw;
        unsigned laneIdx = bitIdx / lw;
        uint64_t laneShift = bitIdx % lw;
        uint64_t fMask = (uint64_t(1) << fw) - 1;
        uint64_t eltMask = fMask << laneShift;
        Value *lane = b.CreateExtractElement(laneVec, laneIdx);
        Value *maskedElt = b.CreateZExt(b.CreateAnd(elt, fMask), b.getLaneTy());
        Value *laneElt = b.CreateShl(maskedElt, laneShift);
        Value *newLane = b.CreateOr(b.CreateAnd(lane, ~eltMask), laneElt);
        return b.CreateInsertElement(laneVec, newLane, laneIdx);
    }
    return b.CreateInsertElement(b.fwCast(fw, vec), elt, idx);
}

Value *SafeInsertElement(KernelBuilder &b, unsigned fw, Value *vec, Value *elt, Value *idx) {
    unsigned lw = b.getLaneWidth();
    if (fw < lw) {
        assert(lw <= 64);
        assert(lw >= fw);
        assert(lw % fw == 0);
        Value *laneVec = b.fwCast(lw, vec);
        Value *bitIdx = b.CreateMul(b.CreateZExtOrTrunc(idx, b.getIntNTy(lw)), b.getIntN(lw, fw));
        Value *laneIdx = b.CreateUDiv(bitIdx, b.getIntN(lw, lw));
        Value *laneShift = b.CreateURem(bitIdx, b.getIntN(lw, lw));
        uint64_t fMask = (uint64_t(1) << fw) - 1;
        Value *eltMask = b.CreateShl(b.getIntN(lw, fMask), laneShift);
        Value *lane = b.CreateExtractElement(laneVec, laneIdx);
        Value *maskedElt = b.CreateZExt(b.CreateAnd(elt, fMask), b.getLaneTy());
        Value *laneElt = b.CreateShl(maskedElt, laneShift);
        Value *newLane = b.CreateOr(b.CreateAnd(lane, b.CreateNot(eltMask)), laneElt);
        return b.CreateInsertElement(laneVec, newLane, laneIdx);
    }
    return b.CreateInsertElement(b.fwCast(fw, vec), elt, idx);
}

Value *SafeExtractElement(KernelBuilder &b, unsigned fw, Value *vec, uint64_t idx) {
    unsigned lw = b.getLaneWidth();
    if (fw < lw) {
        unsigned lw = b.getLaneWidth();
        assert(lw <= 64);
        assert(lw >= fw);
        assert(lw % fw == 0);
        Value *laneVec = b.fwCast(lw, vec);
        uint64_t laneIdx = (idx * fw) / lw;
        Value *lane = b.CreateExtractElement(laneVec, laneIdx);
        uint64_t fMask = (uint64_t(1) << fw) - 1;
        Value *laneField = b.CreateAnd(b.CreateLShr(lane, (idx * fw) % lw), fMask);
        return b.CreateTrunc(laneField, b.getIntNTy(fw));
    }
    return b.CreateExtractElement(b.fwCast(fw, vec), idx);
}

Value *SafeExtractElement(KernelBuilder &b, unsigned fw, Value *vec, Value *idx) {
    unsigned lw = b.getLaneWidth();
    if (fw < lw) {
        unsigned lw = b.getLaneWidth();
        assert(lw <= 64);
        assert(lw >= fw);
        assert(lw % fw == 0);
        Value *laneVec = b.fwCast(lw, vec);
        Value *bitIdx = b.CreateMul(b.CreateZExtOrTrunc(idx, b.getIntNTy(lw)), b.getIntN(lw, fw));
        Value *laneIdx = b.CreateUDiv(bitIdx, b.getIntN(lw, lw));
        Value *lane = b.CreateExtractElement(laneVec, laneIdx);
        Value *fMask = b.getIntN(lw, (uint64_t(1) << fw) - 1);
        Value *laneShift = b.CreateURem(bitIdx, b.getIntN(lw, lw));
        Value *laneField = b.CreateAnd(b.CreateLShr(lane, laneShift), fMask);
        return b.CreateTrunc(laneField, b.getIntNTy(fw));
    }
    return b.CreateExtractElement(b.fwCast(fw, vec), idx);
}

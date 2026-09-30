/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#include "editd_gpu_kernel.h"
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Module.h>
#include <iostream>
using namespace llvm;

namespace kernel {

void bitblock_advance_ci_co(KernelBuilder & b, Value * val, unsigned shift, Value * strideCarryArr, unsigned carryIdx, std::vector<std::vector<Value *>> & adv, std::vector<std::vector<int>> & calculated, int i, int j){
    if (!calculated[i][j]) {
        Type * bbTy = b.getBitBlockType();
        Value * ptr = b.CreateGEP(bbTy, strideCarryArr, {b.getInt32(0), b.getInt32(carryIdx)});
        Value * ci = b.CreateLoad(bbTy, ptr);
        std::pair<Value *, Value *> rslt = b.bitblock_advance(val, ci, shift);
        b.CreateStore(std::get<0>(rslt), ptr);
        adv[i][j] = std::get<1>(rslt);
        calculated[i][j] = 1;
    }
}

void reset_to_zero(std::vector<std::vector<int>> & calculated){
    for (auto & sub : calculated) {
        std::fill(sub.begin(), sub.end(), 0);
    }
}

void editdGPUKernel::generateDoBlockMethod(KernelBuilder & b) {

    IntegerType * const int32ty = b.getInt32Ty();
    IntegerType * const int8ty = b.getInt8Ty();
    Value * groupLen = b.getInt32((mPatternLen + 1) * mGroupSize);
    Value * pattPos = b.getInt32(0);
    Value * pattBuf = b.getScalarField("pattStream");
    Value * strideCarryArr = b.getScalarField("strideCarry");

    unsigned carryIdx = 0;

    std::vector<std::vector<Value *>> e(mPatternLen + 1, std::vector<Value *>(mEditDistance + 1));
    std::vector<std::vector<Value *>> adv(mPatternLen, std::vector<Value *>(mEditDistance + 1));
    std::vector<std::vector<int>> calculated(mPatternLen, std::vector<int>(mEditDistance + 1, 0));

    Type * bbTy = b.getBitBlockType();

    Module * m = b.getModule();
    FunctionType * bidTy = FunctionType::get(int32ty, false);
    Function * const bidFunc = Function::Create(bidTy, Function::ExternalLinkage, "llvm.nvvm.read.ptx.sreg.ctaid.x", m);
    Value * bid = b.CreateCall(bidTy, bidFunc, {});
    Value * pattStartPtr = b.CreateGEP(bbTy, pattBuf, b.CreateMul(groupLen, bid));

    for(unsigned j = 0; j <= mEditDistance; j++){
        e[mPatternLen][j] = b.allZeroes();
    }

    for(unsigned j = 1; j <= mEditDistance; j++){
        e[0][j] = b.allOnes();
    }

    for(unsigned g = 0; g < mGroupSize; g++){
        Value * pattCh = b.CreateLoad(bbTy, b.CreateGEP(bbTy, pattStartPtr, pattPos));
        Value * pattIdx = b.CreateAnd(b.CreateLShr(pattCh, 1), ConstantInt::get(int8ty, 3));
        Value * pattStream = b.loadInputStreamBlock("CCStream", b.CreateZExt(pattIdx, int32ty));
        pattPos = b.CreateAdd(pattPos, ConstantInt::get(int32ty, 1));
        e[0][0] = pattStream;
        for(unsigned i = 1; i < mPatternLen; i++){
            Value * pattCh = b.CreateLoad(bbTy, b.CreateGEP(bbTy, pattStartPtr, pattPos));
            pattIdx = b.CreateAnd(b.CreateLShr(pattCh, 1), ConstantInt::get(int8ty, 3));
            pattStream = b.loadInputStreamBlock("CCStream", b.CreateZExt(pattIdx, int32ty));
            pattPos = b.CreateAdd(pattPos, ConstantInt::get(int32ty, 1));
            bitblock_advance_ci_co(b, e[i-1][0], 1, strideCarryArr, carryIdx++, adv, calculated, i-1, 0);
            e[i][0] = b.CreateAnd(adv[i-1][0], pattStream);
            for(unsigned j = 1; j<= mEditDistance; j++){
                bitblock_advance_ci_co(b, e[i-1][j], 1, strideCarryArr, carryIdx++, adv, calculated, i-1, j);
                bitblock_advance_ci_co(b, e[i-1][j-1], 1, strideCarryArr, carryIdx++, adv, calculated, i-1, j-1);
                bitblock_advance_ci_co(b, e[i][j-1], 1, strideCarryArr, carryIdx++, adv, calculated, i, j-1);
                Value * tmp1 = b.CreateAnd(adv[i-1][j], pattStream);
                Value * tmp2 = b.CreateAnd(adv[i-1][j-1], b.CreateNot(pattStream));
                Value * tmp3 = b.CreateOr(adv[i][j-1], e[i-1][j-1]);
                e[i][j] = b.CreateOr(b.CreateOr(tmp1, tmp2), tmp3);
            }
        }
        e[mPatternLen][0] = b.CreateOr(e[mPatternLen][0], e[mPatternLen-1][0]);
        for(unsigned j = 1; j<= mEditDistance; j++){
            e[mPatternLen][j] = b.CreateOr(e[mPatternLen][j], b.CreateAnd(e[mPatternLen - 1][j], b.CreateNot(e[mPatternLen - 1][j - 1])));
        }
        pattPos = b.CreateAdd(pattPos, ConstantInt::get(int32ty, 1));
        reset_to_zero(calculated);
    }

    for(unsigned j = 0; j<= mEditDistance; j++){
        b.storeOutputStreamBlock("ResultStream", b.getInt32(j), e[mPatternLen][j]);
    }
}

void editdGPUKernel::generateFinalBlockMethod(KernelBuilder & b, Value * remainingBytes) {
    b.setScalarField("EOFmask", b.bitblock_mask_from(remainingBytes));
    RepeatDoBlockLogic(b);
}

editdGPUKernel::editdGPUKernel(LLVMTypeSystemInterface & ts, unsigned dist, unsigned pattLen, unsigned groupSize) :
BlockOrientedKernel(ts, "editd_gpu",
{Binding{ts.getStreamSetTy(4), "CCStream"}},
{Binding{ts.getStreamSetTy(dist + 1), "ResultStream"}},
{Binding{PointerType::get(ts.getContext(), 1), "pattStream"},
Binding{PointerType::getUnqual(ts.getContext()), "strideCarry"}},
{},
{InternalScalar{ScalarType::NonPersistent, ts.getBitBlockType(), "EOFmask"}})
, mEditDistance(dist)
, mPatternLen(pattLen)
, mGroupSize(groupSize) {
}

}

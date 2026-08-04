#include <idisa/idisa_arm_builder.h>

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAArch64.h>
#include <llvm/IR/Module.h>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(20, 0, 0)
#define getOrInsertDeclaration getDeclaration
#endif

using namespace llvm;

namespace {

llvm::GlobalVariable * getOrCreateByteCompressTable(llvm::Module * mod, llvm::LLVMContext & C) {
    const char * const name = "__idisa_arm_byte_compress_table";
    if (llvm::GlobalVariable * existing = mod->getGlobalVariable(name)) {
        return existing;
    }
    llvm::IntegerType * i8Ty = llvm::IntegerType::getInt8Ty(C);
    llvm::FixedVectorType * entryTy = llvm::FixedVectorType::get(i8Ty, 16);
    llvm::SmallVector<llvm::Constant *, 256> entries(256);
    for (unsigned m = 0; m < 256; m++) {
        llvm::Constant * lanes[16];
        unsigned pos = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            if (m & (1u << bit)) {
                lanes[pos++] = llvm::ConstantInt::get(i8Ty, bit);
            }
        }
        for (unsigned i = pos; i < 16; i++) {
            lanes[i] = llvm::ConstantInt::get(i8Ty, 16); // out-of-range => TBL yields 0
        }
        entries[m] = llvm::ConstantVector::get(llvm::ArrayRef<llvm::Constant *>(lanes, 16));
    }
    llvm::ArrayType * tableTy = llvm::ArrayType::get(entryTy, 256);
    llvm::Constant * tableInit = llvm::ConstantArray::get(tableTy, entries);
    return new llvm::GlobalVariable(*mod, tableTy, /*isConstant=*/true,
                                     llvm::GlobalValue::PrivateLinkage, tableInit, name);
}

} // anonymous namespace
namespace IDISA {

std::string IDISA_ARM_Builder::getBuilderUniqueName() { return mBitBlockWidth != 128 ? "ARM_" + std::to_string(mBitBlockWidth) : "ARM";}

Value* IDISA_ARM_Builder::simd_popcount(unsigned fw, Value * a) {
    if (getVectorBitWidth(a) != ARM_width || fw < 8 || fw % 8 != 0) {
        return IDISA_Builder::simd_popcount(fw, a);
    }

    // There is a CNT instruction offered by NEON that counts set bits in each byte.
    // It only exists for vectors of i8, i.e. <8 x i8> and <16 x i8>. For some reason,
    // LLVM exposes an instrinsic for this instruction but fails to select it
    // during compilation. As a workaround we use the LLVM ctpop instrinsic which does
    // the right thing and emits CNT.
    Value* countInBytes = CreatePopcount(fwCast(8, a));

    if (fw == 8) { // if `a` is a vector of i8 then we're already done
        return countInBytes;
    } else if (fw == 128) {
        auto addv = Intrinsic::getOrInsertDeclaration(getModule(),
                                              Intrinsic::aarch64_neon_uaddv,
                                              { getInt32Ty(), FixedVectorType::get(getInt8Ty(), 16) });

        auto popcnt = CreateCall(addv->getFunctionType(),
                                     addv,
                                     fwCast(8, countInBytes));

        // It appears that when fw == 128 most calling code expects we return 2xi64
        return CreateInsertElement(fwCast(64, allZeroes()),
                                   CreateZExt(popcnt, getInt64Ty()),
                                   Constant::getNullValue(getInt32Ty()));
    } else {
        // addParirsW: pairwise widening add
        // Adds each pair of fields in a vector together and stores
        // the result in a vector whose fields are twice as wide as
        // the source vector
        auto addPairsW = [this](unsigned _fw, Value* _a) -> Value* {
            unsigned nElems = getVectorBitWidth(_a) / _fw;
            unsigned destFw = _fw * 2;
            unsigned destNElems = nElems / 2;

            auto low = CreateExtractVector(FixedVectorType::get(getIntNTy(_fw), nElems / 2),
                                           _a,
                                           ConstantInt::get(getInt64Ty(), 0));
            auto hi = CreateExtractVector(FixedVectorType::get(getIntNTy(_fw), nElems / 2),
                                           _a,
                                           ConstantInt::get(getInt64Ty(), nElems / 2));

            auto lowExt = CreateZExt(low, FixedVectorType::get(getIntNTy(destFw), destNElems));
            auto hiExt = CreateZExt(hi, FixedVectorType::get(getIntNTy(destFw), destNElems));

            auto addp = Intrinsic::getOrInsertDeclaration(getModule(),
                                                  Intrinsic::aarch64_neon_addp,
                                                  FixedVectorType::get(getIntNTy(destFw), destNElems));
            return fwCast(destFw, CreateCall(addp->getFunctionType(), addp, {lowExt, hiExt}));
        };

        // Add pairs together and widen each field until we have reduced
        // to the destination field width
        Value* result = countInBytes;
        for (unsigned thisFw = 8; thisFw < fw; thisFw <<= 1) {
            result = addPairsW(thisFw, result);
        }
        return result;
    }
}

Value * IDISA_ARM_Builder::simd_bitreverse(unsigned fw, Value * a) {

    if (fw < 8 || getVectorBitWidth(a) != ARM_width) {
        return IDISA_Builder::simd_bitreverse(fw, a);
    }

    // First reverse the bits in each byte
    auto rbit = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_sve_rbit, fwVectorType(fw));
    if (fw == 8) {
        return CreateCall(rbit->getFunctionType(), rbit, fwCast(8, a));
    }
    Function* refBytesInFields = nullptr;

    // Then reverse the bytes in each field
    if (fw == 64) {
        refBytesInFields = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_sve_revw);
    } else if (fw == 32) {
        refBytesInFields = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_sve_revh);
    } else if (fw == 16) {
        refBytesInFields = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_sve_revb);
    } else {
        return IDISA_Builder::simd_bitreverse(fw, a);
    }

    auto bitsInBytesRevsd = CreateCall(rbit->getFunctionType(), rbit, fwCast(8, a));
    return CreateCall(refBytesInFields->getFunctionType(), refBytesInFields, fwCast(fw, bitsInBytesRevsd));
}

Value * IDISA_ARM_Builder::mvmd_shuffle(unsigned fw, Value * data_table, Value * index_vector) {
    auto vec_width = getVectorBitWidth(data_table);
    if (vec_width == mNativeBitBlockWidth && fw == 8) {
        Function * shuf8Func = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_neon_tbl1, FixedVectorType::get(getInt8Ty(), 16));
        return fwCast(8, CreateCall(shuf8Func->getFunctionType(), shuf8Func, {fwCast(8, data_table), fwCast(8, simd_select_lo(fw, index_vector))}));
    }
    return IDISA_Builder::mvmd_shuffle(fw, data_table, index_vector);
}

Value * IDISA_ARM_Builder::mvmd_shuffle2(unsigned fw, Value * table0, Value * table1, Value * index_vector) {
    if (getVectorBitWidth(table0) == mNativeBitBlockWidth && fw == 8) {
        Function * shuf8Func = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_neon_tbl2, FixedVectorType::get(getInt8Ty(), 16));
        Value * rslt = CreateCall(shuf8Func->getFunctionType(), shuf8Func, {fwCast(8, table0), fwCast(8, table1), fwCast(8, index_vector)});
            return rslt;
    }
    return IDISA_Builder::mvmd_shuffle2(fw, table0, table1, index_vector);
}

Value * IDISA_ARM_Builder::expandFieldMaskToBytes(Value * select_mask, unsigned fw) {
    const unsigned fieldCount = 128 / fw;      // 8, 4, or 2 for fw=16/32/64
    const unsigned bytesPerField = fw / 8;     // 2, 4, or 8
    Value * mask = CreateZExtOrTrunc(select_mask, getIntNTy(fieldCount));
    Value * byteMask = ConstantInt::get(getInt16Ty(), 0);
    for (unsigned j = 0; j < fieldCount; j++) {
        Value * bit = CreateAnd(CreateLShr(mask, ConstantInt::get(getIntNTy(fieldCount), j)),
                                 ConstantInt::get(getIntNTy(fieldCount), 1));
        Value * bit16 = CreateZExt(bit, getInt16Ty());
        for (unsigned k = 0; k < bytesPerField; k++) {
            unsigned destBit = j * bytesPerField + k;
            Value * shifted = CreateShl(bit16, ConstantInt::get(getInt16Ty(), destBit));
            byteMask = CreateOr(byteMask, shifted);
        }
    }
    return byteMask;
}

// raw TBL1: indexes >= 16 yield zero lanes, unlike mvmd_shuffle which reduces them mod 16
Value * IDISA_ARM_Builder::tbl1(Value * table, Value * index_vector) {
    Function * fn = Intrinsic::getDeclaration(getModule(), Intrinsic::aarch64_neon_tbl1,
                                              FixedVectorType::get(getInt8Ty(), 16));
    return CreateCall(fn->getFunctionType(), fn, {fwCast(8, table), fwCast(8, index_vector)});
}

Value * IDISA_ARM_Builder::compressBytes(Value * a, Value * byteMask) {
    GlobalVariable * table = getOrCreateByteCompressTable(getModule(), getContext());
    Type * i32Ty = getInt32Ty();
    FixedVectorType * v16xi8Ty = FixedVectorType::get(getInt8Ty(), 16);

    Value * maskBits = byteMask;
    Value * lowMaskByte = CreateTrunc(maskBits, getInt8Ty());
    Value * highMaskByte = CreateTrunc(CreateLShr(maskBits, ConstantInt::get(getInt16Ty(), 8)), getInt8Ty());

    auto loadTableEntry = [&](Value * idxByte) -> Value * {
        Value * idx32 = CreateZExt(idxByte, i32Ty);
        Value * gep = CreateInBoundsGEP(table->getValueType(), table,
                                         {ConstantInt::get(i32Ty, 0), idx32});
        return CreateLoad(v16xi8Ty, gep);
    };

    Value * lowIdx = loadTableEntry(lowMaskByte);

    Value * highIdxBase = loadTableEntry(highMaskByte);
    Value * highIdx = simd_add(8, highIdxBase, getSplat(16, getInt8(8)));

    Value * lowCompressed = tbl1(a, lowIdx);
    Value * highCompressed = tbl1(a, highIdx);

    Value * countLow = CreateZExtOrTrunc(CreatePopcount(lowMaskByte), getInt8Ty());
    Constant * identity[16];
    for (unsigned i = 0; i < 16; i++) {
        identity[i] = getInt8(i);
    }
    Value * identityVec = ConstantVector::get(ArrayRef<Constant *>(identity, 16));
    Value * shiftIdx = simd_sub(8, identityVec, simd_fill(8, countLow));
    Value * shiftedHigh = tbl1(highCompressed, shiftIdx);

    Value * result = simd_or(lowCompressed, shiftedHigh);

    Value * totalCount = CreateZExtOrTrunc(CreatePopcount(maskBits), getInt8Ty());
    Value * validLane = CreateICmpULT(identityVec, simd_fill(8, totalCount));
    Value * zeroMask = CreateSExt(validLane, v16xi8Ty);

    return simd_and(result, zeroMask);
}

Value * IDISA_ARM_Builder::mvmd_compress(unsigned fw, Value * a, Value * select_mask) {
    if (mBitBlockWidth == 128 && (fw == 8 || fw == 16 || fw == 32 || fw == 64)) {
        Value * byteMask = (fw == 8) ? CreateZExtOrTrunc(select_mask, getInt16Ty())
                                      : expandFieldMaskToBytes(select_mask, fw);
        return compressBytes(a, byteMask);
    }
    return IDISA_Builder::mvmd_compress(fw, a, select_mask);
}

Value * IDISA_ARM_Builder::hsimd_packl(unsigned fw, Value * a, Value * b) {
    if ((fw >= 16) && (fw <= 64) && (getVectorBitWidth(a) == ARM_width)) {
        int nElems = getVectorBitWidth(a) / fw;
        int halfFw = fw / 2;
        Function* uzp1_fn = Intrinsic::getOrInsertDeclaration(getModule(),
                                                      Intrinsic::aarch64_sve_uzp1,
                                                      FixedVectorType::get(getIntNTy(halfFw), nElems * 2));
        return CreateCall(uzp1_fn->getFunctionType(), uzp1_fn, {fwCast(halfFw, a), fwCast(halfFw, b)});
    }
    // Otherwise use default logic.
    return IDISA_Builder::hsimd_packl(fw, a, b);
}

Value * IDISA_ARM_Builder::hsimd_packh(unsigned fw, Value * a, Value * b) {
    if ((fw >= 16) && (fw <= 64) && (getVectorBitWidth(a) == ARM_width)) {
        int nElems = getVectorBitWidth(a) / fw;
        int halfFw = fw / 2;
        Function* uzp2_fn = Intrinsic::getOrInsertDeclaration(getModule(),
                                                      Intrinsic::aarch64_sve_uzp2,
                                                      FixedVectorType::get(getIntNTy(halfFw), nElems * 2));
        return CreateCall(uzp2_fn->getFunctionType(), uzp2_fn, {fwCast(halfFw, a), fwCast(halfFw, b)});
    }
    // Otherwise use default logic.
    return IDISA_Builder::hsimd_packh(fw, a, b);
}

Value * IDISA_ARM_Builder::hsimd_packus(unsigned fw, Value * a, Value * b) {
  if ((fw == 16) && (getVectorBitWidth(a) == ARM_width)) {
    Function * vqmovun_s16_func = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_neon_uqxtn, FixedVectorType::get(getInt8Ty(), 8));
    Value * sat_a = CreateCall(vqmovun_s16_func->getFunctionType(), vqmovun_s16_func, fwCast(16, a));
    Value * sat_b = CreateCall(vqmovun_s16_func->getFunctionType(), vqmovun_s16_func, fwCast(16, b));
    return fwCast(8, CreateDoubleVector(sat_a, sat_b));
  }
  // Otherwise use default logic.
  return IDISA_Builder::hsimd_packus(fw, a, b);
}

Value * IDISA_ARM_Builder::esimd_mergeh(unsigned fw, Value * a, Value * b) {
    if ((fw >= 16) && (fw <= 64) && (getVectorBitWidth(a) == ARM_width)) {
        int nElms = getVectorBitWidth(a) / fw;
        Function * zip2_fn = Intrinsic::getOrInsertDeclaration(getModule(),
                                                     Intrinsic::aarch64_sve_zip2,
                                                     FixedVectorType::get(getIntNTy(fw), nElms));
        return CreateCall(zip2_fn->getFunctionType(), zip2_fn, {fwCast(fw, a), fwCast(fw, b)});
    }
    return IDISA_Builder::esimd_mergeh(fw, a, b);
}

Value * IDISA_ARM_Builder::esimd_mergel(unsigned fw, Value * a, Value * b) {
    if ((fw >= 16) && (fw <= 64) && (getVectorBitWidth(a) == ARM_width)) {
        int nElms = getVectorBitWidth(a) / fw;
        Function * zip1_fn = Intrinsic::getOrInsertDeclaration(getModule(),
                                                     Intrinsic::aarch64_sve_zip1,
                                                     FixedVectorType::get(getIntNTy(fw), nElms));
        return CreateCall(zip1_fn->getFunctionType(), zip1_fn, {fwCast(fw, a), fwCast(fw, b)});
    }
    return IDISA_Builder::esimd_mergel(fw, a, b);
}

// Native variable shift for sub-byte fields. Callers (pext/pdep/rotl/rotr) only feed
// in-range amounts (< fw), so a single byte-lane USHL/USHR plus a fixed field-isolation
// mask replaces the generic emulated inductive-doubling loop.
Value * IDISA_ARM_Builder::simd_sllv(unsigned fw, Value * v, Value * shifts) {
    if (getVectorBitWidth(v) == ARM_width && (fw == 2 || fw == 4)) {
        auto splat8 = [&](uint8_t x) { return getSplat(16, getInt8(x)); };
        if (fw == 4) {
            // remask each nibble after the byte shift so bits never carry across the nibble boundary
            Value * loData = simd_and(v, splat8(0x0F));
            Value * hiData = simd_and(v, splat8(0xF0));
            Value * loAmt = simd_and(shifts, splat8(0x0F));
            Value * hiAmt = simd_srli(8, shifts, 4);
            Value * loSh = simd_and(CreateShl(fwCast(8, loData), fwCast(8, loAmt)), splat8(0x0F));
            Value * hiSh = simd_and(CreateShl(fwCast(8, hiData), fwCast(8, hiAmt)), splat8(0xF0));
            return simd_or(loSh, hiSh);
        }
        // fw == 2: amount is one bit per field; expand it to a full 0b11 field mask and BSL-select
        Value * shifted = simd_and(CreateShl(fwCast(8, v), splat8(1)), splat8(0xAA));
        Value * a = simd_and(shifts, splat8(0x55));
        Value * sel = simd_or(a, CreateShl(fwCast(8, a), splat8(1)));
        return simd_or(simd_and(shifted, sel), simd_and(v, simd_not(sel)));
    }
    return IDISA_Builder::simd_sllv(fw, v, shifts);
}

Value * IDISA_ARM_Builder::simd_srlv(unsigned fw, Value * v, Value * shifts) {
    if (getVectorBitWidth(v) == ARM_width && (fw == 2 || fw == 4)) {
        auto splat8 = [&](uint8_t x) { return getSplat(16, getInt8(x)); };
        if (fw == 4) {
            Value * loData = simd_and(v, splat8(0x0F));
            Value * hiData = simd_and(v, splat8(0xF0));
            Value * loAmt = simd_and(shifts, splat8(0x0F));
            Value * hiAmt = simd_srli(8, shifts, 4);
            Value * loSh = simd_and(CreateLShr(fwCast(8, loData), fwCast(8, loAmt)), splat8(0x0F));
            Value * hiSh = simd_and(CreateLShr(fwCast(8, hiData), fwCast(8, hiAmt)), splat8(0xF0));
            return simd_or(loSh, hiSh);
        }
        Value * shifted = simd_and(CreateLShr(fwCast(8, v), splat8(1)), splat8(0x55));
        Value * a = simd_and(shifts, splat8(0x55));
        Value * sel = simd_or(a, CreateShl(fwCast(8, a), splat8(1)));
        return simd_or(simd_and(shifted, sel), simd_and(v, simd_not(sel)));
    }
    return IDISA_Builder::simd_srlv(fw, v, shifts);
}

}

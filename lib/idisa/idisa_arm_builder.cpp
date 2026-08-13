#include <idisa/idisa_arm_builder.h>

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAArch64.h>
#include <llvm/IR/Module.h>
#include <sstream>
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
    // AllowInternal, or the lookup never matches a private global and every
    // call site gets its own 4KB copy of the table.
    if (llvm::GlobalVariable * existing = mod->getGlobalVariable(name, true)) {
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

// Direct lookup is practical above fw=8: 4 KiB at fw=16 and smaller thereafter.
// Compress maps field f to rank(f); expand inverts it. Index 16 yields zero.
llvm::GlobalVariable * getOrCreateFieldPermuteTable(llvm::Module * mod, llvm::LLVMContext & C,
                                                    unsigned fw, bool isExpand) {
    const unsigned fieldCount = IDISA::ARM_width / fw;
    const unsigned bytesPerField = fw / 8;
    const unsigned entryCount = 1u << fieldCount;
    const std::string name = std::string("__idisa_arm_field_")
                           + (isExpand ? "expand" : "compress")
                           + "_table_" + std::to_string(fw);
    if (llvm::GlobalVariable * existing = mod->getGlobalVariable(name, true)) {
        return existing;
    }
    llvm::IntegerType * i8Ty = llvm::IntegerType::getInt8Ty(C);
    llvm::FixedVectorType * entryTy = llvm::FixedVectorType::get(i8Ty, 16);
    llvm::SmallVector<llvm::Constant *, 256> entries(entryCount);
    for (unsigned m = 0; m < entryCount; m++) {
        llvm::Constant * lanes[16];
        for (unsigned i = 0; i < 16; i++) {
            lanes[i] = llvm::ConstantInt::get(i8Ty, 16);
        }
        unsigned rank = 0;
        for (unsigned f = 0; f < fieldCount; f++) {
            if (m & (1u << f)) {
                const unsigned src = isExpand ? rank : f;
                const unsigned dst = isExpand ? f : rank;
                for (unsigned b = 0; b < bytesPerField; b++) {
                    lanes[dst * bytesPerField + b] =
                        llvm::ConstantInt::get(i8Ty, src * bytesPerField + b);
                }
                rank++;
            }
        }
        entries[m] = llvm::ConstantVector::get(llvm::ArrayRef<llvm::Constant *>(lanes, 16));
    }
    llvm::ArrayType * tableTy = llvm::ArrayType::get(entryTy, entryCount);
    llvm::Constant * tableInit = llvm::ConstantArray::get(tableTy, entries);
    return new llvm::GlobalVariable(*mod, tableTy, /*isConstant=*/true,
                                     llvm::GlobalValue::PrivateLinkage, tableInit, name);
}

} // anonymous namespace

namespace IDISA {

std::string IDISA_ARM_Builder::getBuilderUniqueName() { 
    std::stringstream uname;
    uname << "ARM";
    if (mBitBlockWidth != ARM_width) {
        uname << "_" << mBitBlockWidth;
    }
    if (IDISA::IDISA_Experiment != "") {
        uname << IDISA::IDISA_Experiment;
    }
    return uname.str();
}


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

Value * IDISA_ARM_Builder::mvmd_shuffle(unsigned fw, Value * data_table, Value * index_vector, ShuffleMode mode) {
    auto vec_width = getVectorBitWidth(data_table);
    if (vec_width == mNativeBitBlockWidth && fw == 8) {
        auto fieldCount = vec_width/fw;
        // Default for ARM is ShuffleMode::ZeroOnIndexOver
        if (mode == ShuffleMode::TruncateIndex) {
            Constant * fieldMask = ConstantInt::get(getIntNTy(fw), fieldCount - 1);
            index_vector = simd_and(index_vector, getSplat(fieldCount, fieldMask));
        } else if (mode == ShuffleMode::ZeroOnHighIndexBit) {
            // Preserve high bit for zeroing, but clear others.
            Constant * fieldMask = ConstantInt::get(getIntNTy(fw), (1<<(fw-1)) + fieldCount - 1);
            index_vector = simd_and(index_vector, getSplat(fieldCount, fieldMask));
        }
        Function * shuf8Func = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_neon_tbl1, FixedVectorType::get(getInt8Ty(), 16));
        return fwCast(8, CreateCall(shuf8Func->getFunctionType(), shuf8Func, {fwCast(8, data_table), fwCast(8, index_vector)}));
    }
    return IDISA_Builder::mvmd_shuffle(fw, data_table, index_vector, mode);
}

Value * IDISA_ARM_Builder::mvmd_shuffle2(unsigned fw, Value * table0, Value * table1, Value * index_vector, ShuffleMode mode) {
    auto vec_width = getVectorBitWidth(table0);
    if (vec_width == mNativeBitBlockWidth && fw == 8) {
        auto fieldCount = vec_width/fw;
        Function * shuf8Func = Intrinsic::getOrInsertDeclaration(getModule(), Intrinsic::aarch64_neon_tbl2, FixedVectorType::get(getInt8Ty(), 16));
        // Default for ARM is ShuffleMode::ZeroOnIndexOver
        if (mode == ShuffleMode::TruncateIndex) {
            Constant * fieldMask = ConstantInt::get(getIntNTy(fw), 2*fieldCount - 1);
            index_vector = simd_and(index_vector, getSplat(fieldCount, fieldMask));
        } else if (mode == ShuffleMode::ZeroOnHighIndexBit) {
            // Preserve high bit for zeroing, but clear others.
            Constant * fieldMask = ConstantInt::get(getIntNTy(fw), (1<<(fw-1)) + fieldCount - 1);
            index_vector = simd_and(index_vector, getSplat(fieldCount, fieldMask));
        }        
        Value * rslt = CreateCall(shuf8Func->getFunctionType(), shuf8Func, {fwCast(8, table0), fwCast(8, table1), fwCast(8, index_vector)});
            return rslt;
    }
    return IDISA_Builder::mvmd_shuffle2(fw, table0, table1, index_vector, mode);
}

// Expand a 16-bit mask to one boolean byte lane per bit.
Value * IDISA_ARM_Builder::byteMaskToLaneMask(Value * byteMask) {
    FixedVectorType * v16xi8Ty = FixedVectorType::get(getInt8Ty(), 16);
    Value * maskPair = CreateBitCast(CreateZExtOrTrunc(byteMask, getInt16Ty()),
                                     FixedVectorType::get(getInt8Ty(), 2));
    SmallVector<int, 16> halfIdx(16);
    Constant * sel[16];
    for (unsigned i = 0; i < 16; i++) {
        halfIdx[i] = i / 8;
        sel[i] = getInt8(1u << (i % 8));
    }
    Value * spread = CreateShuffleVector(maskPair, maskPair, halfIdx);
    Value * selVec = ConstantVector::get(ArrayRef<Constant *>(sel, 16));
    return CreateICmpNE(fwCast(8, simd_and(spread, selVec)),
                        ConstantAggregateZero::get(v16xi8Ty));
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

    Value * lowCompressed = mvmd_shuffle(8, a, lowIdx, ShuffleMode::ZeroOnIndexOver);
    Value * highCompressed = mvmd_shuffle(8, a, highIdx, ShuffleMode::ZeroOnIndexOver);

    Value * countLow = CreatePopcount(lowMaskByte);
    Value * shiftedHigh = mvmd_sll(8, highCompressed, countLow);

    Value * result = simd_or(lowCompressed, shiftedHigh);
    return result;
}

// Masking the index to fieldCount bits is required, not an optimization. The
// tables at fw 32 and 64 have only 16 and 4 entries, so an unmasked mask would
// index past the end.
Value * IDISA_ARM_Builder::fieldPermute(unsigned fw, Value * a, Value * select_mask, bool isExpand) {
    const unsigned fieldCount = ARM_width / fw;
    GlobalVariable * table = getOrCreateFieldPermuteTable(getModule(), getContext(), fw, isExpand);
    Type * i32Ty = getInt32Ty();
    Value * idx = CreateAnd(CreateZExtOrTrunc(select_mask, i32Ty),
                            ConstantInt::get(i32Ty, (1u << fieldCount) - 1));
    Value * gep = CreateInBoundsGEP(table->getValueType(), table,
                                     {ConstantInt::get(i32Ty, 0), idx});
    Value * perm = CreateLoad(FixedVectorType::get(getInt8Ty(), ARM_width/8), gep);
    return mvmd_shuffle(8, a, perm, ShuffleMode::ZeroOnIndexOver);
}

Value * IDISA_ARM_Builder::mvmd_compress(unsigned fw, Value * a, Value * select_mask) {
    if (getVectorBitWidth(a) == ARM_width) {
        if (fw == 16 || fw == 32 || fw == 64) {
            return fieldPermute(fw, a, select_mask, false);
        }
        if (fw == 8) {
            return compressBytes(a, CreateZExtOrTrunc(select_mask, getInt16Ty()));
        }
    }
    return IDISA_Builder::mvmd_compress(fw, a, select_mask);
}

Value * IDISA_ARM_Builder::mvmd_expand(unsigned fw, Value * a, Value * select_mask) {
    if (getVectorBitWidth(a) == ARM_width) {
        if (fw == 16 || fw == 32 || fw == 64) {
            return fieldPermute(fw, a, select_mask, true);
        }
    }
    return IDISA_Builder::mvmd_expand(fw, a, select_mask);
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

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <llvm/IR/IRBuilder.h>

namespace kernel { class Kernel; }

class LLVMTypeSystemInterface : private virtual llvm::IRBuilder<> {
    friend class CBuilder;
    friend class kernel::Kernel;
public:

    using llvm::IRBuilder<>::getInt1;
    using llvm::IRBuilder<>::getTrue;
    using llvm::IRBuilder<>::getFalse;
    using llvm::IRBuilder<>::getInt1Ty;

    using llvm::IRBuilder<>::getInt8;
    using llvm::IRBuilder<>::getInt8Ty;
    using llvm::IRBuilder<>::getInt16;
    using llvm::IRBuilder<>::getInt16Ty;
    using llvm::IRBuilder<>::getInt32;
    using llvm::IRBuilder<>::getInt32Ty;
    using llvm::IRBuilder<>::getInt64;
    using llvm::IRBuilder<>::getInt64Ty;
    using llvm::IRBuilder<>::getInt128Ty;

    using llvm::IRBuilder<>::getIntN;
    using llvm::IRBuilder<>::getIntNTy;

    using llvm::IRBuilder<>::getInt8PtrTy;

    using llvm::IRBuilder<>::getHalfTy;
    using llvm::IRBuilder<>::getBFloatTy;
    using llvm::IRBuilder<>::getDoubleTy;
    using llvm::IRBuilder<>::getFloatTy;

    using llvm::IRBuilder<>::getContext;

public:

    #define ADD_POINTER_TYPE_ALIAS(Name) \
        llvm::PointerType * get##Name##PtrTy(unsigned = 0) { \
            return llvm::PointerType::getUnqual(getContext()); \
        }

    // LLVM 18 removed all qualified pointer types but if we want to support earlier LLVM versions, we must still still allow kernels to
    // construct them.
    #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
    ADD_POINTER_TYPE_ALIAS(Int8)
    ADD_POINTER_TYPE_ALIAS(Int16)
    ADD_POINTER_TYPE_ALIAS(Int32)
    ADD_POINTER_TYPE_ALIAS(Int64)
    #endif

    ADD_POINTER_TYPE_ALIAS(Void)
    #undef ADD_POINTER_TYPE

    llvm::IntegerType * LLVM_READNONE getIntAddrTy() const {
        return llvm::IntegerType::get(getContext(), sizeof(intptr_t) * 8);
    }

    llvm::IntegerType * LLVM_READNONE getSizeTy() const {
        return llvm::IntegerType::get(getContext(), sizeof(size_t) * 8);
    }

    inline llvm::ConstantInt * LLVM_READNONE getSize(const size_t value) {
        return llvm::ConstantInt::get(getSizeTy(), value);
    }

    llvm::FixedVectorType * LLVM_READNONE getStreamTy(llvm::LLVMContext & C, const unsigned FieldWidth = 1) {
        return llvm::FixedVectorType::get(llvm::IntegerType::getIntNTy(C, FieldWidth), static_cast<unsigned>(0));
    }

    llvm::ArrayType * LLVM_READNONE getStreamSetTy(llvm::LLVMContext & C, const unsigned NumElements = 1, const unsigned FieldWidth = 1) {
        return llvm::ArrayType::get(getStreamTy(C, FieldWidth), NumElements);
    }

    llvm::FixedVectorType * getStreamTy(const unsigned FieldWidth = 1) {
        return getStreamTy(getContext(), FieldWidth);
    }

    llvm::ArrayType * getStreamSetTy(const unsigned NumElements = 1, const unsigned FieldWidth = 1) {
        return getStreamSetTy(getContext(), NumElements, FieldWidth);
    }

    virtual unsigned getBitBlockWidth() const = 0;

    virtual llvm::FixedVectorType * getBitBlockType() const = 0;

    virtual std::string getBuilderUniqueName() = 0;

    LLVMTypeSystemInterface(llvm::LLVMContext & C) : llvm::IRBuilder<>(C) { }

};

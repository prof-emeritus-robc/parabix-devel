/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>

namespace kernel { class Kernel; }

class LLVMTypeSystemInterface {
    friend class CBuilder;
    friend class kernel::Kernel;
public:

    /// Get a constant value representing either true or false.
    llvm::ConstantInt * LLVM_READNONE getInt1(bool V) {
      return llvm::ConstantInt::get(getInt1Ty(), V);
    }

    /// Get the constant value for i1 true.
    llvm::ConstantInt * LLVM_READNONE getTrue() {
      return llvm::ConstantInt::getTrue(getContext());
    }

    /// Get the constant value for i1 false.
    llvm::ConstantInt * LLVM_READNONE getFalse() {
      return llvm::ConstantInt::getFalse(getContext());
    }

    /// Get a constant 8-bit value.
    llvm::ConstantInt * LLVM_READNONE getInt8(uint8_t C) {
      return llvm::ConstantInt::get(getInt8Ty(), C);
    }

    /// Get a constant 16-bit value.
    llvm::ConstantInt * LLVM_READNONE getInt16(uint16_t C) {
      return llvm::ConstantInt::get(getInt16Ty(), C);
    }

    /// Get a constant 32-bit value.
    llvm::ConstantInt * LLVM_READNONE getInt32(uint32_t C) {
      return llvm::ConstantInt::get(getInt32Ty(), C);
    }

    /// Get a constant 64-bit value.
    llvm::ConstantInt * LLVM_READNONE getInt64(uint64_t C) {
      return llvm::ConstantInt::get(getInt64Ty(), C);
    }

    /// Get a constant 64-bit value.
    llvm::ConstantInt * LLVM_READNONE getSize(size_t C) {
      return llvm::ConstantInt::get(getSizeTy(), C);
    }

    /// Get a constant N-bit value, zero extended or truncated from
    /// a 64-bit value.
    llvm::ConstantInt * LLVM_READNONE getIntN(unsigned N, uint64_t C) {
      return llvm::ConstantInt::get(getIntNTy(N), C);
    }

    /// Get a constant integer value.
    llvm::ConstantInt * LLVM_READNONE getInt(const llvm::APInt &AI) {
      return llvm::ConstantInt::get(getContext(), AI);
    }

    llvm::Constant * LLVM_READNONE getDouble(const double C) {
        return llvm::ConstantFP::get(getDoubleTy(), C);
    }

    llvm::Constant * LLVM_READNONE getFloat(const float C) {
        return llvm::ConstantFP::get(getFloatTy(), C);
    }


    /// Fetch the type representing a single bit
    llvm::IntegerType * LLVM_READNONE getInt1Ty() {
      return llvm::Type::getInt1Ty(getContext());
    }

    /// Fetch the type representing an 8-bit integer.
    llvm::IntegerType * LLVM_READNONE getInt8Ty() {
      return llvm::Type::getInt8Ty(getContext());
    }

    /// Fetch the type representing a pointer to an 8-bit integer value.
    llvm::PointerType * LLVM_READNONE getInt8PtrTy(unsigned AddrSpace = 0) {
        return llvm::PointerType::get(getContext(), AddrSpace);
    }

    /// Fetch the type representing a 16-bit integer.
    llvm::IntegerType * LLVM_READNONE getInt16Ty() {
      return llvm::Type::getInt16Ty(getContext());
    }

    /// Fetch the type representing a 32-bit integer.
    llvm::IntegerType * LLVM_READNONE getInt32Ty() {
      return llvm::Type::getInt32Ty(getContext());
    }

    /// Fetch the type representing a 64-bit integer.
    llvm::IntegerType * LLVM_READNONE getInt64Ty() {
      return llvm::Type::getInt64Ty(getContext());
    }

    /// Fetch the type representing a 64-bit integer.
    llvm::IntegerType * LLVM_READNONE getSizeTy() {
      return llvm::IntegerType::get(getContext(), sizeof(size_t) * 8);
    }

    /// Fetch the type representing a 128-bit integer.
    llvm::IntegerType * LLVM_READNONE getInt128Ty() {
        return llvm::Type::getInt128Ty(getContext());
    }

    /// Fetch the type representing an N-bit integer.
    llvm::IntegerType * LLVM_READNONE getIntNTy(unsigned N) {
      return llvm::Type::getIntNTy(getContext(), N);
    }

    /// Fetch the type representing a 16-bit floating point value.
    llvm::Type * LLVM_READNONE getHalfTy() {
      return llvm::Type::getHalfTy(getContext());
    }

    /// Fetch the type representing a 16-bit brain floating point value.
    llvm::Type * LLVM_READNONE getBFloatTy() {
      return llvm::Type::getBFloatTy(getContext());
    }

    /// Fetch the type representing a 32-bit floating point value.
    llvm::Type * LLVM_READNONE getFloatTy() {
      return llvm::Type::getFloatTy(getContext());
    }

    /// Fetch the type representing a 64-bit floating point value.
    llvm::Type * LLVM_READNONE getDoubleTy() {
      return llvm::Type::getDoubleTy(getContext());
    }

    /// Fetch the type representing an untyped pointer.
    llvm::PointerType * LLVM_READNONE getVoidPtrTy(const unsigned AddressSpace = 0) const {
        return llvm::PointerType::get(getContext(), AddressSpace);
    }

    /// Fetch the type of an integer with size at least as big as that of a
    /// pointer in the given address space.
    llvm::IntegerType * LLVM_READNONE getIntPtrTy(unsigned AddressSpace = 0) {
        return llvm::IntegerType::get(getContext(), sizeof(uintptr_t) * 8);
    }

    virtual llvm::VectorType * getStreamTy(const unsigned FieldWidth = 1) = 0;

    virtual llvm::ArrayType * getStreamSetTy(const unsigned NumElements = 1, const unsigned FieldWidth = 1) = 0;

    virtual unsigned getBitBlockWidth() const = 0;

    virtual llvm::VectorType * getBitBlockType() const = 0;

    virtual llvm::LLVMContext & getContext() const = 0;

protected:

    virtual bool hasExternalFunction(const llvm::StringRef functionName) const = 0;

    virtual llvm::Function * addLinkFunction(llvm::Module * mod, llvm::StringRef name, llvm::FunctionType * type, void * functionPtr) const = 0;
};

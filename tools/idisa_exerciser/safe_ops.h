#pragma once

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "idisa_exerciser.h"

namespace llvm {
class Type;
class Value;
} // namespace llvm

llvm::Value *SafeURem(kernel::KernelBuilder &b, unsigned fw, llvm::Value *vec, unsigned divBy);
llvm::Value *SafeInsertElement(kernel::KernelBuilder &b, unsigned fw, llvm::Value *vec, llvm::Value *elt, uint64_t idx);
llvm::Value *SafeInsertElement(kernel::KernelBuilder &b, unsigned fw, llvm::Value *vec, llvm::Value *elt,
                               llvm::Value *idx);
llvm::Value *SafeExtractElement(kernel::KernelBuilder &b, unsigned fw, llvm::Value *vec, uint64_t idx);
llvm::Value *SafeExtractElement(kernel::KernelBuilder &b, unsigned fw, llvm::Value *vec, llvm::Value *idx);

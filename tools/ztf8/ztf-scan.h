/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/pablo.h>
#include <kernel/core/kernel_builder.h>
#include "ztf-logic.h"

namespace kernel {

class LengthGroupCompression final : public MultiBlockKernel {
public:
    LengthGroupCompression(LLVMTypeSystemInterface & ts,
                           EncodingInfo encodingScheme,
                           unsigned groupNo,
                           StreamSet * symbolMarks,
                           StreamSet * hashValues,
                           StreamSet * const byteData,
                           StreamSet * compressionMask,
                           StreamSet * encodedBytes,
                           unsigned strideBlocks = 8);
private:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;

    const EncodingInfo mEncodingScheme;
    const unsigned mGroupNo;
};

class LengthGroupDecompression final : public MultiBlockKernel {
public:
    LengthGroupDecompression(LLVMTypeSystemInterface & ts,
                             EncodingInfo encodingScheme,
                             unsigned groupNo,
                             StreamSet * keyMarks,
                             StreamSet * hashValues,
                             StreamSet * const hashMarks,
                             StreamSet * const byteData,
                             StreamSet * const result,
                             unsigned strideBlocks = 8);
private:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;

    const EncodingInfo mEncodingScheme;
    const unsigned mGroupNo;
};

class FixedLengthCompression final : public MultiBlockKernel {
public:
    FixedLengthCompression(LLVMTypeSystemInterface & ts,
                           EncodingInfo encodingScheme,
                           unsigned length,
                           StreamSet * const byteData,
                           StreamSet * hashValues,
                           std::vector<StreamSet *> symbolMarks,
                           StreamSet * compressionMask,
                           StreamSet * encodedBytes,
                           unsigned strideBlocks = 8);
private:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;

    const EncodingInfo mEncodingScheme;
    const unsigned mLo;
    const unsigned mHi;
    size_t mSubTableSize;
};

class FixedLengthDecompression final : public MultiBlockKernel {
public:
    FixedLengthDecompression(LLVMTypeSystemInterface & ts,
                             EncodingInfo encodingScheme,
                             unsigned lo,
                             StreamSet * const byteData,
                             StreamSet * const hashValues,
                             std::vector<StreamSet *> keyMarks,
                             std::vector<StreamSet *> hashMarks,
                             StreamSet * const result, unsigned strideBlocks = 8);
private:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;

    const EncodingInfo mEncodingScheme;
    const unsigned mLo;
    const unsigned mHi;
    size_t mSubTableSize;
};
}

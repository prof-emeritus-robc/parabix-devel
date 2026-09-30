/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/core/kernel.h>
#include <llvm/IR/Value.h>
#include <kernel/streamutils/stream_select.h>

namespace IDISA { class IDISA_Builder; }

namespace kernel {

/*  FilterByMask - extract selected bits of input streams according to a mask.

    One output bit is produced for every 1 bit in the mask stream.

    Input stream:    abcdefghjklmnpqra
    Mask stream:     ...1.1...111..1..     . represents 0
    Output stream:   dfjklmq

    The output stream is produced at the Popcount rate of the mask stream.

    The number of streams to process is governed by the size of the output stream set.
    The input streams to process are selected sequentially from the stream set,
    starting from the position indicated by the streamOffset value.

*/
    
void FilterByMask(PipelineBuilder & P,
                  StreamSet * mask, StreamSet * inputs, StreamSet * outputs,
                  unsigned streamOffset = 0,
                  unsigned extractionFieldWidth = 64);

//
// Parallel Prefix Deletion Kernel
// see Parallel Prefix Compress in Henry S. Warren, Hacker's Delight, Chapter 7
//
// Given that we want to delete bits within fields of width fw, moving
// nondeleted bits to the right, the parallel prefix compress method can
// be applied.   This requires a preprocessing step to compute log2(fw)
// masks that can be used to select bits to be moved in each step of the
// algorithm.
//
class DeletionKernel final : public BlockOrientedKernel {
public:
    DeletionKernel(LLVMTypeSystemInterface & ts, StreamSet * input, StreamSet * delMask, StreamSet * output, StreamSet * unitCounts);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
    void generateFinalBlockMethod(KernelBuilder & b, llvm::Value * remainingBytes) override;
private:
    const unsigned mDeletionFieldWidth;
    const unsigned mStreamCount;
};

// Compress within fields of size fieldWidth.
class FieldCompressKernel final : public MultiBlockKernel {
public:
    FieldCompressKernel(LLVMTypeSystemInterface & ts,
                        SelectOperation const & maskOp, SelectOperationList const & inputOps, StreamSet * outputStreamSet,
                        unsigned fieldWidth = 64);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
private:
    const unsigned mFW;
    SelectedInput mMaskOp;
    SelectedInputList mInputOps;

};

class PEXTFieldCompressKernel final : public MultiBlockKernel {
public:
    PEXTFieldCompressKernel(LLVMTypeSystemInterface & ts, unsigned fw, unsigned streamCount);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
private:
    const unsigned mPEXTWidth;
    const unsigned mStreamCount;
};

//
//  Given streams that are compressed within fields, produced fully
//  compressed streams.
class StreamCompressKernel final : public MultiBlockKernel {
public:
    StreamCompressKernel(LLVMTypeSystemInterface & ts
                         , StreamSet * extractionMask
                         , StreamSet * source
                         , StreamSet * compressedOutput
                         , const unsigned FieldWidth = sizeof(size_t) * 8);
protected:
    void generateMultiBlockLogic(KernelBuilder & kb, llvm::Value * const numOfBlocks) override;
private:
    const unsigned mFW;
    const unsigned mStreamCount;
};

/*
Input: a set of bitstreams
Output: swizzles containing the input bitstreams with the specified bits deleted
*/
class SwizzledDeleteByPEXTkernel final : public MultiBlockKernel {
public:
    using SwizzleSets = std::vector<std::vector<llvm::Value *>>;
    SwizzledDeleteByPEXTkernel(LLVMTypeSystemInterface & ts
                               , StreamSet * selectors, StreamSet * inputStreamSet
                               , const std::vector<StreamSet *> & outputs
                               , unsigned PEXTWidth = sizeof(size_t) * 8);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfBlocks) override;
private:
    SwizzleSets makeSwizzleSets(KernelBuilder & b, llvm::Value * selectors, llvm::Value * const strideIndex);
private:
    const unsigned mStreamCount;
    const unsigned mSwizzleFactor;
    const unsigned mSwizzleSetCount;
    const unsigned mPEXTWidth;
};

class DeleteByPEXTkernel final : public BlockOrientedKernel {
public:
    DeleteByPEXTkernel(LLVMTypeSystemInterface & ts, unsigned fw, unsigned streamCount, unsigned PEXT_width = sizeof(size_t) * 8);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
    void generateFinalBlockMethod(KernelBuilder & b, llvm::Value * remainingBytes) override;
    void generateProcessingLoop(KernelBuilder & b, llvm::Value * delMask);
private:
    const unsigned mDelCountFieldWidth;
    const unsigned mStreamCount;
    const unsigned mSwizzleFactor;
    const unsigned mPEXTWidth;
};

class SwizzledBitstreamCompressByCount final : public BlockOrientedKernel {
public:
    SwizzledBitstreamCompressByCount(LLVMTypeSystemInterface & ts, unsigned bitStreamCount, unsigned fieldWidth = sizeof(size_t) * 8);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
    void generateFinalBlockMethod(KernelBuilder & b, llvm::Value * remainingBytes) override;
private:
    const unsigned mBitStreamCount;
    const unsigned mFieldWidth;
    const unsigned mSwizzleFactor;
    const unsigned mSwizzleSetCount;
};

// Compress within fields of size fieldWidth.
class FilterByMaskKernel final : public MultiBlockKernel {
public:
    FilterByMaskKernel(LLVMTypeSystemInterface & ts,
                        SelectOperation const & maskOp, SelectOperationList const & inputOps, StreamSet * outputStreamSet,
                        unsigned fieldWidth = 64,
                        ProcessingRateProbabilityDistribution insertionProbabilityDistribution = MaximumDistribution());
protected:
    void generateMultiBlockLogic(KernelBuilder & kb, llvm::Value * const numOfStrides) override;
private:
    const unsigned mFW;
    SelectedInput mMaskOp;
    SelectedInputList mInputOps;
    unsigned mFieldsPerBlock;
    unsigned mStreamCount;
    llvm::Type * mPendingType;
    unsigned mPendingSetCount;
};

class ByteFilterByMaskKernel final : public MultiBlockKernel {
public:
    ByteFilterByMaskKernel(LLVMTypeSystemInterface & b, StreamSet * const byteStream, StreamSet * const filter, StreamSet * const output, Scalar * streamOffset = nullptr);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

}


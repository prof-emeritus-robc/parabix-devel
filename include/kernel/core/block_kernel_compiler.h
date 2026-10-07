#pragma once

#include <kernel/core/kernel_compiler.h>

namespace kernel {

class BlockKernelCompiler : public KernelCompiler {
public:

    BlockKernelCompiler(BlockOrientedKernel * const kernel) noexcept;

    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfBlocks);

    void generateDefaultFinalBlockMethod(KernelBuilder & b);

protected:

    llvm::Value * getRemainingItems(KernelBuilder & b);

    void incrementCountableItemCounts(KernelBuilder & b);

    llvm::Value * getPopCountRateItemCount(KernelBuilder & b, const ProcessingRate & rate);

    void writeDoBlockMethod(KernelBuilder & b);

    void writeFinalBlockMethod(KernelBuilder & b, llvm::Value * remainingItems);

    void validateProvisionalLookAheadStride(KernelBuilder & b);

    void writeProvisionalStride(KernelBuilder & b, llvm::Value * const numOfBlocks, llvm::BasicBlock * const segmentDone);

    llvm::Value * saveKernelState(KernelBuilder & b, llvm::StructType * const stateTy, llvm::Value * const handle);

    void restoreKernelState(KernelBuilder & b, llvm::StructType * const stateTy, llvm::Value * const handle, llvm::Value * const saved);

private:

    llvm::Function *            mDoBlockMethod;
    llvm::BasicBlock *          mStrideLoopBody;
    llvm::IndirectBrInst *      mStrideLoopBranch;
    llvm::PHINode *             mStrideLoopTarget;
    llvm::PHINode *             mStrideBlockIndex;
};

}


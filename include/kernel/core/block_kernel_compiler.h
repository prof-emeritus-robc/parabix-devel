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

private:

    llvm::Function *            mDoBlockMethod;
    llvm::BasicBlock *          mStrideLoopBody;
    llvm::IndirectBrInst *      mStrideLoopBranch;
    llvm::PHINode *             mStrideLoopTarget;
    llvm::PHINode *             mStrideBlockIndex;
};

}


/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/core/block_kernel_compiler.h>
#include <pablo/carry_manager.h>
#include <unordered_map>
#include <vector>
#include <memory>
namespace IDISA { class IDISA_Builder; }
namespace llvm { class BasicBlock; }
namespace llvm { class Function; }
namespace llvm { class Value; }
namespace pablo { class If; }
namespace pablo { class PabloAST; }
namespace pablo { class PabloBlock; }
namespace pablo { class PabloKernel; }
namespace pablo { class Statement; }
namespace pablo { class Var; }
namespace pablo { class While; }

namespace pablo {

class PabloCompiler final : public kernel::BlockKernelCompiler {
public:

    friend class PabloKernel;

    using TranslationMap = std::unordered_map<const PabloAST *, llvm::Value *>;

    using KernelBuilder = kernel::KernelBuilder;

    PabloCompiler(PabloKernel * kernel);

protected:

    void initializeKernelData(KernelBuilder & b);

    void initializeIllustrator(KernelBuilder & b);

    void compile(KernelBuilder & b);

    void releaseKernelData(KernelBuilder & b);

    void clearCarryData(KernelBuilder & b);

private:

    bool identifyIllustratedValues(KernelBuilder & b, const PabloBlock * const block, llvm::SmallVector<size_t, 8> & loopIds, size_t & currentLoopId);

    void examineBlock(KernelBuilder & b, const PabloBlock * const block);

    void compileBlock(KernelBuilder & b, const PabloBlock * const block);

    void compileStatement(KernelBuilder & b, const Statement * stmt);

    void compileIf(KernelBuilder & b, const If * ifStmt);

    void compileWhile(KernelBuilder & b, const While * whileStmt);

    void addBranchCounter(KernelBuilder & b);

    const Var * findInputParam(const Statement * const stmt, const Var * const param) const;

    llvm::Value * getPointerToVar(KernelBuilder & b, const Var * var, llvm::Value * index1, llvm::Value * index2 = nullptr);

    llvm::Value * compileExpression(KernelBuilder & b, const PabloAST * expr, const bool ensureLoaded = true);

    static void dumpValueToConsole(KernelBuilder & b, const PabloAST * expr, llvm::Value * value);

private:

    PabloKernel * const                 mKernel;
    llvm::Value *                       mIllustratorStrideNum;
    std::unique_ptr<CarryManager> const mCarryManager;
    TranslationMap                      mMarker;
    unsigned                            mBranchCount;
    llvm::BasicBlock *                  mEntryBlock;
    std::vector<llvm::BasicBlock *>     mBasicBlock;
    llvm::SmallVector<const While *, 0> mContainsIllustratedValue;
};

}


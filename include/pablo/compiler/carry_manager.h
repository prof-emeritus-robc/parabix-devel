/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/compiler/carry_data.h>
#include <llvm/ADT/SmallVector.h>
#include <memory>
#include <vector>

namespace IDISA { class IDISA_Builder; }
namespace llvm { class BasicBlock; }
namespace llvm { class ConstantInt; }
namespace llvm { class Function; }
namespace llvm { class PHINode; }
namespace llvm { class StructType; }
namespace llvm { class Type; }
namespace llvm { class Value; }
namespace pablo { class Advance; }
namespace pablo { class IndexedAdvance; }
namespace pablo { class PabloBlock; }
namespace pablo { class PabloKernel; }
namespace pablo { class Statement; }
namespace kernel { class KernelBuilder; }

/*
 * Carry Data Manager.
 *
 * Each PabloBlock (Main, If, While) has a contiguous data area for carry information.
 * The data area may be at a fixed or variable base offset from the base of the
 * main function carry data area.
 * The data area for each block consists of contiguous space for the local carries and
 * advances of the block plus the areas of any ifs/whiles nested within the block.

*/

namespace pablo {

class CarryManager {

    enum { LONG_ADVANCE_BASE = 64 };

    template <typename T>
    using Vec = llvm::SmallVector<T, 64>;

public:

    CarryManager() noexcept;

    virtual ~CarryManager() = default;

    virtual void initializeCarryData(kernel::KernelBuilder & b, PabloKernel * const kernel);

    virtual void releaseCarryData(kernel::KernelBuilder & idb);

    virtual void initializeCodeGen(kernel::KernelBuilder & b);

    virtual void finalizeCodeGen(kernel::KernelBuilder & b);

    /* Entering and leaving loops. */

    virtual void enterLoopScope(kernel::KernelBuilder & b);

    virtual void enterLoopBody(kernel::KernelBuilder & b, llvm::BasicBlock * const entryBlock);

    virtual void leaveLoopBody(kernel::KernelBuilder & b);

    virtual void leaveLoopScope(kernel::KernelBuilder & b, llvm::BasicBlock * const entryBlock, llvm::BasicBlock * const exitBlock);

    /* Entering and leaving ifs. */

    virtual void enterIfScope(kernel::KernelBuilder & b);

    virtual void enterIfBody(kernel::KernelBuilder & b, llvm::BasicBlock * const entryBlock);

    virtual void leaveIfBody(kernel::KernelBuilder & b, llvm::BasicBlock * const exitBlock);

    virtual void leaveIfScope(kernel::KernelBuilder & b, llvm::BasicBlock * const entryBlock, llvm::BasicBlock * const exitBlock);

    /* Methods for processing individual carry-generating operations. */

    virtual llvm::Value * addCarryInCarryOut(kernel::KernelBuilder & b, const Statement * operation, llvm::Value * const e1, llvm::Value * const e2);

    virtual llvm::Value * subBorrowInBorrowOut(kernel::KernelBuilder & b, const Statement * operation, llvm::Value * const e1, llvm::Value * const e2);

    virtual llvm::Value * advanceCarryInCarryOut(kernel::KernelBuilder & b, const Advance * advance, llvm::Value * const strm);

    virtual llvm::Value * indexedAdvanceCarryInCarryOut(kernel::KernelBuilder & b, const IndexedAdvance * advance, llvm::Value * const strm, llvm::Value * const index_strm);

    /* Methods for getting and setting carry summary values for If statements */

    virtual llvm::Value * generateEntrySummaryTest(kernel::KernelBuilder & b, llvm::Value * condition);

    virtual llvm::Type * getSummaryTypeFromCurrentFrame(kernel::KernelBuilder & b) const;

    virtual llvm::Value * generateExitSummaryTest(kernel::KernelBuilder & b, llvm::Value * condition);

    /* Clear carry state for conditional regions */

    virtual void clearCarryData(kernel::KernelBuilder & idb);

protected:

    static unsigned getScopeCount(const PabloBlock * const scope, unsigned index = 0);

    virtual llvm::StructType * analyse(kernel::KernelBuilder & b, const PabloBlock * const scope);


    llvm::StructType * analyse(kernel::KernelBuilder & b, const PabloBlock * const scope, const unsigned ifDepth, const unsigned whileDepth, const bool isNestedWithinNonCarryCollapsingLoop);

    /* Entering and leaving scopes. */
    void enterScope(kernel::KernelBuilder & b);
    void leaveScope();

    /* Methods for processing individual carry-generating operations. */
    virtual llvm::Value * getNextCarryIn(kernel::KernelBuilder & b);
    virtual void setNextCarryOut(kernel::KernelBuilder & b, llvm::Value * const carryOut);
    virtual llvm::Value * shortIndexedAdvanceCarryInCarryOut(kernel::KernelBuilder & b, const unsigned shiftAmount, llvm::Value * const strm, llvm::Value * const index_strm);
    virtual llvm::Value * longAdvanceCarryInCarryOut(kernel::KernelBuilder & b, llvm::Value * const value, const unsigned shiftAmount);
    virtual llvm::Value * readCarryInSummary(kernel::KernelBuilder & b) const;
    virtual void writeCarryOutSummary(kernel::KernelBuilder & b, llvm::Value * const summary) const;

    /* Summary handling routines */
    virtual void addToCarryOutSummary(kernel::KernelBuilder & b, llvm::Value * const value);

    virtual void phiCurrentCarryOutSummary(kernel::KernelBuilder & b, llvm::BasicBlock * const entryBlock, llvm::BasicBlock * const exitBlock);
    virtual void phiOuterCarryOutSummary(kernel::KernelBuilder & b, llvm::BasicBlock * const entryBlock, llvm::BasicBlock * const exitBlock);
    virtual void writeCurrentCarryOutSummary(kernel::KernelBuilder & b);
    virtual void combineCarryOutSummary(kernel::KernelBuilder & b, const unsigned offset);

    /* Misc. routines */
    static bool hasNonEmptyCarryStruct(const llvm::Type * const frameTy);
    static bool isEmptyCarryStruct(const std::vector<llvm::Type *> & frameTys);

    static void callIndexedAdvCarryInCarryOut(kernel::KernelBuilder & b, const size_t shiftAmount,
                                              llvm::Value * const strm, llvm::Value * const index_strm, llvm::Value * carryIn,
                                              llvm::Value *& result, llvm::Value *& carryOut);

protected:

    const PabloKernel *                             mKernel;

    llvm::Value *                                   mCurrentFrame;
    llvm::StructType *                              mCurrentFrameType;
    unsigned                                        mCurrentFrameIndex;

    const CarryData *                               mCarryInfo;

    llvm::StructType *                              mCarryFrameType;

    llvm::Value *                                   mNextSummaryTest;

    unsigned                                        mIfDepth;

    bool                                            mHasLongAdvance;
    unsigned                                        mIndexedLongAdvanceTotal;
    unsigned                                        mIndexedLongAdvanceIndex;
    bool                                            mHasNonCarryCollapsingLoops;
    bool                                            mHasLoop;
    unsigned                                        mLoopDepth;
    llvm::PHINode *                                 mNestedLoopCarryInMaskPhi;
    llvm::Value *                                   mLoopSelector;
    llvm::Value *                                   mNextLoopSelector;
    llvm::Value *                                   mCarryPackPtr;

    struct NonCarryCollapsingFrame {
        llvm::StructType * OuterFrameType;
        llvm::Value *      OuterFrame;
        size_t             NestedFrameIndex;
        llvm::PHINode *    LoopIterationPhi = nullptr;
        llvm::Value *      LastNonZeroIteration = nullptr;
        llvm::Value *      LastIncomingCarryIteration;

        NonCarryCollapsingFrame(llvm::StructType * ty, llvm::Value * frame, size_t index, llvm::Value * lastIncomingCarryIteration)
        : OuterFrameType(ty), OuterFrame(frame), NestedFrameIndex(index), LastIncomingCarryIteration(lastIncomingCarryIteration) {

        }
    };

    Vec<NonCarryCollapsingFrame>                    mNonCarryCollapsingModeStack;

    Vec<CarryData>                                  mCarryMetadata;

    struct CarryFrame {
        llvm::Value *       Frame = nullptr;
        llvm::StructType *  Type = nullptr;
        unsigned            Index = 0;

        CarryFrame() = default;
        CarryFrame(llvm::Value * frame, llvm::StructType * type, unsigned index) : Frame(frame), Type(type), Index(index) {}
    };

    Vec<CarryFrame>                                 mCarryFrameStack;

    unsigned                                        mCarryScopes;
    Vec<unsigned>                                   mCarryScopeIndex;

    Vec<llvm::Value *>                              mCarrySummaryStack;
};

}


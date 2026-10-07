#include <kernel/core/block_kernel_compiler.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/MDBuilder.h>

using namespace llvm;

namespace kernel {

const auto DO_BLOCK_SUFFIX = "_DoBlock";
const auto FINAL_BLOCK_SUFFIX = "_FinalBlock";

#define TARGET (reinterpret_cast<BlockOrientedKernel *>(mTarget))

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateMultiBlockLogic
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockKernelCompiler::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfBlocks) {

    const auto stride = mTarget->getStride();

    if (LLVM_UNLIKELY(stride != b.getBitBlockWidth())) {
        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << getName() << ": the Stride (" << stride << ") of BlockOrientedKernel "
               "equal to the BitBlockWidth (" << b.getBitBlockWidth() << ")";
        report_fatal_error(out.str());
    }

    const auto hasFinalBlock = true; // mTarget->requiresExplicitPartialFinalStride();

    const auto hasProvisionalStride = mTarget->hasAttribute(Attribute::KindId::ProvisionalLookAheadStride);
    if (LLVM_UNLIKELY(hasProvisionalStride)) {
        validateProvisionalLookAheadStride(b);
    }

    BasicBlock * const entryBlock = b.GetInsertBlock();
    mStrideLoopBody = b.CreateBasicBlock(getName() + "_strideLoopBody");


    BasicBlock * const incrementCountableItems = b.CreateBasicBlock(getName() + "_incrementCountableItems");
    BasicBlock * const stridesDone = b.CreateBasicBlock(getName() + "_stridesDone");
    BasicBlock * doFinalBlock = nullptr;
    BasicBlock * segmentDone = nullptr;
    // A kernel with a provisional stride continues to checkProvisional, rather than
    // segmentDone, after its full strides in a non-final segment.
    BasicBlock * checkProvisional = nullptr;
    BasicBlock * stridesExit = nullptr;

    if (hasFinalBlock) {
        doFinalBlock = b.CreateBasicBlock(getName() + "_doFinalBlock");
        segmentDone = b.CreateBasicBlock(getName() + "_segmentDone");
        stridesExit = segmentDone;
        if (LLVM_UNLIKELY(hasProvisionalStride)) {
            checkProvisional = b.CreateBasicBlock(getName() + "_checkProvisional");
            stridesExit = checkProvisional;
        }
        b.CreateUnlikelyCondBr(mIsFinal, doFinalBlock, mStrideLoopBody);
    } else {
        b.CreateBr(mStrideLoopBody);
    }

    /// BLOCK BODY

    b.SetInsertPoint(mStrideLoopBody);
    mStrideLoopTarget = nullptr;
    if (hasFinalBlock && b.supportsIndirectBr()) {
        Value * const baseTarget = BlockAddress::get(stridesExit);
        mStrideLoopTarget = b.CreatePHI(baseTarget->getType(), 2, "strideTarget");
        mStrideLoopTarget->addIncoming(baseTarget, entryBlock);
    }
    mStrideBlockIndex = b.CreatePHI(b.getSizeTy(), 2);
    mStrideBlockIndex->addIncoming(b.getSize(0), entryBlock);

    /// GENERATE DO BLOCK METHOD

    writeDoBlockMethod(b);

    Value * const nextStrideBlockIndex = b.CreateAdd(mStrideBlockIndex, b.getSize(1));
    Value * noMore = b.CreateICmpEQ(nextStrideBlockIndex, numOfBlocks);
    if (canSetTerminateSignal()) {
        noMore = b.CreateOr(noMore, b.getTerminationSignal());
    }
    b.CreateUnlikelyCondBr(noMore, stridesDone, incrementCountableItems);

    b.SetInsertPoint(incrementCountableItems);
    incrementCountableItemCounts(b);
    BasicBlock * const bodyEnd = b.GetInsertBlock();
    if (mStrideLoopTarget) {
        mStrideLoopTarget->addIncoming(mStrideLoopTarget, bodyEnd);
    }
    mStrideBlockIndex->addIncoming(nextStrideBlockIndex, bodyEnd);

    b.CreateBr(mStrideLoopBody);

    stridesDone->moveAfter(bodyEnd);

    /// STRIDE DONE

    b.SetInsertPoint(stridesDone);

    if (hasFinalBlock) {

        // Now conditionally perform the final block processing depending on the doFinal parameter.
        if (mStrideLoopTarget) {
            mStrideLoopBranch = b.CreateIndirectBr(mStrideLoopTarget, 3);
            mStrideLoopBranch->addDestination(doFinalBlock);
            mStrideLoopBranch->addDestination(stridesExit);
        } else {
            b.CreateUnlikelyCondBr(mIsFinal, doFinalBlock, stridesExit);
        }

        doFinalBlock->moveAfter(stridesDone);

        /// DO FINAL BLOCK

        b.SetInsertPoint(doFinalBlock);
        writeFinalBlockMethod(b, getRemainingItems(b));
        b.CreateBr(segmentDone);

        if (LLVM_UNLIKELY(hasProvisionalStride)) {
            checkProvisional->moveAfter(b.GetInsertBlock());
            b.SetInsertPoint(checkProvisional);
            writeProvisionalStride(b, numOfBlocks, segmentDone);
        }

        segmentDone->moveAfter(b.GetInsertBlock());

        b.SetInsertPoint(segmentDone);

        // Update the branch prediction metadata to indicate that the likely target will be segmentDone
        if (mStrideLoopTarget) {
            MDBuilder mdb(b.getContext());
            const auto destinations = mStrideLoopBranch->getNumDestinations();
            SmallVector<uint32_t, 16> weights(destinations);
            for (unsigned i = 0; i < destinations; ++i) {
                weights[i] = (mStrideLoopBranch->getDestination(i) == stridesExit) ? 100 : 1;
            }
            mStrideLoopBranch->setMetadata(LLVMContext::MD_prof, mdb.createBranchWeights(weights));
        }

    }


}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief validateProvisionalLookAheadStride
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockKernelCompiler::validateProvisionalLookAheadStride(KernelBuilder & /* b */) {
    std::string reason;
    if (LLVM_UNLIKELY(!TARGET->meetsProvisionalLookAheadStrideRequirements(&reason))) {
        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << getName() << ": ProvisionalLookAheadStride requires " << reason;
        report_fatal_error(out.str());
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeProvisionalStride
 *
 * Called in a non-final segment after the full strides.  If the accessible input extends past the
 * full strides, process one further stride as if it were a full stride, then restore the kernel state
 * to its value after the full strides so that the provisional stride is reprocessed next time.
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockKernelCompiler::writeProvisionalStride(KernelBuilder & b, Value * const numOfBlocks, BasicBlock * const segmentDone) {

    BasicBlock * const doProvisional = b.CreateBasicBlock(getName() + "_doProvisionalStride", segmentDone);
    BasicBlock * const provisionalDone = b.CreateBasicBlock(getName() + "_provisionalStrideDone", segmentDone);

    // All stream inputs are FixedRate(1), so the full strides cover numOfBlocks * stride items.
    Value * const fullItems = b.CreateMul(numOfBlocks, b.getSize(mTarget->getStride()));
    Value * const hasProvisional = b.CreateICmpUGT(getRemainingItems(b), fullItems);
    b.CreateLikelyCondBr(hasProvisional, doProvisional, segmentDone);

    b.SetInsertPoint(doProvisional);
    // the stride loop does not advance the item counts after its last stride
    incrementCountableItemCounts(b);
    Value * const savedShared = saveKernelState(b, mTarget->getSharedStateType(), getHandle());
    Value * const savedThreadLocal = saveKernelState(b, mTarget->getThreadLocalStateType(), getThreadLocalHandle());

    if (b.supportsIndirectBr()) {
        // Reenter the stride loop body for exactly one stride and resume at provisionalDone.
        mStrideLoopBranch->addDestination(provisionalDone);
        BasicBlock * const current = b.GetInsertBlock();
        mStrideLoopTarget->addIncoming(BlockAddress::get(provisionalDone), current);
        mStrideBlockIndex->addIncoming(b.CreateSub(numOfBlocks, b.getSize(1)), current);
        b.CreateBr(mStrideLoopBody);
    } else {
        std::vector<Value *> args;
        args.reserve(1 + mAccessibleInputItems.size());
        args.push_back(b.getHandle());
        args.insert(args.end(), mAccessibleInputItems.begin(), mAccessibleInputItems.end());
        b.CreateCall(mDoBlockMethod->getFunctionType(), mDoBlockMethod, args);
        b.CreateBr(provisionalDone);
    }

    b.SetInsertPoint(provisionalDone);
    restoreKernelState(b, mTarget->getSharedStateType(), getHandle(), savedShared);
    restoreKernelState(b, mTarget->getThreadLocalStateType(), getThreadLocalHandle(), savedThreadLocal);
    b.CreateBr(segmentDone);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief saveKernelState
 ** ------------------------------------------------------------------------------------------------------------- */
Value * BlockKernelCompiler::saveKernelState(KernelBuilder & b, StructType * const stateTy, Value * const handle) {
    if (stateTy == nullptr || handle == nullptr || stateTy->isEmptyTy()) {
        return nullptr;
    }
    const DataLayout & DL = b.getModule()->getDataLayout();
    const auto size = DL.getTypeAllocSize(stateTy);
    const auto align = DL.getABITypeAlign(stateTy).value();
    Value * const saved = b.CreateAllocaAtEntryPoint(stateTy, nullptr, "savedKernelState");
    b.CreateMemCpy(saved, handle, size, align);
    return saved;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief restoreKernelState
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockKernelCompiler::restoreKernelState(KernelBuilder & b, StructType * const stateTy, Value * const handle, Value * const saved) {
    if (saved == nullptr) {
        return;
    }
    const DataLayout & DL = b.getModule()->getDataLayout();
    const auto size = DL.getTypeAllocSize(stateTy);
    const auto align = DL.getABITypeAlign(stateTy).value();
    b.CreateMemCpy(handle, saved, size, align);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief incrementCountableItemCounts
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockKernelCompiler::incrementCountableItemCounts(KernelBuilder & b) {
    // Update the processed item counts

    const auto stride = mTarget->getStride();

    for (const Binding & input : getInputStreamSetBindings()) {
        if (isCountable(input) && !input.isDeferred()) {
            const ProcessingRate & rate = input.getRate();
            Value * offset = nullptr;
            if (rate.isFixed()) {
                offset = b.getSize(ceiling(getUpperBound(input) * stride));
            } else { // if (rate.isPopCount() || rate.isNegatedPopCount())
                offset = getPopCountRateItemCount(b, rate);
            }
            Value * const initial = b.getProcessedItemCount(input.getName());
            Value * const processed = b.CreateAdd(initial, offset);
            b.setProcessedItemCount(input.getName(), processed);
        }
    }
    // Update the produced item counts
    for (const Binding & output : getOutputStreamSetBindings()) {
        if (isCountable(output) && !output.isDeferred()) {
            const ProcessingRate & rate = output.getRate();
            Value * offset = nullptr;
            if (rate.isFixed()) {
                offset = b.getSize(ceiling(getUpperBound(output) * stride));
            } else { // if (rate.isPopCount() || rate.isNegatedPopCount())
                offset = getPopCountRateItemCount(b, rate);
            }
            Value * const initial = b.getProducedItemCount(output.getName());
            Value * const produced = b.CreateAdd(initial, offset);
            b.setProducedItemCount(output.getName(), produced);
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getPopCountRateItemCount
 ** ------------------------------------------------------------------------------------------------------------- */
Value * BlockKernelCompiler::getPopCountRateItemCount(KernelBuilder & /* b */,
                                                      const ProcessingRate & /* rate */) {
    report_fatal_error("PopCount rates are not currently supported");
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getRemainingItems
 ** ------------------------------------------------------------------------------------------------------------- */
Value * BlockKernelCompiler::getRemainingItems(KernelBuilder & b) {
    const auto count = mInputStreamSets.size();
    assert (count > 0);
    for (unsigned i = 0; i < count; i++) {
        if (mInputStreamSets[i].isPrincipal()) {
            return mAccessibleInputItems[i];
        }
    }
    return mAccessibleInputItems[0];
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeDoBlockMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void BlockKernelCompiler::writeDoBlockMethod(KernelBuilder & b) {

    Value * const self = b.getHandle();
    Function * const cp = mCurrentMethod;
    auto ip = b.saveIP();
    Vec<Value *> availableItemCount;

    /// Check if the do block method is called and create the function if necessary
    if (!b.supportsIndirectBr()) {

        std::vector<Type *> params;
        params.reserve(1 + mAccessibleInputItems.size());
        params.push_back(self->getType());
        for (Value * avail : mAccessibleInputItems) {
            params.push_back(avail->getType());
        }

        FunctionType * const type = FunctionType::get(b.getVoidTy(), params, false);
        mCurrentMethod = Function::Create(type, GlobalValue::InternalLinkage, getName() + DO_BLOCK_SUFFIX, b.getModule());
        mCurrentMethod->setCallingConv(CallingConv::C);
        mCurrentMethod->setDoesNotThrow();
        auto args = mCurrentMethod->arg_begin();
        args->setName("self");
        setHandle(&*args);
        availableItemCount.reserve(mAccessibleInputItems.size());
        while (++args != mCurrentMethod->arg_end()) {
            availableItemCount.push_back(&*args);
        }
        assert (availableItemCount.size() == mAccessibleInputItems.size());
        mAccessibleInputItems.swap(availableItemCount);
        b.SetInsertPoint(BasicBlock::Create(b.getContext(), "entry", mCurrentMethod));
    }

    TARGET->generateDoBlockMethod(b); // must be implemented by the BlockOrientedKernelBuilder subtype

    if (!b.supportsIndirectBr()) {
        // Restore the DoSegment function state then call the DoBlock method
        b.CreateRetVoid();
        mDoBlockMethod = mCurrentMethod;
        b.restoreIP(ip);
        setHandle(self);
        mCurrentMethod = cp;
        mAccessibleInputItems.swap(availableItemCount);
        generateDefaultFinalBlockMethod(b);
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeFinalBlockMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void BlockKernelCompiler::writeFinalBlockMethod(KernelBuilder & b, Value * remainingItems) {

    Value * const self = b.getHandle();
    Function * const cp = mCurrentMethod;
    Value * const remainingItemCount = remainingItems;
    auto ip = b.saveIP();
    Vec<Value *> availableItemCount;

    if (!b.supportsIndirectBr()) {
        std::vector<Type *> params;
        params.reserve(2 + mAccessibleInputItems.size());
        params.push_back(self->getType());
        params.push_back(b.getSizeTy());
        for (Value * avail : mAccessibleInputItems) {
            params.push_back(avail->getType());
        }
        FunctionType * const type = FunctionType::get(b.getVoidTy(), params, false);
        mCurrentMethod = Function::Create(type, GlobalValue::InternalLinkage, getName() + FINAL_BLOCK_SUFFIX, b.getModule());
        mCurrentMethod->setCallingConv(CallingConv::C);
        mCurrentMethod->setDoesNotThrow();
        auto args = mCurrentMethod->arg_begin();
        args->setName("self");
        setHandle(&*args);
        remainingItems = &*(++args);
        remainingItems->setName("remainingItems");
        availableItemCount.reserve(mAccessibleInputItems.size());
        while (++args != mCurrentMethod->arg_end()) {
            availableItemCount.push_back(&*args);
        }
        assert (availableItemCount.size() == mAccessibleInputItems.size());
        mAccessibleInputItems.swap(availableItemCount);
        b.SetInsertPoint(BasicBlock::Create(b.getContext(), "entry", mCurrentMethod));
    }

    TARGET->generateFinalBlockMethod(b, remainingItems); // may be implemented by the BlockOrientedKernel subtype

    if (!b.supportsIndirectBr()) {
        b.CreateRetVoid();
        b.restoreIP(ip);
        setHandle(self);
        mAccessibleInputItems.swap(availableItemCount);
        // Restore the DoSegment function state then call the DoFinal method
        std::vector<Value *> args;
        args.reserve(2 + mAccessibleInputItems.size());
        args.push_back(self);
        args.push_back(remainingItemCount);
        args.insert(args.end(), mAccessibleInputItems.begin(), mAccessibleInputItems.end());
        b.CreateCall(mCurrentMethod->getFunctionType(), mCurrentMethod, args);
        mCurrentMethod = cp;
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateDefaultFinalBlockMethod
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockKernelCompiler::generateDefaultFinalBlockMethod(KernelBuilder & b) {
    if (b.supportsIndirectBr()) {
        BasicBlock * const resumePoint = b.CreateBasicBlock("resume");
        mStrideLoopBranch->addDestination(resumePoint);
        BasicBlock * const current = b.GetInsertBlock();
        mStrideLoopTarget->addIncoming(BlockAddress::get(resumePoint), current);
        mStrideBlockIndex->addIncoming(b.getSize(0), current);
        b.CreateBr(mStrideLoopBody);
        resumePoint->moveAfter(current);
        b.SetInsertPoint(resumePoint);
    } else {
        std::vector<Value *> args;
        args.reserve(1 + mAccessibleInputItems.size());
        args.push_back(b.getHandle());
        args.insert(args.end(), mAccessibleInputItems.begin(), mAccessibleInputItems.end());
        b.CreateCall(mDoBlockMethod->getFunctionType(), mDoBlockMethod, args);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
BlockKernelCompiler::BlockKernelCompiler(BlockOrientedKernel * const kernel) noexcept
: KernelCompiler(kernel) {

}

}

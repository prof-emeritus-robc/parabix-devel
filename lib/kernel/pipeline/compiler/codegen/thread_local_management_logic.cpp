#include "../pipeline_compiler.hpp"
#include <queue>
#include <stack>

// #define VERIFY_THREAD_LOCAL_OUTPUT_COPY

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addThreadLocalPartitionProperties
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::addThreadLocalPartitionProperties(KernelBuilder & b, const size_t partitionId, const size_t groupId) {
    if (out_degree(partitionId, ThreadLocalPlacement) > 0) {
        mTarget->addThreadLocalScalar(b.getSizeTy(), PARTITION_THREAD_LOCAL_STREAMSET_MAX_STRIDE_COUNT + std::to_string(partitionId), groupId);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeThreadLocalMemory
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::initializeThreadLocalMemory(KernelBuilder & b, Value * segmentSize) {

    if (num_edges(ThreadLocalPlacement) == 0) {
        return;
    }

    using Vertex = ThreadLocalPlacementGraph::vertex_descriptor;
    using Edge = ThreadLocalPlacementGraph::edge_descriptor;

    const auto n = (LastStreamSet - FirstStreamSet) + 1 + PartitionCount;

    const auto bw = b.getBitBlockWidth();

    std::vector<Value *> precalculatedOffset(n, nullptr);
    ConstantInt * const sz_ZERO = b.getSize(0);

    segmentSize = b.CreateUMax(segmentSize, b.getSize(MinimumThreadLocalSegmentSize));

    const Rational T{mTarget->getStride(), b.getBitBlockWidth()};

    const auto m = PartitionCount + n;
    assert (num_vertices(ThreadLocalPlacement) == m + 1);
    assert (in_degree(m, ThreadLocalPlacement) > 0);

    std::function<Value *(Vertex)> calculatePlacement = [&](const Vertex v) -> Value * {
        assert (v >= PartitionCount);
        const auto streamSet = v - PartitionCount;
        assert (streamSet < m);
        Value * offset = precalculatedOffset[streamSet];
        if (offset) {
            return offset;
        }
        const auto & Tv = ThreadLocalPlacement[v];
        Value * const maxStrides = b.CreateAdd(segmentSize, b.getSize(Tv.OverflowStrideAdjustment));
        assert (in_degree(v, ThreadLocalPlacement) > 0);
        ThreadLocalPlacementGraph::in_edge_iterator begin, end;
        std::tie(begin, end) = in_edges(v, ThreadLocalPlacement);
        Rational r = ThreadLocalPlacement[*begin];
//        if (streamSet <= LastStreamSet) {
//            const BufferNode & bn = mBufferGraph[streamSet];
//            r *= bn.RelativeIORate;
//        }
        Value * off = b.CreateCeilUMulRational(maxStrides, r);
        const auto first = source(*begin, ThreadLocalPlacement);
        if (first < PartitionCount) {
            assert (in_degree(v, ThreadLocalPlacement) == 1);
        } else {
            Value * prior = calculatePlacement(first);
            for (auto ei = begin; ++ei != end; ) {
                prior = b.CreateUMax(prior, calculatePlacement(source(*ei, ThreadLocalPlacement)));
            }
            assert (prior);
            off = b.CreateAdd(off, prior);
        }
        precalculatedOffset[streamSet] = off;
        return off;
    };

    ThreadLocalPlacementGraph::in_edge_iterator ei, end;
    std::tie(ei, end) = in_edges(m, ThreadLocalPlacement);
    Value * memorySize = calculatePlacement(source(*ei, ThreadLocalPlacement));
    while (++ei != end) {
        auto ms = calculatePlacement(source(*ei, ThreadLocalPlacement));
        memorySize = b.CreateUMax(memorySize, ms);
    }

    assert (memorySize);
    const auto pageSize = getPageSize();
    assert (is_pow2(pageSize));
    memorySize = b.CreateShl(memorySize, b.getSize(floor_log2(pageSize)));
    assert (mTarget->hasThreadLocal());
    Value * const base = b.CreateAlignedMalloc(memorySize, pageSize);
    PointerType * const int8PtrTy = b.getInt8PtrTy();
    b.setScalarField(BASE_THREAD_LOCAL_STREAMSET_MEMORY, b.CreatePointerCast(base, int8PtrTy));
    b.setScalarField(BASE_THREAD_LOCAL_STREAMSET_MEMORY_BYTES, memorySize);
    for (size_t partId = 1; partId < PartitionCount; ++partId) {
        if (out_degree(partId, ThreadLocalPlacement) > 0) {
            const auto f = FirstKernelInPartition[partId];
            const auto max = MaximumNumOfStrides[f];
            b.setScalarField(PARTITION_THREAD_LOCAL_STREAMSET_MAX_STRIDE_COUNT + std::to_string(partId), b.getSize(max));
        }
    }

}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeThreadLocalMemoryPhiNodes
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::initializeThreadLocalMemoryPhiNodes(KernelBuilder & b) {

    assert (mIsPartitionRoot);

    const auto oneAfterLastKernel = FirstKernelInPartition[mCurrentPartitionId + 1];

    IntegerType * const sizeTy = b.getSizeTy();

    PointerType * const int8PtrTy = b.getInt8PtrTy();
    const auto prefix = makeKernelName(mKernelId);
    mThreadLocalStreamSetBaseAddressAtEntryPhi = PHINode::Create(int8PtrTy, 2, prefix + "_threadLocalBaseAddressPhi", mKernelLoopEntry);
    mThreadLocalStreamSetBaseAddressAtEntryPhi->addIncoming(UndefValue::get(int8PtrTy), mKernelLoopStart);
    mThreadLocalStreamSetBaseAddress = mThreadLocalStreamSetBaseAddressAtEntryPhi;

    #ifndef NDEBUG
    mThreadLocalStartOffsetAtEntryPhi.reset(FirstStreamSet, LastStreamSet + PartitionCount);
    mThreadLocalStartOffsetAtExitPhi.reset(FirstStreamSet, LastStreamSet + PartitionCount);
    #endif

    Constant * const undefVal = UndefValue::get(sizeTy);
    for (auto streamSet : mIsThreadLocalStreamSet) {
        assert (FirstStreamSet <= streamSet && streamSet <= (LastStreamSet + PartitionCount));
        PHINode * const entryPhi = PHINode::Create(sizeTy, 2, prefix + "_threadLocalOffsetAtLoopEntry", mKernelLoopEntry);
        mThreadLocalStartOffsetAtEntryPhi[streamSet] = entryPhi;
        entryPhi->addIncoming(undefVal, mKernelLoopStart);

        PHINode * const entryEndPhi = PHINode::Create(sizeTy, 2, prefix + "_threadLocalEndOffsetAtLoopEntry", mKernelLoopEntry);
        mThreadLocalEndOffsetAtEntryPhi[streamSet] = entryEndPhi;
        entryEndPhi->addIncoming(undefVal, mKernelLoopStart);

        mThreadLocalStartOffsetAtExitPhi[streamSet] = PHINode::Create(sizeTy, 2, prefix + "_threadLocalStartOffsetAtLoopExit", mKernelLoopExit);
        mThreadLocalEndOffsetAtExitPhi[streamSet] = PHINode::Create(sizeTy, 2, prefix + "_threadLocalEndOffsetAtLoopExit", mKernelLoopExit);
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief updateThreadLocalMemoryLoopEntryPhiNodes
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::updateThreadLocalMemoryLoopEntryPhiNodes(KernelBuilder & b) {

    assert (mIsPartitionRoot);

    assert (mThreadLocalStreamSetBaseAddress);
    assert (mThreadLocalStreamSetBaseAddressAtEntryPhi);

    BasicBlock * const exitBlock = b.GetInsertBlock();

    mThreadLocalStreamSetBaseAddressAtEntryPhi->addIncoming(mThreadLocalStreamSetBaseAddress, exitBlock);

    for (auto streamSet : mIsThreadLocalStreamSet) {
        mThreadLocalStartOffsetAtEntryPhi[streamSet]->addIncoming(mThreadLocalStartOffset[streamSet], exitBlock);
        mThreadLocalEndOffsetAtEntryPhi[streamSet]->addIncoming(mThreadLocalEndOffset[streamSet], exitBlock);
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief updateThreadLocalMemoryLoopExitPhiNodes
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::updateThreadLocalMemoryLoopExitPhiNodes(KernelBuilder & b) {

    assert (mIsPartitionRoot);

    assert (mThreadLocalStreamSetBaseAddress);

    BasicBlock * const exitBlock = b.GetInsertBlock();

    mThreadLocalStreamSetBaseAddressAtExitPhi->addIncoming(mThreadLocalStreamSetBaseAddress, exitBlock);

    for (auto streamSet : mIsThreadLocalStreamSet) {
        mThreadLocalStartOffsetAtExitPhi[streamSet]->addIncoming(mThreadLocalStartOffset[streamSet], exitBlock);
        mThreadLocalEndOffsetAtExitPhi[streamSet]->addIncoming(mThreadLocalEndOffset[streamSet], exitBlock);
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief updateThreadLocalMemoryAtInsufficentIOPhiNodes
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::updateThreadLocalMemoryAtInsufficentIOPhiNodes(KernelBuilder & b) {
    BasicBlock * const exitBlock = b.GetInsertBlock();
    mThreadLocalStreamSetBaseAddressAtExitPhi->addIncoming(UndefValue::get(b.getInt8PtrTy()), exitBlock);
    Constant * undefVal = UndefValue::get(b.getSizeTy());
    for (auto streamSet : mIsThreadLocalStreamSet) {
        mThreadLocalStartOffsetAtExitPhi[streamSet]->addIncoming(undefVal, exitBlock);
        mThreadLocalEndOffsetAtExitPhi[streamSet]->addIncoming(undefVal, exitBlock);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief updateThreadLocalMemoryAfterTermination
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::updateThreadLocalMemoryAfterTerminationPhiNodes(KernelBuilder & b) {
    BasicBlock * const exitBlock = b.GetInsertBlock();
    mThreadLocalStreamSetBaseAddressAtExitPhi->addIncoming(mThreadLocalStreamSetBaseAddress, exitBlock);
    for (auto streamSet : mIsThreadLocalStreamSet) {
        mThreadLocalStartOffsetAtExitPhi[streamSet]->addIncoming(mThreadLocalStartOffset[streamSet], exitBlock);
        mThreadLocalStartOffset[streamSet] = mThreadLocalStartOffsetAtExitPhi[streamSet];
        mThreadLocalEndOffsetAtExitPhi[streamSet]->addIncoming(mThreadLocalEndOffset[streamSet], exitBlock);
        mThreadLocalEndOffset[streamSet] = mThreadLocalEndOffsetAtExitPhi[streamSet];
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief scaleThreadLocalMemoryToMaximumNumOfStrides
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::allocateThreadLocalMemoryForMaximumNumOfStrides(KernelBuilder & b, Value * maximumNumOfStrides, Value * nonCountableNumOfStrides) {

    assert (mIsPartitionRoot);

    if (out_degree(mCurrentPartitionId, ThreadLocalPlacement) == 0) {
        assert (mIsThreadLocalStreamSet.size() == 0);
        return;
    }
    assert (mIsThreadLocalStreamSet.size() > 0);

    BasicBlock * const allocateThreadLocal = b.CreateBasicBlock("allocateThreadLocal", mKernelLoopCall);
    BasicBlock * const expandThreadLocalMemory = b.CreateBasicBlock("expandThreadLocalMemory", mKernelLoopCall);
    BasicBlock * const copyOrAcceptThreadLocalMemoryLayout = b.CreateBasicBlock("copyOrAcceptThreadLocalMemoryLayout", mKernelLoopCall);
    BasicBlock * const copyThreadLocalMemory = b.CreateBasicBlock("copyThreadLocalMemory", mKernelLoopCall);
    BasicBlock * const copyThreadLocalMemoryExit = b.CreateBasicBlock("copyThreadLocalMemoryExit", mKernelLoopCall);
    BasicBlock * const afterExpansion = b.CreateBasicBlock("afterExpansion", mKernelLoopCall);
    BasicBlock * const allocateThreadLocalExit = b.CreateBasicBlock("allocateThreadLocalExit", mKernelLoopCall);

    PointerType * const int8PtrTy = b.getInt8PtrTy();

    IntegerType * const sizeTy = b.getSizeTy();

    IntegerType * const int8Ty = b.getInt8Ty();

    BasicBlock * const entryBlock = b.GetInsertBlock();

    // TODO: if a non-countable port was a port that most limited the provided max num of strides,
    // we want to double the thread-local memory of this partition to reduce the chance we've
    // wrongly throttled the dataflow.

    assert (nonCountableNumOfStrides || maximumNumOfStrides);

    if (LLVM_UNLIKELY(maximumNumOfStrides == nullptr)) {
        maximumNumOfStrides = b.CreateMul(nonCountableNumOfStrides, b.getSize(4));
    }
    maximumNumOfStrides = b.CreateAdd(mCurrentNumOfStridesAtLoopEntryPhi, maximumNumOfStrides);

    Value * threadLocalPtr = nullptr;
    Type * threadLocalTy = nullptr;
    std::tie(threadLocalPtr, threadLocalTy) = b.getScalarFieldPtr(BASE_THREAD_LOCAL_STREAMSET_MEMORY);

    Value * maxStrideCountPtr; Type * maxStrideCountPtrTy;
    std::tie(maxStrideCountPtr, maxStrideCountPtrTy) = b.getScalarFieldPtr(PARTITION_THREAD_LOCAL_STREAMSET_MAX_STRIDE_COUNT + std::to_string(mCurrentPartitionId));
    assert (maxStrideCountPtrTy == sizeTy);

    Value * const initialMaxStride = b.CreateAlignedLoad(sizeTy, maxStrideCountPtr, SizeTyABIAlignment);

    Value * const hasEnough = b.CreateICmpULE(maximumNumOfStrides, initialMaxStride);

    Value * const noRecalcOrReallocNeeded = b.CreateAnd(hasEnough, mExecutedAtLeastOnceAtLoopEntryPhi);

    b.CreateCondBr(noRecalcOrReallocNeeded, allocateThreadLocalExit, allocateThreadLocal);

    b.SetInsertPoint(allocateThreadLocal);

    const auto m = PartitionCount + (LastStreamSet - FirstStreamSet) + 1 + PartitionCount;
    assert (num_vertices(ThreadLocalPlacement) == m + 1);
    std::vector<unsigned> toVisit(m + 1, 0);
    for (unsigned i = PartitionCount; i < m; ++i) {
        toVisit[i] = in_degree(i, ThreadLocalPlacement);
    }
    assert (in_degree(m, ThreadLocalPlacement) < -1U);
    toVisit[m] = -1U;

    const auto pageSize = getPageSize();
    assert (is_pow2(pageSize));

    ConstantInt * const LOG_2_PAGE_SIZE = b.getSize(floor_log2(pageSize));

    std::queue<unsigned> Q;

    Value * const sz_ZERO = b.getSize(0);

    #ifndef NDEBUG
    flat_set<unsigned> visited;
    visited.emplace(mCurrentPartitionId);
    #endif

    const auto bw = b.getBitBlockWidth();

    if (LLVM_UNLIKELY(EnableCycleCounter)) {
        startCycleCounter(b, CycleCounter::BUFFER_EXPANSION);
    }

    maximumNumOfStrides = b.CreateSelect(hasEnough, initialMaxStride, maximumNumOfStrides);

    b.CreateAlignedStore(maximumNumOfStrides, maxStrideCountPtr, SizeTyABIAlignment);

    std::vector<size_t> selected;

    #ifndef NDEBUG
    mThreadLocalStartOffset.reset(FirstStreamSet, LastStreamSet + PartitionCount);
    mThreadLocalEndOffset.reset(FirstStreamSet, LastStreamSet + PartitionCount);
    #endif

    Value * memoryForSegment = nullptr;
    for (auto u = mCurrentPartitionId;;) {
        for (auto e : make_iterator_range(out_edges(u, ThreadLocalPlacement))) {
            const auto v = target(e, ThreadLocalPlacement);
            assert (PartitionCount <= v && v <= m);
            assert (v != u);
            auto & T = toVisit[v];
            assert (T > 0);
            if (--T == 0) {
                #ifndef NDEBUG
                assert (visited.count(v) == 0);
                visited.emplace(v);
                #endif

                const auto streamSet = v + FirstStreamSet - PartitionCount;
                assert (mIsThreadLocalStreamSet.count(streamSet));
                const auto & Tv = ThreadLocalPlacement[v];

                Value * start = nullptr;
                Value * end = nullptr;

                Value * const maxStrides = b.CreateAdd(maximumNumOfStrides, b.getSize(Tv.OverflowStrideAdjustment));
                if (streamSet <= LastStreamSet) {
                    const auto producer = parent(streamSet, mBufferGraph);
                    assert (FirstKernelInPartition[mCurrentPartitionId] <= producer && producer < FirstKernelInPartition[mCurrentPartitionId + 1]);
                    if (producer == mKernelId) {
                        selected.push_back(streamSet);
                    }
                }

                Value * const off = b.CreateShl(b.CreateCeilUMulRational(maxStrides, ThreadLocalPlacement[e]), LOG_2_PAGE_SIZE);
                if (u < PartitionCount) {
                    start = sz_ZERO;
                    end = off;
                } else {
                    for (auto in : make_iterator_range(in_edges(v, ThreadLocalPlacement))) {
                        const auto src = FirstStreamSet + source(in, ThreadLocalPlacement)  - PartitionCount;
                        assert (mThreadLocalEndOffset[src]);
                        start = b.CreateUMax(start, mThreadLocalEndOffset[src]);
                    }
                    end = b.CreateAdd(start, off);
                }

                mThreadLocalStartOffset[streamSet] = start;
                mThreadLocalEndOffset[streamSet] = end;

                if (streamSet <= LastStreamSet) {

                    if (LLVM_UNLIKELY(mCheckStreamSets)) {
                        auto & dl = b.getModule()->getDataLayout();
                        const auto & bn = mBufferGraph[streamSet];
                        ExternalBuffer * const buf = cast<ExternalBuffer>(bn.Buffer);
                        const auto ts = b.getTypeSize(dl, buf->getType());
                        buf->setCapacity(b, b.CreateMulRational(off, Rational{bw, ts}));
                    }

                    for (auto inOut = streamSet; LLVM_UNLIKELY(out_degree(inOut, InOutStreamSetReplacement)); ) {
                        inOut = child(inOut, InOutStreamSetReplacement);
                        mThreadLocalStartOffset[inOut] = start;
                        mThreadLocalEndOffset[inOut] = end;
                    }
                }

                #if defined(PRINT_DEBUG_MESSAGES) && !defined(PRINT_DEBUG_MESSAGES_NO_ADDRESS_DISPLAY)
                debugPrint(b, "mappedAddrRange%" PRIu64 " = [%" PRIx64 ", %" PRIx64 ")", b.getSize(streamSet), start, end);
                #endif

                if (Tv.Terminal) {
                    memoryForSegment = b.CreateUMax(memoryForSegment, end);
                } else if (out_degree(v, ThreadLocalPlacement) > 0) {
                    Q.push(v);
                }
            }
        }
        if (Q.empty()) {
            break;
        }
        u = Q.front();
        assert (toVisit[u] == 0);
        Q.pop();
    }

    assert (memoryForSegment);
    assert ((mIsThreadLocalStreamSet.size() + 1) == visited.size() && mIsThreadLocalStreamSet.count(mCurrentPartitionId) == 0);

    if (LLVM_UNLIKELY(CheckAssertions())) {

        for (auto e : make_iterator_range(edges(ThreadLocalConflictGraph))) {
            const auto x = source(e, ThreadLocalConflictGraph) + FirstStreamSet;
            assert (FirstStreamSet <= x && x <= LastStreamSet);
            const auto producer = parent(x, mBufferGraph);
            if (KernelPartitionId[producer] == mCurrentPartitionId) {
                const auto y = target(e, ThreadLocalConflictGraph) + FirstStreamSet;
                assert (FirstStreamSet <= y && y <= LastStreamSet);
                Value * const after = b.CreateICmpULE(mThreadLocalEndOffset[x], mThreadLocalStartOffset[y]);
                Value * const before = b.CreateICmpULE(mThreadLocalEndOffset[y], mThreadLocalStartOffset[x]);
                b.CreateAssert(b.CreateOr(before, after),
                        "Thread-local "
                        "streamset %" PRIu64 " [%" PRIx64 ",%" PRIx64 ") overlaps "
                        "streamset %" PRIu64 " [%" PRIx64 ",%" PRIx64 ") when max strides=%" PRIu64,
                        b.getSize(x), mThreadLocalStartOffset[x], mThreadLocalEndOffset[x],
                        b.getSize(y), mThreadLocalStartOffset[y], mThreadLocalEndOffset[y],
                        maximumNumOfStrides);
            }
        }
    }

    mThreadLocalStreamSetBaseAddress = b.CreateAlignedLoad(int8PtrTy, threadLocalPtr, PtrTyABIAlignment);
    Value * const threadLocalMemorySizePtr = b.getScalarFieldPtr(BASE_THREAD_LOCAL_STREAMSET_MEMORY_BYTES).first;
    Value * const currentMem = b.CreateAlignedLoad(sizeTy, threadLocalMemorySizePtr, SizeTyABIAlignment);

    // TODO: is it possible we could reuse memory but need new offsets on a second pass that causes us to overlap
    // a copy of one buffer to a different one?
    Value * const expandBuffer = b.CreateICmpUGT(memoryForSegment, currentMem);
    b.CreateUnlikelyCondBr(expandBuffer, expandThreadLocalMemory, copyOrAcceptThreadLocalMemoryLayout);

    b.SetInsertPoint(expandThreadLocalMemory);
    // At minimum, we want to double the required space to minimize future reallocs
    Value * const newSize = b.CreateRoundUp(memoryForSegment, currentMem);
    b.CreateAlignedStore(newSize, threadLocalMemorySizePtr, SizeTyABIAlignment);
    Value * const newThreadLocalBaseAddress = b.CreateAlignedMalloc(newSize, pageSize);
    b.CreateAlignedStore(newThreadLocalBaseAddress, threadLocalPtr, PtrTyABIAlignment);
    b.CreateBr(copyOrAcceptThreadLocalMemoryLayout);

    // If we already processed one iteration, we have likely written something to the old thread local buffer.
    // Copy it to the correct location of the new one.
    b.SetInsertPoint(copyOrAcceptThreadLocalMemoryLayout);
    PHINode * const newThreadLocalBaseAddressPhi = b.CreatePHI(int8PtrTy, 2);
    newThreadLocalBaseAddressPhi->addIncoming(newThreadLocalBaseAddress, expandThreadLocalMemory);
    newThreadLocalBaseAddressPhi->addIncoming(mThreadLocalStreamSetBaseAddress, allocateThreadLocal);

    PHINode * sizePhi = b.CreatePHI(sizeTy, 2);
    sizePhi->addIncoming(newSize, expandThreadLocalMemory);
    sizePhi->addIncoming(currentMem, allocateThreadLocal);
    b.CreateCondBr(mExecutedAtLeastOnceAtLoopEntryPhi, copyThreadLocalMemory, afterExpansion);

    b.SetInsertPoint(copyThreadLocalMemory);
    #ifdef VERIFY_THREAD_LOCAL_OUTPUT_COPY
    Value * const copyBuffer = b.CreateAlignedMalloc(currentMem, pageSize);
    b.CreateMemCpy(copyBuffer, mThreadLocalStreamSetBaseAddress, currentMem, pageSize);
    #endif
    for (auto i = selected.rbegin(); i != selected.rend(); ++i) {
        const auto streamSet = *i;
        Value * const priorEnd = mThreadLocalEndOffsetAtEntryPhi[streamSet];
        Value * const priorStart = mThreadLocalStartOffsetAtEntryPhi[streamSet];
        Value * const priorLength = b.CreateSub(priorEnd, priorStart);
        Value * const priorPtr = b.CreateGEP(int8Ty, mThreadLocalStreamSetBaseAddress, priorStart);
        Value * const currentPtr = b.CreateGEP(int8Ty, newThreadLocalBaseAddressPhi, mThreadLocalStartOffset[streamSet]);
        b.CreateMemCpy(currentPtr, priorPtr, priorLength, pageSize);
        #ifdef VERIFY_THREAD_LOCAL_OUTPUT_COPY
        Value * const copyPtr = b.CreateGEP(int8Ty, copyBuffer, priorStart);
        Value * const match = b.CreateIsNull(b.CreateMemCmp(copyPtr, currentPtr, priorLength));
        b.CreateAssert (match, "thread local movement failed for streamset %" PRIu64, b.getSize(streamSet));
        #endif
    }
    #ifdef VERIFY_THREAD_LOCAL_OUTPUT_COPY
    b.CreateFree(copyBuffer);
    #endif
    b.CreateCondBr(expandBuffer, copyThreadLocalMemoryExit, afterExpansion);

    b.SetInsertPoint(copyThreadLocalMemoryExit);
    b.CreateFree(mThreadLocalStreamSetBaseAddress);
    b.CreateBr(afterExpansion);

    b.SetInsertPoint(afterExpansion);
    if (LLVM_UNLIKELY(EnableCycleCounter)) {
        updateCycleCounter(b, mKernelId, CycleCounter::BUFFER_EXPANSION);
    }
    b.CreateBr(allocateThreadLocalExit);

    b.SetInsertPoint(allocateThreadLocalExit);
    PHINode * const threadLocalBaseAttrPhi = b.CreatePHI(int8PtrTy, 2);
    threadLocalBaseAttrPhi->addIncoming(mThreadLocalStreamSetBaseAddressAtEntryPhi, entryBlock);
    threadLocalBaseAttrPhi->addIncoming(newThreadLocalBaseAddressPhi, afterExpansion);
    mThreadLocalStreamSetBaseAddress = threadLocalBaseAttrPhi;

    for (auto streamSet : mIsThreadLocalStreamSet) {
        PHINode * const startPhi = b.CreatePHI(sizeTy, 2);
        startPhi->addIncoming(mThreadLocalStartOffsetAtEntryPhi[streamSet], entryBlock);
        startPhi->addIncoming(mThreadLocalStartOffset[streamSet], afterExpansion);
        mThreadLocalStartOffset[streamSet] = startPhi;

        PHINode * const endPhi = b.CreatePHI(sizeTy, 2);
        endPhi->addIncoming(mThreadLocalEndOffsetAtEntryPhi[streamSet], entryBlock);
        endPhi->addIncoming(mThreadLocalEndOffset[streamSet], afterExpansion);
        mThreadLocalEndOffset[streamSet] = endPhi;
    }

    remapThreadLocalBufferMemory(b);
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief assignThreadLocalBufferMemoryForPartition
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::remapThreadLocalBufferMemory(KernelBuilder & b) {

    const auto blockWidth = b.getBitBlockWidth();

    auto & DL = b.getModule()->getDataLayout();

    for (const auto output : make_iterator_range(out_edges(mKernelId, mBufferGraph))) {
        const auto streamSet = target(output, mBufferGraph);
        assert (FirstStreamSet <= streamSet && streamSet <= LastStreamSet);
        const BufferNode & bn = mBufferGraph[streamSet];
        if (bn.isThreadLocal()) {

            assert (out_degree(mCurrentPartitionId, ThreadLocalPlacement) > 0);

            ExternalBuffer * const buffer = cast<ExternalBuffer>(bn.Buffer);
            Value * const produced = mInitiallyProducedItemCount[streamSet];
            PointerType * const ptrTy = buffer->getPointerType();

            const auto typeWidth = b.getTypeSize(DL, buffer->getType());

            const auto fieldWidth = buffer->getFieldWidth();

            Value * offsetBytes = nullptr;
            if (fieldWidth == 1) {
                Rational bytesPerItem{typeWidth, blockWidth};
                offsetBytes = b.CreateMulRational(produced, bytesPerItem);
            } else {
                // NOTE: these multiplications compute the floor of each equation and are not
                // equivalent to the fieldWidth=1 calculation despite appearing so.
                Rational fieldsPerBlock{fieldWidth, blockWidth};
                Value * const offsetVecOffset = b.CreateMulRational(produced, fieldsPerBlock);
                Rational bytesPerBlock{typeWidth, fieldWidth};
                offsetBytes = b.CreateMulRational(offsetVecOffset, bytesPerBlock);
            }
            Value * const startOffset = mThreadLocalStartOffset[streamSet]; assert (startOffset);
            Value * const virtualBaseOffset = b.CreateSub(startOffset, offsetBytes);
            Value * const ba = b.CreateGEP(b.getInt8Ty(), mThreadLocalStreamSetBaseAddress, virtualBaseOffset);
            buffer->setBaseAddress(b, b.CreatePointerCast(ba, ptrTy));

        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief identifyAllThreadLocalStreamSetsInCurrentPartition
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::identifyAllThreadLocalStreamSetsInCurrentPartition() {

    const auto n = (LastStreamSet - FirstStreamSet) + 1 + PartitionCount;
    const auto m = PartitionCount + n;
    assert (num_vertices(ThreadLocalPlacement) == m + 1);


    std::queue<unsigned> Q;
    mIsThreadLocalStreamSet.clear();
    for (auto u = mCurrentPartitionId;;) {
        for (auto e : make_iterator_range(out_edges(u, ThreadLocalPlacement))) {
            const auto v = target(e, ThreadLocalPlacement);
            assert (PartitionCount <= v && v <= m);
            if (LLVM_UNLIKELY(v == m)) {
                continue;
            }
            const auto streamSet = v + FirstStreamSet - PartitionCount;
            assert (FirstStreamSet <= streamSet && streamSet <= (LastStreamSet + PartitionCount));
            if (mIsThreadLocalStreamSet.count(streamSet) == 0) {
                mIsThreadLocalStreamSet.insert(streamSet);
                Q.push(v);
            }
        }
        if (Q.empty()) {
            break;
        }
        u = Q.front();
        Q.pop();
    }
    assert (mIsThreadLocalStreamSet.size() > 0 || out_degree(mCurrentPartitionId, ThreadLocalPlacement) == 0);

}

}

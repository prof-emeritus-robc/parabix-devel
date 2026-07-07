#include "popcount_kernel.h"

#include <kernel/core/kernel_builder.h>
#include <boost/intrusive/detail/math.hpp>

// This option is mostly for testing lookbehind of an input streamset but
// creates a cross-thread memory dependency that is otherwise unnecessary.

// #define USE_LOOKBEHIND_FOR_LAST_VALUE // must match pipeline/compiler/config.h

using namespace llvm;

namespace kernel {

const std::string INPUT = "input";

const std::string OUTPUT_STREAM = "output";
const std::string POSITIVE_STREAM = "positive";
const std::string NEGATIVE_STREAM = "negative";

const std::string CURRENT_COUNT = "currentCount";
const std::string POSITIVE_COUNT = "positiveCount";
const std::string NEGATIVE_COUNT = "negativeCount";

using Rational = ProcessingRate::Rational;

bool isNotConstantOne(Value * const value) {
    return !isa<Constant>(value) || !cast<Constant>(value)->isOneValue();
}

using boost::intrusive::detail::floor_log2;

// #define PRINT_POP_COUNTS_TO_STDERR

// #define PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY

// #define PRINT_DEBUG_MESSAGES_INCLUDE_THREAD_NUM

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateMultiBlockLogic
 ** ------------------------------------------------------------------------------------------------------------- */
void PopCountKernel::generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) {


    Constant * const sz_ZERO = b.getSize(0);
    Constant * const sz_ONE = b.getSize(1);
    #ifdef USE_LOOKBEHIND_FOR_LAST_VALUE
    Constant * const NEG_ONE = ConstantExpr::getNeg(ONE);
    #endif
    IntegerType * const sizeTy = b.getSizeTy();

    const Binding & input = b.getInputStreamSetBinding(INPUT);
    const ProcessingRate & rate = input.getRate();
    const Rational & rv = rate.getRate();
    assert (rv.denominator() == 1);
    const auto inputWidth = rv.numerator();
    const auto blockWidth = b.getBitBlockWidth();
    const auto sizeWidth = sizeTy->getBitWidth();

    if (isNotConstantOne(b.getInputStreamSetCount(INPUT))) {
        report_fatal_error("PopCount input stream must be a single stream");
    }

    Value * position = nullptr;
    if (LLVM_LIKELY(mType != PopCountType::BOTH)) {
        position = b.getProducedItemCount(OUTPUT_STREAM);
    } else {
        position = b.getProducedItemCount(POSITIVE_STREAM);
    }

    #if defined(PRINT_POP_COUNTS_TO_STDERR) || defined(PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY)
    ConstantInt * const STDERR = b.getInt32(STDERR_FILENO);

    Function * pthreadSelfFn = b.getModule()->getFunction("pthread_self");
    if (pthreadSelfFn == nullptr) {
        IntegerType * const pThreadTy = IntegerType::getIntNTy(b.getContext(), sizeof(pthread_t) * CHAR_BIT);
        FunctionType * funTy = FunctionType::get(pThreadTy, false);
        pthreadSelfFn = b.LinkFunction("pthread_self", funTy, (void*)&pthread_self);
    }

    #ifdef PRINT_DEBUG_MESSAGES_INCLUDE_THREAD_NUM
    Value * const _pThreadNum = b.CreateCall(pthreadSelfFn);
    #endif

    auto debugPrint = [&](const StringRef label, auto &&... params) {
        std::string tmp;
        raw_string_ostream out(tmp);
        #ifdef PRINT_DEBUG_MESSAGES_INCLUDE_THREAD_NUM
        out << "%016" PRIx64;
        #endif
        out << "  " << label << "\n";
        #ifdef PRINT_DEBUG_MESSAGES_INCLUDE_THREAD_NUM
        b.CreateDprintfCall(STDERR, out.str(), _pThreadNum, params...);
        #else
        b.CreateDprintfCall(STDERR, out.str(), params...);
        #endif
    };

    debugPrint("initial position = %" PRIu64, position);
    #endif

    // TODO: load the initial counts from a lookbehind
    Value * positiveArray = nullptr;
    Value * initialPositiveCount = nullptr;
    Value * negativeArray = nullptr;
    Value * initialNegativeCount = nullptr;

    if (LLVM_LIKELY(mType == PopCountType::POSITIVE || mType == PopCountType::NEGATIVE)) {
        Value * const array = b.getRawOutputPointer(OUTPUT_STREAM, position);
        #ifdef USE_LOOKBEHIND_FOR_LAST_VALUE
        Value * const count = b.CreateLoad(b.CreateInBoundsGEP(sizeTy, array, NEG_ONE));
        #else
        Value * const count = b.getScalarField("count");
        #endif
        if (LLVM_LIKELY(mType == PopCountType::POSITIVE)) {
            positiveArray = array;
            initialPositiveCount = count;
            #if defined(PRINT_POP_COUNTS_TO_STDERR) || defined(PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY)
            debugPrint("initial count(pos) = %" PRIu64, count);
            #endif
        } else { // if (mType == PopCountType::NEGATIVE) {
            negativeArray = array;
            initialNegativeCount = count;
            #if defined(PRINT_POP_COUNTS_TO_STDERR) || defined(PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY)
            debugPrint("initial count(neg) = %" PRIu64, count);
            #endif
        }
    } else { // if (mType == PopCountType::BOTH) {
        positiveArray = b.getRawOutputPointer(POSITIVE_STREAM, position);
        negativeArray = b.getRawOutputPointer(NEGATIVE_STREAM, position);
        #ifdef USE_LOOKBEHIND_FOR_LAST_VALUE
        initialPositiveCount = b.CreateLoad(b.CreateInBoundsGEP(sizeTy, positiveArray, NEG_ONE));
        initialNegativeCount = b.CreateLoad(b.CreateInBoundsGEP(sizeTy, negativeArray, NEG_ONE));
        #else
        initialPositiveCount = b.getScalarField("posCount");
        initialNegativeCount = b.getScalarField("negCount");
        #endif
        #if defined(PRINT_POP_COUNTS_TO_STDERR) || defined(PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY)
        debugPrint("initial count(pos) = %" PRIu64, initialPositiveCount);
        debugPrint("initial count(neg) = %" PRIu64, initialNegativeCount);
        #endif
    }


    Value * positivePartialSum = nullptr;
    Value * negativePartialSum = nullptr;

    Module * const m = b.getModule();
    auto & DL = m->getDataLayout();

    IntegerType * const intTy = b.getIntNTy(inputWidth);

    const auto sizeTyAlign = b.getAlignOf(DL, sizeTy);


    // ----------------------------------------------------------------------------------------
    // handle initial partial blocks for short strides
    // ----------------------------------------------------------------------------------------
    if (inputWidth < blockWidth) {



        const auto outputFieldsPerBlock = blockWidth / sizeWidth;


        const auto steps = blockWidth / inputWidth;
        assert (is_power_2(steps));
        ConstantInt * const sz_STEP_MASK = b.getSize(steps - 1);
        ConstantInt * const sz_STEPS = b.getSize(steps);

        VectorType * const vecTy = VectorType::get(intTy, steps, false);

        const auto vecTyAlign = b.getAlignOf(DL, vecTy);

        PointerType * const ptrVecTy = vecTy->getPointerTo();

        VectorType * const sizeVecTy = VectorType::get(sizeTy, outputFieldsPerBlock, false);

        auto generateIterativePopCountCode = [&](Value * const writeStart,
                Value * const inputIndex, Value * const inputOffset,
                const bool fromOffset,
                Value *& positiveCount, Value *& negativeCount) -> Value * {

            if (LLVM_LIKELY(b.supportsIndirectBr())) {

                PointerType * const i8PtrTy = b.getInt8PtrTy();
                const auto i8PtrTyAlign = DL.getABITypeAlign(i8PtrTy).value();

                SmallVector<BasicBlock *, 32> stepEntryPoint(steps);
                for (unsigned i = 1; i < steps; ++i) {
                    stepEntryPoint[i] = b.CreateBasicBlock("");
                }
                stepEntryPoint[0] = b.CreateBasicBlock("");

                BasicBlock * const entry = b.GetInsertBlock();
                Function * const f = entry->getParent();

                SmallVector<Constant *, 32> stepEntryPointAddr(steps);
                for (unsigned i = 0; i < steps; ++i) {
                    unsigned jumpIndex;
                    if (fromOffset) {
                        jumpIndex = i;
                    } else {
                        jumpIndex = (steps - i) % steps;
                    }
                    assert (jumpIndex != 0 || i == 0);
                    stepEntryPointAddr[i] = BlockAddress::get(f, stepEntryPoint[jumpIndex]);
                }

                ArrayType * const stepEntryPointAddrTy = ArrayType::get(i8PtrTy, steps);
                Constant * const stepEntryPointAddrArray = ConstantArray::get(stepEntryPointAddrTy, stepEntryPointAddr);

                GlobalVariable * const stepEntryTargetArray =
                    new GlobalVariable(*m, stepEntryPointAddrTy, true, GlobalValue::InternalLinkage, stepEntryPointAddrArray);

                Value * const inputPtr = b.CreatePointerCast(b.getInputStreamBlockPtr(INPUT, sz_ZERO, inputIndex), intTy->getPointerTo());

                FixedArray<Value *, 2> jumpIndex;
                jumpIndex[0] = sz_ZERO;
                Value * const remaining = b.CreateSub(numOfStrides, writeStart);
                Value * c = nullptr;
                Value * jumpPoint = nullptr;
                if (fromOffset) {
                    c = b.CreateICmpULT(remaining, sz_STEPS);
                    jumpPoint = b.CreateSelect(c, sz_ZERO, inputOffset);
                } else {
                    jumpPoint = remaining;
                }
                jumpIndex[1] = jumpPoint;

                Value * const initialJumpTargetPtr = b.CreateGEP(stepEntryPointAddrTy, stepEntryTargetArray, jumpIndex);
                Value * const initialJumpTarget = b.CreateAlignedLoad(i8PtrTy, initialJumpTargetPtr, i8PtrTyAlign);
                IndirectBrInst * const br = b.CreateIndirectBr(initialJumpTarget, steps);

                SmallVector<PHINode *, 32> offsetPhi(steps);
                SmallVector<PHINode *, 32> positiveSumPhi(steps);
                SmallVector<PHINode *, 32> negativeSumPhi(steps);
                for (unsigned i = 0; i < steps; ++i) {
                    offsetPhi[i] = PHINode::Create(sizeTy, 2, "", stepEntryPoint[i]);
                    offsetPhi[i]->addIncoming(sz_ZERO, entry);
                    if (positiveArray) {
                        positiveSumPhi[i] = PHINode::Create(sizeTy, 2, "", stepEntryPoint[i]);
                        positiveSumPhi[i]->addIncoming(positiveCount, entry);
                    }
                    if (negativeArray) {
                        negativeSumPhi[i] = PHINode::Create(sizeTy, 2, "", stepEntryPoint[i]);
                        negativeSumPhi[i]->addIncoming(negativeCount, entry);
                    }
                    br->addDestination(stepEntryPoint[i]);
                }

                Value * nextOffset = nullptr;

                for (unsigned i = 1; i < steps; ++i) {
                    b.SetInsertPoint(stepEntryPoint[i]);
                    Value * const readOffset = b.CreateAdd(inputOffset, offsetPhi[i]);
                    Value * const ptr = b.CreateInBoundsGEP(intTy, inputPtr, readOffset);
                    Value * const value = b.CreateAlignedLoad(intTy, ptr, b.getAlignOf(DL, intTy));
                    Value * const sum = b.CreateZExt(b.CreatePopcount(value), sizeTy);
                    Value * writeOffset = b.CreateAdd(writeStart, offsetPhi[i]);
                    const auto next = (i + 1) % steps;
                    if (positiveArray) {
                        Value * positivePartialSum = b.CreateAdd(positiveSumPhi[i], sum);
                        Value * const ptr = b.CreateInBoundsGEP(sizeTy, positiveArray, writeOffset);
                        b.CreateAlignedStore(positivePartialSum, ptr, sizeTyAlign);
                        positiveSumPhi[next]->addIncoming(positivePartialSum, stepEntryPoint[i]);
                    }
                    if (negativeArray) {
                        Value * negSum = b.CreateSub(b.getSize(inputWidth), sum);
                        Value * negativePartialSum = b.CreateAdd(negativeSumPhi[i], negSum);
                        Value * const ptr = b.CreateInBoundsGEP(sizeTy, negativeArray, writeOffset);
                        b.CreateAlignedStore(negativePartialSum, ptr, sizeTyAlign);
                        negativeSumPhi[next]->addIncoming(negativePartialSum, stepEntryPoint[i]);
                    }
                    nextOffset = b.CreateAdd(offsetPhi[i], sz_ONE);
                    offsetPhi[next]->addIncoming(nextOffset, stepEntryPoint[i]);
                    b.CreateBr(stepEntryPoint[next]);
                }

                b.SetInsertPoint(stepEntryPoint[0]);
                if (positiveArray) {
                    positiveCount = positiveSumPhi[0];
                }
                if (negativeArray) {
                    negativeCount = negativeSumPhi[0];
                }
                return offsetPhi[0];

            } else { // no non-indirect branch support

                llvm::report_fatal_error("Not supported yet");

            }
        };

        Value * startBlockOffset = b.CreateAnd(position, sz_STEP_MASK);

        ConstantInt * const sz_LOG_2_STEPS = b.getSize(floor_log2(steps));

        Value * posCount = initialPositiveCount;
        Value * negCount = initialNegativeCount;

        Value * const numProcessed =
            generateIterativePopCountCode(sz_ZERO, sz_ZERO, startBlockOffset, true, posCount, negCount);

        if (positiveArray) {
            posCount = b.simd_fill(sizeWidth, posCount);
        }
        if (negativeArray) {
            negCount = b.simd_fill(sizeWidth, negCount);
        }

        startBlockOffset = b.CreateAdd(startBlockOffset, numProcessed);

        Value * const firstIndex = b.CreateLShr(startBlockOffset, sz_LOG_2_STEPS);

        Value * const totalNumOfStrides = b.CreateAdd(b.CreateLShr(b.CreateSub(numOfStrides, numProcessed), sz_LOG_2_STEPS), firstIndex);

        // ----------------------------------------------------------------------------------------
        // process all full vector width blocks
        // ----------------------------------------------------------------------------------------

        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const popCountLoop = b.CreateBasicBlock("Loop");
        BasicBlock * const popCountExit = b.CreateBasicBlock("Exit");

        b.CreateCondBr(b.CreateICmpNE(firstIndex, totalNumOfStrides), popCountLoop, popCountExit);

        b.SetInsertPoint(popCountLoop);
        PHINode * const indexPhi = b.CreatePHI(sizeTy, 2);
        indexPhi->addIncoming(firstIndex, entry);
        PHINode * const writePosPhi = b.CreatePHI(sizeTy, 2);
        writePosPhi->addIncoming(numProcessed, entry);

        PHINode * positiveSumPhi = nullptr;
        if (positiveArray) {
            positiveSumPhi = b.CreatePHI(sizeVecTy, 2);
            positiveSumPhi->addIncoming(posCount, entry);
        }
        PHINode * negativeSumPhi = nullptr;
        if (negativeArray) {
            negativeSumPhi = b.CreatePHI(sizeVecTy, 2);
            negativeSumPhi->addIncoming(negCount, entry);
        }
        Value * value = b.loadInputStreamBlock(INPUT, sz_ZERO, indexPhi);
        if (LLVM_UNLIKELY(positiveSumPhi == nullptr)) { // only negative count
            value = b.CreateNot(value);
        }
        Value * const count = b.simd_popcount(inputWidth, value);
        Value * partialSum = b.hsimd_partial_sum(inputWidth, count);
        assert (partialSum->getType() == vecTy);

        Value * newPositiveSum = nullptr;
        Value * newNegativeSum = nullptr;

        if (inputWidth <= sizeWidth) {

            assert ((sizeWidth % inputWidth) == 0);

            const auto inputTypePerSizeField = (sizeWidth / inputWidth);

            assert ((outputFieldsPerBlock * inputTypePerSizeField) == steps);

            SmallVector<Value *, 16> partialSumArray(inputTypePerSizeField, nullptr);
            partialSumArray[0] = partialSum;
            for (unsigned i = 1; i < inputTypePerSizeField; i *= 2) {
                const auto fw = inputWidth * i;
                assert ((blockWidth % fw) == 0);
                const auto fieldCount = blockWidth / fw;

                const auto count = inputTypePerSizeField / i;
                const auto half = count / 2; assert (half > 0);

                VectorType * const unpackTy = VectorType::get(b.getIntNTy(fw), fieldCount, false);
                Constant * const nil = ConstantVector::getNullValue(unpackTy);

                for (unsigned j = 0; j < inputTypePerSizeField; j += count) {
                    assert (partialSumArray[j]);
                    Value * vec = b.CreateBitCast(partialSumArray[j], unpackTy);
                    partialSumArray[j] = b.esimd_mergel(fw, vec, nil);
                    assert ((j + half) < inputTypePerSizeField);
                    assert (partialSumArray[j + half] == nullptr);
                    partialSumArray[j + half] = b.esimd_mergeh(fw, vec, nil);
                }
            }

            if (positiveArray) {
                for (unsigned i = 0; i < inputTypePerSizeField; ++i) {
                    Value * const sum = b.CreateBitCast(partialSumArray[i], sizeVecTy);
                    newPositiveSum = b.CreateAdd(sum, positiveSumPhi);
                    Value * const idx = b.CreateAdd(writePosPhi, b.getSize(i * outputFieldsPerBlock));
                    Value * const ptr = b.CreateInBoundsGEP(sizeTy, positiveArray, idx);
                    b.CreateAlignedStore(newPositiveSum, b.CreatePointerCast(ptr, ptrVecTy), vecTyAlign);
                }
                newPositiveSum = b.mvmd_srli(sizeWidth, newPositiveSum, outputFieldsPerBlock - 1);
                for (unsigned j = 1; j < outputFieldsPerBlock; j *= 2) {
                    Value * const shifted = b.mvmd_slli(sizeWidth, newPositiveSum, j);
                    newPositiveSum = b.CreateOr(newPositiveSum, shifted);
                }
                positiveSumPhi->addIncoming(newPositiveSum, popCountLoop);
            }

            if (negativeArray) {
                Constant * vec_IW = nullptr;
                if (positiveArray) {
                    vec_IW = ConstantVector::getSplat(sizeVecTy->getElementCount(), b.getSize(inputWidth));
                }
                for (unsigned i = 0; i < inputTypePerSizeField; ++i) {
                    Value * sum = b.CreateBitCast(partialSumArray[i], sizeVecTy);
                    if (positiveArray) {
                        sum = b.CreateSub(vec_IW, sum);
                    }
                    newNegativeSum = b.CreateAdd(sum, negativeSumPhi);
                    Value * const idx = b.CreateAdd(writePosPhi, b.getSize(i * outputFieldsPerBlock));
                    Value * const ptr = b.CreateInBoundsGEP(sizeTy, negativeArray, idx);
                    b.CreateAlignedStore(newNegativeSum, b.CreatePointerCast(ptr, ptrVecTy), vecTyAlign);
                }
                newNegativeSum = b.mvmd_srli(sizeWidth, newNegativeSum, outputFieldsPerBlock - 1);
                for (unsigned j = 1; j < outputFieldsPerBlock; j *= 2) {
                    Value * const shifted = b.mvmd_slli(sizeWidth, newNegativeSum, j);
                    newNegativeSum = b.CreateOr(newNegativeSum, shifted);
                }
                negativeSumPhi->addIncoming(newNegativeSum, popCountLoop);
            }

        } else { // sizeWidth < inputWidth

            assert ((inputWidth % sizeWidth) == 0);

//            SmallVector<Value *, 16> partialSumArray(sizeTypePerInputField, nullptr);
//            partialSumArray[0] = partialSum;

//            for (unsigned i = 1; i < sizeTypePerInputField; i *= 2) {
//                const auto fw = inputWidth * i;
//                assert ((blockWidth % fw) == 0);
//                const auto fieldCount = blockWidth / fw;

//                b.hsimd_packl(fw, )


//            }

            llvm::report_fatal_error("Not supported yet");

        }




        Value * const nextPos = b.CreateAdd(writePosPhi, sz_STEPS);
        writePosPhi->addIncoming(nextPos, popCountLoop);

        Value * const nextIndex = b.CreateAdd(indexPhi, sz_ONE);
        indexPhi->addIncoming(nextIndex, popCountLoop);

        b.CreateCondBr(b.CreateICmpNE(nextIndex, totalNumOfStrides), popCountLoop, popCountExit);

        // ----------------------------------------------------------------------------------------
        // process any remaining partial vectors
        // ----------------------------------------------------------------------------------------

        b.SetInsertPoint(popCountExit);
        PHINode * const finalPosPhi = b.CreatePHI(sizeTy, 2);
        finalPosPhi->addIncoming(numProcessed, entry);
        finalPosPhi->addIncoming(nextPos, popCountLoop);
        PHINode * const finalBlockOffsetPhi = b.CreatePHI(sizeTy, 2);
        finalBlockOffsetPhi->addIncoming(startBlockOffset, entry);
        finalBlockOffsetPhi->addIncoming(sz_ZERO, popCountLoop);

        PHINode * finalPositiveSumPhi = nullptr;
        if (positiveArray) {
            finalPositiveSumPhi = b.CreatePHI(sizeVecTy, 2);
            finalPositiveSumPhi->addIncoming(posCount, entry);
            finalPositiveSumPhi->addIncoming(newPositiveSum, popCountLoop);
        }
        PHINode * finalNegativeSumPhi = nullptr;
        if (negativeArray) {
            finalNegativeSumPhi = b.CreatePHI(sizeVecTy, 2);
            finalNegativeSumPhi->addIncoming(negCount, entry);
            finalNegativeSumPhi->addIncoming(newNegativeSum, popCountLoop);
        }

        if (positiveArray) {
            positivePartialSum = b.mvmd_extract(sizeWidth, finalPositiveSumPhi, outputFieldsPerBlock - 1);
        }
        if (negativeArray) {
            negativePartialSum = b.mvmd_extract(sizeWidth, finalNegativeSumPhi, outputFieldsPerBlock - 1);
        }

        Value * finalStartPos = finalPosPhi;

        Value * finalBlockOffset = finalBlockOffsetPhi;

        generateIterativePopCountCode(finalStartPos, totalNumOfStrides, finalBlockOffset, false,
                                      positivePartialSum, negativePartialSum);

    } else { // inputWidth >= blockWidth

        // ----------------------------------------------------------------------------------------
        // process all full vector width blocks
        // ----------------------------------------------------------------------------------------

        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const popCountLoop = b.CreateBasicBlock("Loop");
        BasicBlock * const popCountExit = b.CreateBasicBlock("Exit");
        b.CreateBr(popCountLoop);

        b.SetInsertPoint(popCountLoop);
        PHINode * const index = b.CreatePHI(sizeTy, 2);
        index->addIncoming(sz_ZERO, entry);
        PHINode * positiveSumPhi = nullptr;
        if (positiveArray) {
            positiveSumPhi = b.CreatePHI(sizeTy, 2);
            positiveSumPhi->addIncoming(initialPositiveCount, entry);
        }
        PHINode * negativeSumPhi = nullptr;
        if (negativeArray) {
            negativeSumPhi = b.CreatePHI(sizeTy, 2);
            negativeSumPhi->addIncoming(initialNegativeCount, entry);
        }
        assert (inputWidth >= blockWidth && inputWidth % blockWidth == 0);
        const auto step = (inputWidth / blockWidth);

        Constant * const STEP = b.getSize(step);
        Value * const baseIndex = b.CreateMul(index, STEP);

        Value * partialSumVector = nullptr;
        // half adder will require the same number of pop count steps when n < 4
        if (step < 4) {
            for (unsigned i = 0; i < step; ++i) {
                Constant * const I = b.getSize(i);
                Value * const idx = b.CreateAdd(baseIndex, I);
                Value * value = b.loadInputStreamBlock(INPUT, sz_ZERO, idx);
                if (LLVM_UNLIKELY(positiveSumPhi == nullptr)) { // only negative count
                    value = b.CreateNot(value);
                }
                Value * const count = b.simd_popcount(sizeWidth, value);
                if (i == 0) {
                    partialSumVector = count;
                } else {
                    partialSumVector = b.CreateAdd(partialSumVector, count);
                }
            }
        } else {
            const auto m = floor_log2(step + 1) + 1;
            SmallVector<Value *, 64> adders(m);
            // load the first block
            Value * value = b.loadInputStreamBlock(INPUT, sz_ZERO, baseIndex);
            if (LLVM_UNLIKELY(positiveSumPhi == nullptr)) { // only negative count
                value = b.CreateNot(value);
            }
            adders[0] = value;

            // load and half-add the subsequent blocks
            for (unsigned i = 1; i < step; ++i) {
                Constant * const I = b.getSize(i);
                Value * const idx = b.CreateAdd(baseIndex, I);
                Value * value = b.loadInputStreamBlock(INPUT, sz_ZERO, idx);
                if (LLVM_UNLIKELY(positiveSumPhi == nullptr)) { // only negative count
                    value = b.CreateNot(value);
                }
                const auto k = floor_log2(i);
                for (unsigned j = 0; j <= k; ++j) {
                    Value * const sum_in = adders[j]; assert (sum_in);
                    Value * const sum_out = b.simd_xor(sum_in, value);
                    Value * const carry_out = b.simd_and(sum_in, value);
                    adders[j] = sum_out;
                    value = carry_out;
                }
                const auto l = floor_log2(i + 1);
                if (k < l) {
                    adders[l] = value;
                }
            }
            // sum the half adders
            for (unsigned i = 0; i < m; ++i) {
                Value * const count = b.simd_popcount(sizeWidth, adders[i]);
                if (i == 0) {
                    partialSumVector = count;
                } else {
                    partialSumVector = b.CreateAdd(partialSumVector, b.CreateShl(count, i));
                }
            }
        }

        const auto field = blockWidth / sizeWidth;
        assert (blockWidth % sizeWidth == 0);
        Value * const partialSum = b.hsimd_partial_sum(sizeWidth, partialSumVector);
        Value * const sum = b.mvmd_extract(sizeWidth, partialSum, field - 1);

        if (positiveArray) {
            positivePartialSum = b.CreateAdd(positiveSumPhi, sum);
            positiveSumPhi->addIncoming(positivePartialSum, popCountLoop);
            Value * const ptr = b.CreateInBoundsGEP(sizeTy, positiveArray, index);
            b.CreateAlignedStore(positivePartialSum, ptr, sizeTyAlign);
        }

        if (negativeArray) {
            Value * negSum = sum;
            if (positiveArray) {
                Constant * const INPUT_WIDTH = b.getSize(inputWidth);
                negSum = b.CreateSub(INPUT_WIDTH, sum);
            }
            negativePartialSum = b.CreateAdd(negativeSumPhi, negSum);
            negativeSumPhi->addIncoming(negativePartialSum, popCountLoop);
            Value * const ptr = b.CreateInBoundsGEP(sizeTy, negativeArray, index);
            b.CreateAlignedStore(negativePartialSum, ptr, sizeTyAlign);
        }


        BasicBlock * const popCountLoopEnd = b.GetInsertBlock();
        Value * const nextIndex = b.CreateAdd(index, sz_ONE);
        index->addIncoming(nextIndex, popCountLoopEnd);
        Value * const done = b.CreateICmpNE(nextIndex, numOfStrides);
        b.CreateCondBr(done, popCountLoop, popCountExit);

        b.SetInsertPoint(popCountExit);
    }

    #ifndef USE_LOOKBEHIND_FOR_LAST_VALUE
    if (LLVM_LIKELY(mType == PopCountType::POSITIVE || mType == PopCountType::NEGATIVE)) {
        Value * count = positivePartialSum;
        if (LLVM_UNLIKELY(mType == PopCountType::NEGATIVE)) {
            count = negativePartialSum;
        }
        assert (count);
        b.setScalarField("count", count);
    } else {
        b.setScalarField("posCount", positivePartialSum);
        b.setScalarField("negCount", negativePartialSum);
    }
    #endif

    #if !defined(PRINT_POP_COUNTS_TO_STDERR) && !defined(PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY)
    if (codegen::DebugOptionIsSet(codegen::EnablePipelineAsserts)) {
    #endif
        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const checkLoop = b.CreateBasicBlock();
        BasicBlock * const checkExit = b.CreateBasicBlock();
        b.CreateBr(checkLoop);

        b.SetInsertPoint(checkLoop);
        PHINode * idxPhi = b.CreatePHI(sizeTy, 2);
        idxPhi->addIncoming(sz_ZERO, entry);
        PHINode * priorPos = nullptr;
        if (positiveArray) {
            priorPos = b.CreatePHI(sizeTy, 2);
            priorPos->addIncoming(initialPositiveCount, entry);
        }
        PHINode * priorNeg = nullptr;
        if (negativeArray) {
            priorNeg = b.CreatePHI(sizeTy, 2);
            priorNeg->addIncoming(initialNegativeCount, entry);
        }
        Value * lastPosVal = nullptr;
        if (positiveArray) {
            Value * ptr = b.CreateGEP(sizeTy, positiveArray, idxPhi);
            Value * val = b.CreateAlignedLoad(sizeTy, ptr, sizeTyAlign);
            lastPosVal = val;
            #ifdef PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY
            debugPrint("> pos[%" PRIu64 "] = %" PRIu64, b.CreateAdd(position, idx), val);
            #elif defined(PRINT_POP_COUNTS_TO_STDERR)
            debugPrint("> pos[%" PRIu64 "] = %" PRIu64 " (0x%" PRIx64 ")", b.CreateAdd(position, idxPhi), val, ptr);
            #endif
            b.CreateAssert(b.CreateICmpULE(priorPos, val),
                           "Prior positive popcount value %" PRIu64 " is not less than current %" PRIu64 " at index %" PRIu64,
                           priorPos, val, idxPhi);
            priorPos->addIncoming(val, checkLoop);
        }
        Value * lastNegVal = nullptr;
        if (negativeArray) {
            Value * ptr = b.CreateGEP(sizeTy, negativeArray, idxPhi);
            Value * val = b.CreateAlignedLoad(sizeTy, ptr, sizeTyAlign);
            lastNegVal = val;
            #ifdef PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY
            debugPrint("> neg[%" PRIu64 "] = %" PRIu64, b.CreateAdd(position, idx), val);
            #elif defined(PRINT_POP_COUNTS_TO_STDERR)
            debugPrint("> neg[%" PRIu64 "] = %" PRIu64 " (0x%" PRIx64 ")", b.CreateAdd(position, idxPhi), val, ptr);
            #endif
            b.CreateAssert(b.CreateICmpULE(priorNeg, val),
                           "Prior negative popcount value %" PRIu64 " is not less than current %" PRIu64 " at index %" PRIu64,
                           priorNeg, val, idxPhi);
            priorNeg->addIncoming(val, checkLoop);
        }
        Value * const nextIdx = b.CreateAdd(idxPhi, sz_ONE);
        idxPhi->addIncoming(nextIdx, checkLoop);

        b.CreateCondBr(b.CreateICmpULT(nextIdx, numOfStrides), checkLoop, checkExit);

        b.SetInsertPoint(checkExit);
        if (positiveArray) {
            b.CreateAssert(b.CreateICmpEQ(lastPosVal, positivePartialSum),
                           "Last positive popcount value %" PRIu64 " should be %" PRIu64 " at index %" PRIu64,
                           lastPosVal, positivePartialSum, idxPhi);
        }
        if (negativeArray) {
            b.CreateAssert(b.CreateICmpEQ(lastNegVal, negativePartialSum),
                           "Last negative popcount value %" PRIu64 " should be %" PRIu64 " at index %" PRIu64,
                           lastNegVal, negativePartialSum, idxPhi);
        }

    #if !defined(PRINT_POP_COUNTS_TO_STDERR) && !defined(PRINT_POP_COUNTS_TO_STDERR_NO_ADDRESS_DISPLAY)
    }
    #endif
}


#ifdef USE_LOOKBEHIND_FOR_LAST_VALUE
#define LOOK_BEHIND_ATTR , LookBehind(1)
#else
#define LOOK_BEHIND_ATTR
#endif

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
PopCountKernel::PopCountKernel(LLVMTypeSystemInterface & ts, const PopCountType type, const unsigned stepFactor, StreamSet * input, StreamSet * const output)
: MultiBlockKernel(ts, TypeId::PopCountKernel, "PopCount" + std::string{type == PopCountType::POSITIVE ? "P" : "N"} + std::to_string(stepFactor)
// input streams
,{Binding{INPUT, input, FixedRate(stepFactor) }}
// output stream
,{Binding{OUTPUT_STREAM, output, FixedRate() LOOK_BEHIND_ATTR }}
// unnused I/O scalars
,{} ,{},
// internal scalar
{})
, mType(type) {
    // a block of input becomes a single integer of output
    setStride(1);
    assert (type != PopCountType::BOTH);
    #ifndef USE_LOOKBEHIND_FOR_LAST_VALUE
    addInternalScalar(ts.getSizeTy(), "count");
    #endif
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
PopCountKernel::PopCountKernel(LLVMTypeSystemInterface & ts, const PopCountType type, const unsigned stepFactor, StreamSet * input, StreamSet * const positive, StreamSet * const negative)
: MultiBlockKernel(ts, TypeId::PopCountKernel, ".PopCountB" + std::to_string(stepFactor)
// input streams
,{Binding{INPUT, input, FixedRate(stepFactor) }}
// output stream
,{Binding{POSITIVE_STREAM, positive, FixedRate() LOOK_BEHIND_ATTR }
 ,Binding{NEGATIVE_STREAM, negative, FixedRate() LOOK_BEHIND_ATTR }}
// unnused I/O scalars
,{} ,{},
// internal scalar
{})
, mType(type) {
    // a block of input becomes a single integer of output
    setStride(1);
    assert (type == PopCountType::BOTH);
    #ifndef USE_LOOKBEHIND_FOR_LAST_VALUE
    addInternalScalar(ts.getSizeTy(), "posCount");
    addInternalScalar(ts.getSizeTy(), "negCount");
    #endif
}

}

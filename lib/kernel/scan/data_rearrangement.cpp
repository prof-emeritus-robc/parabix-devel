/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/scan/data_rearrangement.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kernel {

const unsigned SCAN_WORD_BITS = 64;

static std::string rearrangementName(unsigned numFields, const std::vector<unsigned> & fieldOrder,
                                     unsigned delimiterLength, StreamSet * Data, unsigned strideBlocks) {
    std::string tmp;
    raw_string_ostream nm(tmp);
    nm << "DataRearrangement_" << numFields << "_";
    for (unsigned i = 0; i < fieldOrder.size(); i++) {
        if (i > 0) nm << ".";
        nm << fieldOrder[i];
    }
    if (delimiterLength != 1) {
        nm << "_d" << delimiterLength;
    }
    nm << "_" << Data->shapeString() << "_" << strideBlocks;
    return nm.str();
}

DataRearrangementKernel::DataRearrangementKernel(LLVMTypeSystemInterface & ts,
                                                 unsigned numFields, const std::vector<unsigned> & fieldOrder,
                                                 StreamSet * const Separators, StreamSet * const Data,
                                                 StreamSet * const Rearranged, unsigned delimiterLength,
                                                 unsigned strideBlocks)
: MultiBlockKernel(ts, rearrangementName(numFields, fieldOrder, delimiterLength, Data, strideBlocks),
// inputs
{Binding{"separators", Separators},
 Binding{"data", Data, FixedRate(), Deferred()}},
// outputs: as fields are not repeated, the output is never longer than the input.  The
// pipeline provides output space for the input held back by the deferred data input.
{Binding{"rearranged", Rearranged, BoundedRate(0, 1)}},
{}, {},
// kernel state: the number of separators seen in the current record, and their positions.
{InternalScalar{ts.getSizeTy(), "fieldNo"},
 InternalScalar{ArrayType::get(ts.getSizeTy(), numFields), "sepPos"}})
, mNumFields(numFields)
, mFieldOrder(fieldOrder)
, mDelimiterLength(delimiterLength)
, mStreamCount(Data->getNumElements())
, mFieldWidth(Data->getFieldWidth()) {
    if (LLVM_UNLIKELY(numFields == 0 || fieldOrder.empty() || fieldOrder.size() > numFields)) {
        report_fatal_error("DataRearrangementKernel: the field order must have 1 to N entries");
    }
    std::vector<bool> seen(numFields, false);
    for (const auto f : fieldOrder) {
        if (LLVM_UNLIKELY(f == 0 || f > numFields)) {
            report_fatal_error("DataRearrangementKernel: field numbers must be in 1..N");
        }
        if (LLVM_UNLIKELY(seen[f - 1])) {
            report_fatal_error("DataRearrangementKernel: a field may be selected at most once");
        }
        seen[f - 1] = true;
    }
    if (LLVM_UNLIKELY(delimiterLength == 0)) {
        report_fatal_error("DataRearrangementKernel: the delimiter length must be at least 1");
    }
    if (LLVM_UNLIKELY(mFieldWidth < 8 || (mFieldWidth & (mFieldWidth - 1)) != 0)) {
        report_fatal_error("DataRearrangementKernel: data field width must be a power of 2, at least 8");
    }
    if (LLVM_UNLIKELY(Separators->getNumElements() != 1 || Separators->getFieldWidth() != 1)) {
        report_fatal_error("DataRearrangementKernel: separators must be a single bit stream");
    }
    if (LLVM_UNLIKELY(Rearranged->getNumElements() != mStreamCount || Rearranged->getFieldWidth() != mFieldWidth)) {
        report_fatal_error("DataRearrangementKernel: output shape must match the data shape");
    }
    // The stride is limited so that the mask of nonzero scan words is a single 64-bit value.
    setStride(std::min(ts.getBitBlockWidth() * strideBlocks, SCAN_WORD_BITS * SCAN_WORD_BITS));
}

void DataRearrangementKernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    const unsigned BW = b.getBitBlockWidth();
    assert ((mStride % BW) == 0);
    const unsigned blocksPerStride = mStride / BW;
    const unsigned wordsPerBlock = BW / SCAN_WORD_BITS;
    IntegerType * const sizeTy = b.getSizeTy();
    assert (sizeTy->getBitWidth() == SCAN_WORD_BITS);
    IntegerType * const itemTy = b.getIntNTy(mFieldWidth);
    Constant * const ZERO = b.getSize(0);
    Constant * const ONE = b.getSize(1);
    Constant * const SZ_BW = b.getSize(BW);
    Constant * const SZ_WORD_BITS = b.getSize(SCAN_WORD_BITS);
    Constant * const SZ_STRIDE = b.getSize(mStride);
    Constant * const SZ_BLOCKS_PER_STRIDE = b.getSize(blocksPerStride);
    Constant * const ITEM_BYTES = b.getSize(mFieldWidth / 8);
    Constant * const NUM_FIELDS = b.getSize(mNumFields);
    Constant * const DELIM_LEN = b.getSize(mDelimiterLength);

    const auto sepArray = b.getScalarFieldPtr("sepPos");
    auto sepPosPtr = [&](Value * k) {
        return b.CreateInBoundsGEP(sepArray.second, sepArray.first, {b.getInt32(0), k});
    };
    auto loadSepPos = [&](unsigned k) {
        return b.CreateLoad(sizeTy, sepPosPtr(b.getSize(k)));
    };

    //  The state of the scan is kept in variables, promoted to registers by the optimizer.
    Value * const fieldNoVar = b.CreateAllocaAtEntryPoint(sizeTy);
    Value * const recordStartVar = b.CreateAllocaAtEntryPoint(sizeTy);
    Value * const producedVar = b.CreateAllocaAtEntryPoint(sizeTy);
    b.CreateStore(b.getScalarField("fieldNo"), fieldNoVar);
    b.CreateStore(b.getProcessedItemCount("data"), recordStartVar);
    b.CreateStore(b.getProducedItemCount("rearranged"), producedVar);

    Value * const initialPos = b.getProcessedItemCount("separators");
    Value * const avail = b.getAvailableItemCount("data");



    //  Copy len items at position src of the data to position dst of the output.  Each
    //  memcpy is a run of items within one block of both the input and the output, so
    //  that it is contiguous for both, whether a streamset has one stream or several
    //  block-interleaved streams, and whether or not its buffer wraps around.
    auto copyItems = [&](Value * const src, Value * const dst, Value * const len) {
        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const copyLoop = b.CreateBasicBlock("copyLoop");
        BasicBlock * const copyDone = b.CreateBasicBlock("copyDone");
        b.CreateCondBr(b.CreateICmpNE(len, ZERO), copyLoop, copyDone);
        b.SetInsertPoint(copyLoop);
        PHINode * const srcPhi = b.CreatePHI(sizeTy, 2);
        srcPhi->addIncoming(src, entry);
        PHINode * const dstPhi = b.CreatePHI(sizeTy, 2);
        dstPhi->addIncoming(dst, entry);
        PHINode * const remPhi = b.CreatePHI(sizeTy, 2);
        remPhi->addIncoming(len, entry);
        Value * const srcRoom = b.CreateSub(SZ_BW, b.CreateURem(srcPhi, SZ_BW));
        Value * const dstRoom = b.CreateSub(SZ_BW, b.CreateURem(dstPhi, SZ_BW));
        Value * const chunk = b.CreateUMin(remPhi, b.CreateUMin(srcRoom, dstRoom));
        Value * const chunkBytes = b.CreateMul(chunk, ITEM_BYTES);
        for (unsigned s = 0; s < mStreamCount; s++) {
            Value * const from = b.getRawInputPointer("data", b.getInt32(s), srcPhi);
            Value * const to = b.getRawOutputPointer("rearranged", b.getInt32(s), dstPhi);
            b.CreateMemCpy(to, from, chunkBytes, 1);
        }
        Value * const rem = b.CreateSub(remPhi, chunk);
        srcPhi->addIncoming(b.CreateAdd(srcPhi, chunk), copyLoop);
        dstPhi->addIncoming(b.CreateAdd(dstPhi, chunk), copyLoop);
        remPhi->addIncoming(rem, copyLoop);
        b.CreateCondBr(b.CreateICmpNE(rem, ZERO), copyLoop, copyDone);
        b.SetInsertPoint(copyDone);
    };

    auto copyItem = [&](Value * const src, Value * const dst) {
        for (unsigned s = 0; s < mStreamCount; s++) {
            Value * const item = b.CreateLoad(itemTy, b.getRawInputPointer("data", b.getInt32(s), src));
            b.CreateStore(item, b.getRawOutputPointer("rearranged", b.getInt32(s), dst));
        }
    };

    //  Write the output record for the current record, whose separator positions are
    //  in sepPos.  At the end of the input (atEnd), missing separators are positioned
    //  at avail: their fields are truncated to empty and their separators not written.
    auto emitRecord = [&](const bool atEnd) {
        Value * const recordStart = b.CreateLoad(sizeTy, recordStartVar);
        Value * produced = b.CreateLoad(sizeTy, producedVar);
        const unsigned outFields = mFieldOrder.size();
        for (unsigned i = 0; i < outFields; i++) {
            const unsigned k = mFieldOrder[i] - 1;
            Value * const end = loadSepPos(k);
            //  Field k > 0 follows field separator k - 1 (never the record separator).
            Value * start = (k == 0) ? recordStart : b.CreateAdd(loadSepPos(k - 1), DELIM_LEN);
            if (atEnd) {
                start = b.CreateUMin(start, end);
            }
            Value * const len = b.CreateSub(end, start);
            copyItems(start, produced, len);
            produced = b.CreateAdd(produced, len);
            const bool isRecordSep = (i == outFields - 1);
            Value * const sep = loadSepPos(isRecordSep ? (mNumFields - 1) : i);
            const unsigned sepLen = isRecordSep ? 1 : mDelimiterLength;
            auto writeSeparator = [&]() {
                if (sepLen == 1) {
                    copyItem(sep, produced);
                } else {
                    copyItems(sep, produced, b.getSize(sepLen));
                }
            };
            if (atEnd) {
                BasicBlock * const writeSep = b.CreateBasicBlock("writeSep");
                BasicBlock * const sepDone = b.CreateBasicBlock("sepDone");
                Value * const present = b.CreateICmpULT(sep, avail);
                b.CreateCondBr(present, writeSep, sepDone);
                b.SetInsertPoint(writeSep);
                writeSeparator();
                b.CreateBr(sepDone);
                b.SetInsertPoint(sepDone);
                produced = b.CreateAdd(produced, b.CreateSelect(present, b.getSize(sepLen), ZERO));
            } else {
                writeSeparator();
                produced = b.CreateAdd(produced, b.getSize(sepLen));
            }
        }
        b.CreateStore(produced, producedVar);
    };

    BasicBlock * const entryBlock = b.GetInsertBlock();
    BasicBlock * const strideLoop = b.CreateBasicBlock("strideLoop");
    BasicBlock * const sepLoop = b.CreateBasicBlock("sepLoop");
    BasicBlock * const processSep = b.CreateBasicBlock("processSep");
    BasicBlock * const fieldSep = b.CreateBasicBlock("fieldSep");
    BasicBlock * const recordSep = b.CreateBasicBlock("recordSep");
    BasicBlock * const nextSep = b.CreateBasicBlock("nextSep");
    BasicBlock * const strideDone = b.CreateBasicBlock("strideDone");
    BasicBlock * const stridesDone = b.CreateBasicBlock("stridesDone");
    BasicBlock * const checkFinalRecord = b.CreateBasicBlock("checkFinalRecord");
    BasicBlock * const finalRecord = b.CreateBasicBlock("finalRecord");
    BasicBlock * const finish = b.CreateBasicBlock("finish");

    b.CreateCondBr(b.CreateICmpNE(numOfStrides, ZERO), strideLoop, stridesDone);

    b.SetInsertPoint(strideLoop);
    PHINode * const strideNo = b.CreatePHI(sizeTy, 2);
    strideNo->addIncoming(ZERO, entryBlock);
    Value * const stridePos = b.CreateAdd(initialPos, b.CreateMul(strideNo, SZ_STRIDE));
    Value * const strideBlockOffset = b.CreateMul(strideNo, SZ_BLOCKS_PER_STRIDE);
    //  The mask of the scan words of the stride that have any separator.
    Value * sepMask = ZERO;
    for (unsigned blk = 0; blk < blocksPerStride; blk++) {
        Value * const blockIndex = b.CreateAdd(strideBlockOffset, b.getSize(blk));
        Value * const bits = b.loadInputStreamBlock("separators", ZERO, blockIndex);
        Value * const anyBits = b.simd_any(SCAN_WORD_BITS, bits);
        Value * const wordMask = b.CreateZExtOrTrunc(b.hsimd_signmask(SCAN_WORD_BITS, anyBits), sizeTy);
        sepMask = b.CreateOr(sepMask, b.CreateShl(wordMask, b.getSize(blk * wordsPerBlock)));
    }
    Value * const wordBasePtr = b.getInputStreamBlockPtr("separators", ZERO, strideBlockOffset);
    b.CreateCondBr(b.CreateICmpNE(sepMask, ZERO), sepLoop, strideDone);

    //  Precondition: the mask has at least one more separator.
    b.SetInsertPoint(sepLoop);
    PHINode * const maskPhi = b.CreatePHI(sizeTy, 2);
    maskPhi->addIncoming(sepMask, strideLoop);
    PHINode * const wordPhi = b.CreatePHI(sizeTy, 2);
    wordPhi->addIncoming(ZERO, strideLoop);
    Value * const wordIdx = b.CreateCountForwardZeroes(maskPhi, "wordIdx");
    Value * const loadedWord = b.CreateLoad(sizeTy, b.CreateGEP(sizeTy, wordBasePtr, wordIdx));
    Value * const theWord = b.CreateSelect(b.CreateICmpEQ(wordPhi, ZERO), loadedWord, wordPhi);
    Value * const wordPos = b.CreateAdd(stridePos, b.CreateMul(wordIdx, SZ_WORD_BITS));
    Value * const sepPos = b.CreateAdd(wordPos, b.CreateCountForwardZeroes(theWord), "sepPos");
    //  At the end of the input, separator bits past its end are ignored.
    b.CreateLikelyCondBr(b.CreateICmpULT(sepPos, avail), processSep, stridesDone);

    b.SetInsertPoint(processSep);
    Value * const fieldNo = b.CreateLoad(sizeTy, fieldNoVar);
    b.CreateStore(sepPos, sepPosPtr(fieldNo));
    Value * const nextFieldNo = b.CreateAdd(fieldNo, ONE);
    b.CreateCondBr(b.CreateICmpEQ(nextFieldNo, NUM_FIELDS), recordSep, fieldSep);

    b.SetInsertPoint(fieldSep);
    b.CreateStore(nextFieldNo, fieldNoVar);
    b.CreateBr(nextSep);

    b.SetInsertPoint(recordSep);
    emitRecord(false);
    b.CreateStore(ZERO, fieldNoVar);
    b.CreateStore(b.CreateAdd(sepPos, ONE), recordStartVar);
    b.CreateBr(nextSep);

    b.SetInsertPoint(nextSep);
    Value * const dropSep = b.CreateResetLowestBit(theWord, "dropSep");
    Value * const nextMask = b.CreateSelect(b.CreateICmpEQ(dropSep, ZERO), b.CreateResetLowestBit(maskPhi), maskPhi);
    maskPhi->addIncoming(nextMask, nextSep);
    wordPhi->addIncoming(dropSep, nextSep);
    b.CreateCondBr(b.CreateICmpNE(nextMask, ZERO), sepLoop, strideDone);

    b.SetInsertPoint(strideDone);
    Value * const nextStrideNo = b.CreateAdd(strideNo, ONE);
    strideNo->addIncoming(nextStrideNo, strideDone);
    b.CreateCondBr(b.CreateICmpULT(nextStrideNo, numOfStrides), strideLoop, stridesDone);

    b.SetInsertPoint(stridesDone);
    b.CreateCondBr(b.isFinal(), checkFinalRecord, finish);

    //  At the end of the input, a record with any data or separators is unterminated.
    b.SetInsertPoint(checkFinalRecord);
    Value * const finalFieldNo = b.CreateLoad(sizeTy, fieldNoVar);
    Value * const pendingData = b.CreateICmpULT(b.CreateLoad(sizeTy, recordStartVar), avail);
    Value * const unterminated = b.CreateOr(b.CreateICmpNE(finalFieldNo, ZERO), pendingData);
    b.CreateCondBr(unterminated, finalRecord, finish);

    b.SetInsertPoint(finalRecord);
    for (unsigned k = 0; k < mNumFields; k++) {
        Value * const ptr = sepPosPtr(b.getSize(k));
        Value * const missing = b.CreateICmpUGE(b.getSize(k), finalFieldNo);
        b.CreateStore(b.CreateSelect(missing, avail, b.CreateLoad(sizeTy, ptr)), ptr);
    }
    emitRecord(true);
    b.CreateStore(ZERO, fieldNoVar);
    b.CreateStore(avail, recordStartVar);
    b.CreateBr(finish);

    b.SetInsertPoint(finish);
    b.setScalarField("fieldNo", b.CreateLoad(sizeTy, fieldNoVar));
    b.setProcessedItemCount("data", b.CreateLoad(sizeTy, recordStartVar));
    b.setProducedItemCount("rearranged", b.CreateLoad(sizeTy, producedVar));
}

}

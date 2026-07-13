/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <pablo/compressed_carry_manager.h>

#include <pablo/carry_data.h>
#include <pablo/codegenstate.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Transforms/Utils/Local.h>
#include <pablo/branch.h>
#include <pablo/pablo_intrinsic.h>
#include <pablo/pe_advance.h>
#include <pablo/pe_scanthru.h>
#include <pablo/pe_matchstar.h>
#include <pablo/pe_var.h>
#include <kernel/core/kernel_builder.h>
#include <toolchain/toolchain.h>
#include <array>

enum NonCarryCollapsingMode {
    NestedCapacity = 0,
    LastIncomingCarryLoopIteration = 1,
    NestedCarryState = 2
};

#define LONG_ADVANCE_BREAKPOINT 64

using namespace llvm;

using KernelBuilder = kernel::KernelBuilder;

namespace pablo {

/* Local Helper Functions */

static inline unsigned ceil_log2(const unsigned v) {
    assert ("log2(0) is undefined!" && v != 0);
    return (sizeof(unsigned) * CHAR_BIT) - __builtin_clz(v - 1U);
}


static inline unsigned nearest_pow2(const unsigned v) {
    assert (v > 0 && v < (UINT32_MAX / 2));
    return (v < 2) ? 1 : (1U << ceil_log2(v));
}


static inline unsigned ceil_udiv(const unsigned x, const unsigned y) {
    return (((x - 1) | (y - 1)) + 1) / y;
}

static Type * toSummaryType(KernelBuilder & b, int32_t summarySize) {
    switch (summarySize) {
    case 0:
        return nullptr;
    case 8:
        return b.getInt8Ty();
    case 64:
        return b.getInt64Ty();
    default:
        assert ("unexpected summary type" && (uint32_t) summarySize == b.getBitBlockWidth());
        return b.getBitBlockType();
    }
}

inline unsigned getVectorBitWidth(const Type * const ty) {
    return cast<FixedVectorType>(ty)->getPrimitiveSizeInBits().getFixedValue();
}

inline unsigned getTypeBitWidth(const Type * const ty) {
    if (LLVM_UNLIKELY(ty == nullptr)) {
        return 0U;
    } else if (ty->isIntegerTy()) {
        return cast<IntegerType>(ty)->getBitWidth();
    } else {
        return getVectorBitWidth(ty);
    }
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeCurrentCarryOutSummary
 ** ------------------------------------------------------------------------------------------------------------- */
void CompressedCarryManager::writeCurrentCarryOutSummary(kernel::KernelBuilder & b) {
    if (LLVM_LIKELY(mCarryInfo->hasExplicitSummary())) {
        const auto n = mCarrySummaryStack.size(); assert (n > 0);
        writeCarryOutSummary(b, mCarrySummaryStack[n - 1]);
    } else if (mCarryInfo->hasImplicitSummary()) {
        const auto n = mCarrySummaryStack.size(); assert (n > 0);
        mCarrySummaryStack[n - 1] = b.CreateLoad(mCarryInfo->getSummarySizeTy(), mCurrentFrame);
    }
}

void CompressedCarryManager::combineCarryOutSummary(kernel::KernelBuilder & b, const unsigned offset) {
    if (LLVM_LIKELY(mCarryInfo->hasSummary())) {
        const auto n = mCarrySummaryStack.size(); assert (n > 0);
        // combine the outer summary with the nested summary so that when
        // we leave the scope, we'll properly phi out the value of the new
        // outer summary
        if (n > 2) {
            Value * nested = mCarrySummaryStack[n - 1];
            Value * const outer = mCarrySummaryStack[n - 2];
            Type * const nestedTy = nested->getType();
            Type * const outerTy = outer->getType();
            if (nestedTy != outerTy) {
                const auto nestedBW = getTypeBitWidth(nestedTy);
                const auto outerBW = getTypeBitWidth(outerTy);
                assert (nestedBW != outerBW);
                if (nestedBW < outerBW) {
                    assert (nestedTy->isIntegerTy());
                    nested = b.CreateZExt(nested, b.getIntNTy(outerBW));
                    if (outerTy->isVectorTy()) {
                        nested = b.CreateBitCast(nested, outerTy);
                    }
                } else {
                    Type * const intNestedTy = b.getIntNTy(nestedBW);
                    if (nestedTy->isVectorTy()) {
                        nested = b.CreateBitCast(nested, intNestedTy);
                    }
                    nested = b.CreateICmpNE(nested, Constant::getNullValue(intNestedTy));
                    assert (outerTy->isIntegerTy());
                    nested = b.CreateZExt(nested, outerTy);
                }
            }
            assert (nested->getType() == outer->getType());
            mCarrySummaryStack[n - offset] = b.CreateOr(outer, nested);
        }
    }
}

/* ===== Operations ===== */

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief shortIndexedAdvanceCarryInCarryOut
 ** ------------------------------------------------------------------------------------------------------------- */
Value * CompressedCarryManager::shortIndexedAdvanceCarryInCarryOut(kernel::KernelBuilder & b, const unsigned shiftAmount, Value * const strm, Value * const index_strm) {
    Value * carryIn = getNextCarryIn(b);
    Type * ty = carryIn->getType();
    if (ty->isVectorTy()) {
        carryIn = b.CreateBitCast(carryIn, b.getIntNTy(getTypeBitWidth(ty)));
    }
    carryIn = b.CreateBitCast(b.CreateZExt(carryIn, b.getIntNTy(b.getBitBlockWidth())), b.getBitBlockType());
    Value * carryOut, * result;
    std::tie(carryOut, result) = b.bitblock_indexed_advance(strm, index_strm, carryIn, shiftAmount);
    const auto fw = (shiftAmount < 8) ? 8U : 64U;
    carryOut = b.mvmd_extract(fw, carryOut, 0);
    setNextCarryOut(b, carryOut);
    return result;
}

/* ===== Summary Operations ===== */

Value * CompressedCarryManager::readCarryInSummary(kernel::KernelBuilder & b) const {
    assert (mCarryInfo->hasSummary());
    Value * summary = nullptr;
    if (LLVM_LIKELY(mCarryInfo->hasImplicitSummary())) {
        summary = b.CreateLoad(mCarryInfo->getSummarySizeTy(), mCurrentFrame);
    } else {
        assert (mCarryInfo->hasExplicitSummary());
        Value * ptr = nullptr;
        Constant * const ZERO = b.getInt32(0);
        FixedArray<Value *, 3> indices;
        indices[0] = ZERO;
        indices[1] = ZERO;
        indices[2] = mLoopDepth == 0 ? ZERO : mLoopSelector;
        ptr = b.CreateGEP(mCurrentFrameType, mCurrentFrame, indices);
        Type * carryTy = mCurrentFrameType->getStructElementType(0)->getArrayElementType();
        summary = b.CreateLoad(carryTy, ptr);
        if (mNestedLoopCarryInMaskPhi) {
            summary = b.CreateAnd(summary, mNestedLoopCarryInMaskPhi);
        }
    }
    return summary;
}

Type * CompressedCarryManager::getSummaryTypeFromCurrentFrame(kernel::KernelBuilder & b) const {
    return mCarryInfo->getSummarySizeTy();
}

inline static bool isCarryGeneratingStatement(const Statement * const stmt) {
    if (IntrinsicCall const * call = dyn_cast<IntrinsicCall>(stmt)) {
        return call->isCarryProducing();
    } else {
        return isa<CarryProducingStatement>(stmt);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyse
 ** ------------------------------------------------------------------------------------------------------------- */
StructType * CompressedCarryManager::analyse(kernel::KernelBuilder & b, const PabloBlock * const entryScope) {

    Type * const int8Ty = b.getInt8Ty();
    Type * const int64Ty = b.getInt64Ty();
    Type * const blockTy = b.getBitBlockType();
    const auto blockWidth = b.getBitBlockWidth();
    const auto maxNumSmallCarriesForImplicitSummary = blockWidth / 8;

    auto & DL = b.getModule()->getDataLayout();

    std::function<StructType *(const PabloBlock *, unsigned, unsigned, bool)> analyseRec = [&](
            const PabloBlock * const scope,
            const unsigned ifDepth,
            const unsigned whileDepth,
            const bool inNonCarryCollapsingLoop) {

        const auto carryScopeIndex = mCarryScopes++;

        const auto packSize = (whileDepth == 0) ? 1U : 2U;
        ArrayType * const i8PackTy = ArrayType::get(int8Ty, packSize);
        ArrayType * const i64PackTy = ArrayType::get(int64Ty, packSize);

        auto canUseImplicitSummary = false; // (whileDepth == 0);
        size_t carryStateBytes = 0;

        /* Get Carry Types */

        auto getNestedSummarySize = [&](const unsigned nestedScopeIndex) -> unsigned {
            assert (nestedScopeIndex < mCarryMetadata.size());
            return getTypeBitWidth(mCarryMetadata[nestedScopeIndex].getSummarySizeTy());
        };

        unsigned summarySize = 0;

        std::vector<Type *> state;

        for (const Statement * stmt : *scope) {
            if (LLVM_UNLIKELY(isa<Advance>(stmt) || isa<IndexedAdvance>(stmt))) {
                const auto amount = isa<Advance>(stmt)
                    ? cast<Advance>(stmt)->getAmount()
                    : cast<IndexedAdvance>(stmt)->getAmount();

                Type * type = nullptr;
                if (LLVM_LIKELY(amount < 8)) {
                    type = i8PackTy;
                    summarySize = std::max(8U, summarySize);
                    ++carryStateBytes;
                } else if (LLVM_UNLIKELY(amount < LONG_ADVANCE_BREAKPOINT)) {
                    carryStateBytes += (LONG_ADVANCE_BREAKPOINT / 8);
                    type = i64PackTy;
                    summarySize = std::max<unsigned>(LONG_ADVANCE_BREAKPOINT, summarySize);
                } else {
                    canUseImplicitSummary = false;
                    const auto numOfBlocks = ceil_udiv(amount, blockWidth);
                    const auto additionalBlocks = (isa<IndexedAdvance>(stmt) ? 1U : 0U) + ((whileDepth != 0) ? 1U : 0U);
                    type = ArrayType::get(blockTy, nearest_pow2(numOfBlocks + additionalBlocks));
                    if (LLVM_UNLIKELY(ifDepth > 0 && numOfBlocks != 1)) {
                        const auto summarySize = ceil_udiv(numOfBlocks, blockWidth);
                        // 1 bit will mark the presense of any bit in each block.
                        state.push_back(ArrayType::get(blockTy, summarySize));
                    }
                    mHasLongAdvance = true;
                    if (isa<IndexedAdvance>(stmt)) {
                        mIndexedLongAdvanceTotal++;
                    }
                    summarySize = blockWidth;
                }
                state.push_back(type);
            } else if (LLVM_UNLIKELY(isCarryGeneratingStatement(stmt))) {
                ++carryStateBytes;
                summarySize = std::max(8U, summarySize);
                state.push_back(i8PackTy);
            } else if (LLVM_UNLIKELY(isa<If>(stmt))) {
                canUseImplicitSummary = false;
                const auto nestedScopeIndex = mCarryScopes;
                state.push_back(analyseRec(cast<If>(stmt)->getBody(), ifDepth + 1, whileDepth, false));
                summarySize = std::max(getNestedSummarySize(nestedScopeIndex), summarySize);
            } else if (LLVM_UNLIKELY(isa<While>(stmt))) {
                mHasLoop = true;
                canUseImplicitSummary = false;
                const auto nestedScopeIndex = mCarryScopes;
                const PabloBlock * const nestedScope = cast<While>(stmt)->getBody();
                const auto carryCollapsingMode = cast<While>(stmt)->isRegular();
                state.push_back(analyseRec(nestedScope, ifDepth, whileDepth + 1, !carryCollapsingMode));
                summarySize = std::max(getNestedSummarySize(nestedScopeIndex), summarySize);
            }
        }

        if (carryStateBytes > maxNumSmallCarriesForImplicitSummary) {
            canUseImplicitSummary = false;
        }

        /* Construct Carry State Struct */
        CarryData & cd = mCarryMetadata[carryScopeIndex];
        StructType * carryState = nullptr;
        CarryData::SummaryKind summaryKind = CarryData::NoSummary;

        // Insert the smallest possible summary for this scope.
        Type * summaryTy = toSummaryType(b, summarySize);

        unsigned packedSizeInBits = 0;
        const auto n = state.size();
        for (unsigned i = 0; i < n; ++i) {
            packedSizeInBits += CBuilder::getTypeSize(DL, state[i]);
        }
        packedSizeInBits *= 8;

        unsigned requiredSummaryPaddingInBits = 0;

        if (LLVM_LIKELY(ifDepth != 0 || whileDepth != 0)) {
            if (LLVM_LIKELY(!isEmptyCarryStruct(state) && !inNonCarryCollapsingLoop)) {
                if (LLVM_LIKELY(canUseImplicitSummary)) {
                    summaryKind = CarryData::ImplicitSummary;
                    // Insert padding if necessary to ensure the implicit summary does
                    // not contain bits of a subsequent frame.
                    if (packedSizeInBits <= 8) {
                        summarySize = 8;
                    } else if (packedSizeInBits <= 64) {
                        summarySize = 64;
                    } else {
                        summarySize = blockWidth;
                    }
                    assert (summarySize >= packedSizeInBits);
                    requiredSummaryPaddingInBits = summarySize - packedSizeInBits;
                    summaryTy = toSummaryType(b, summarySize);
                } else {
                    summaryKind = CarryData::ExplicitSummary;
                    state.insert(state.begin(), ArrayType::get(summaryTy, packSize));
                }
            }
        }

        unsigned alignmentPadding = 0;

        if (summarySize) {
            const auto unpaddedOffsetInBits = ((requiredSummaryPaddingInBits + packedSizeInBits) % summarySize);
            alignmentPadding = ((summarySize - unpaddedOffsetInBits) % summarySize);
        }

        const auto paddingInBits = requiredSummaryPaddingInBits + alignmentPadding;
        if (paddingInBits) {
            assert ((paddingInBits % 8) == 0);
            state.push_back(ArrayType::get(int8Ty, paddingInBits / 8));
        }

        carryState = StructType::get(b.getContext(), state);

        if (LLVM_UNLIKELY(inNonCarryCollapsingLoop && state.size() > 0)) {
            mHasNonCarryCollapsingLoops = true;
            cd.setNestedCarryStateType(carryState);
            summaryKind = (CarryData::SummaryKind)(CarryData::ExplicitSummary | CarryData::NonCarryCollapsingMode);
            FixedArray<Type *, 3> fields;
            fields[NestedCapacity] = b.getSizeTy();
            fields[LastIncomingCarryLoopIteration] = b.getSizeTy();
            fields[NestedCarryState] = PointerType::getUnqual(b.getContext());
            carryState = StructType::get(b.getContext(), fields);
        }
        cd.setSummarySizeTy(summaryTy);
        cd.setSummaryKind(summaryKind);
        return carryState;
    };

    return analyseRec(entryScope, 0, 0, false);
}

CompressedCarryManager::CompressedCarryManager() noexcept
: CarryManager()
{}

} // namespace pablo

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

namespace llvm {
    class Type;
    class StructType;
}

namespace pablo {

class CarryData {
public:

    enum SummaryKind {
        NoSummary = 0
        , ImplicitSummary = 1
        , BorrowedSummary = 2
        , ExplicitSummary = 3
        , NonCarryCollapsingMode = 4
    };
             
    bool hasSummary() const {
        return (mSummaryKind & (ImplicitSummary | BorrowedSummary | ExplicitSummary)) != NoSummary;
    }
    
    bool hasImplicitSummary() const {
        return (mSummaryKind & (ImplicitSummary | BorrowedSummary | ExplicitSummary)) == ImplicitSummary;
    }

    bool hasBorrowedSummary() const {
        return (mSummaryKind & (ImplicitSummary | BorrowedSummary | ExplicitSummary)) == BorrowedSummary;
    }

    bool hasExplicitSummary() const {
        return (mSummaryKind & (ImplicitSummary | BorrowedSummary | ExplicitSummary)) == ExplicitSummary;
    }

    bool nonCarryCollapsingMode() const {
        return (mSummaryKind & (NonCarryCollapsingMode)) != 0;
    }

    void setSummaryKind(const SummaryKind value) {
        mSummaryKind = value;
    }

    llvm::Type * getSummarySizeTy() const {
        return mSummarySize;
    }

    void setSummarySizeTy(llvm::Type * summarySize) {
        mSummarySize = summarySize;
    }

    void setNestedCarryStateType(llvm::StructType * stateType) {
        mNestedCarryStateType = stateType;
    }

    llvm::StructType * getNestedCarryStateType() const {
        return mNestedCarryStateType;
    }
    
private:

    SummaryKind         mSummaryKind = NoSummary;
    llvm::Type *        mSummarySize = nullptr;
    llvm::StructType *  mNestedCarryStateType = nullptr;
};


}



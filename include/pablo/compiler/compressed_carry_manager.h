/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include "carry_manager.h"

namespace pablo {

class CompressedCarryManager final : public CarryManager {

public:

    CompressedCarryManager() noexcept;

protected:

    llvm::StructType * analyse(kernel::KernelBuilder & b, const PabloBlock * const scope) override;

    llvm::Value * shortIndexedAdvanceCarryInCarryOut(kernel::KernelBuilder & b, const unsigned shiftAmount, llvm::Value * const strm, llvm::Value * const index_strm) override;

    llvm::Value * readCarryInSummary(kernel::KernelBuilder & b) const override;

    void writeCurrentCarryOutSummary(kernel::KernelBuilder & b) override;

    void combineCarryOutSummary(kernel::KernelBuilder & b, const unsigned offset) override;

    llvm::Type * getSummaryTypeFromCurrentFrame(kernel::KernelBuilder & b) const override;

};

}


#pragma once

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#include <idisa/idisa_builder.h>

namespace IDISA {

constexpr unsigned I64_width = 64;

class IDISA_I64_Builder : public virtual IDISA_Builder {
public:
    static const unsigned NativeBitBlockWidth = I64_width;
  
    IDISA_I64_Builder(llvm::LLVMContext & C, const FeatureSet & featureSet, unsigned bitBlockWidth, unsigned laneWidth)
    : llvm::IRBuilder<>(C)
    , IDISA_Builder(C, featureSet, I64_width, bitBlockWidth, laneWidth) {

    } 

    virtual std::string getBuilderUniqueName() override;

    ~IDISA_I64_Builder() {}

};

}


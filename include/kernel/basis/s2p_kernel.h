/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <re/alphabet/alphabet.h>
#include <pablo/pablo_kernel.h>
#include <kernel/pipeline/driver/driver.h>
#include <string>

namespace IDISA { class IDISA_Builder; }  // lines 14-14
namespace llvm { class Value; }

namespace kernel {

    
class S2PKernel final : public MultiBlockKernel {
public:
    S2PKernel(LLVMTypeSystemInterface & ts,
              StreamSet * const codeUnitStream,
              StreamSet * const BasisBits,
              StreamSet * zeroMask = nullptr);
protected:
    std::string makeCacheName(StreamSet * const BasisBits, StreamSet * zeroMask);
    Bindings makeInputBindings(StreamSet * codeUnitStream, StreamSet * zeroMask);
    Bindings makeOutputBindings(StreamSet * const BasisBits);
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
private:
    bool mZeroMask;
};

// Equivalent to S2P, but split into stages for better balancing
// with multiple cores.
void Staged_S2P(PipelineBuilder & P,
                StreamSet * codeUnitStream, StreamSet * BasisBits,
                bool completionFromQuads = false);

//
// Selected S2P algorithm based on command-line parameters:
// SplitTransposition, PabloTransposition
void Selected_S2P(PipelineBuilder & P, StreamSet * codeUnitStream, StreamSet * BasisBits);

class S2P_i21_3xi8 final : public MultiBlockKernel {
public:
    S2P_i21_3xi8(LLVMTypeSystemInterface & ts, StreamSet * const i32Stream, StreamSet * const i8stream0, StreamSet * const i8stream1, StreamSet * const i8stream2);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

class S2P_3xi8_21xi1 final : public MultiBlockKernel {
public:
    S2P_3xi8_21xi1(LLVMTypeSystemInterface & ts, StreamSet * const i8stream0, StreamSet * const i8stream1, StreamSet * const i8stream2, StreamSet * const BasisBits);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

class S2P_21Kernel final : public MultiBlockKernel {
public:
    S2P_21Kernel(LLVMTypeSystemInterface & ts, StreamSet * const codeUnitStream, StreamSet * const BasisBits);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

class S2P_PabloKernel final : public pablo::PabloKernel {
public:
    S2P_PabloKernel(LLVMTypeSystemInterface & ts, StreamSet * const codeUnitStream, StreamSet * const BasisBits);
protected:
    void generatePabloMethod() override;
private:
    const unsigned          mCodeUnitWidth;
};


}

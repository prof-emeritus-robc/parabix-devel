/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/core/kernel.h>
#include <pablo/pablo.h>
namespace kernel { class PipelineBuilder; }

namespace kernel {
//
// Convert a byte stream of hexadecimal values into a bit stream.
// Each hexadecimal input byte generates 4 bits to the output stream.
//

class HexToBinary final : public kernel::BlockOrientedKernel {
public:
    HexToBinary(LLVMTypeSystemInterface & ts, StreamSet * hexStream, StreamSet * binStream);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
};

//
// Produce a hexadecimal output stream with one hexadecimal byte
// for each 4 bits of an input bit stream.
//

class BinaryToHex final : public kernel::BlockOrientedKernel {
public:
    BinaryToHex(LLVMTypeSystemInterface & ts, StreamSet * binStream, StreamSet * hexStream);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
    void generateFinalBlockMethod(KernelBuilder & b, llvm::Value * const remainingBits) override;
};

//
//  Given an Mx1 input stream bixData and an Nx1 output variable bixPairData,
//  where M > N and M <= 2*N, write the bits of bixData to bixPairData such that:
//  bixPairData[i] at pos p = bixData[N + i] at position p/2, if p is even and i < M
//                          = bixData[i] at position (p - 1)/2, if p is odd
//                          = 0 otherwise
//

class BixSplit final : public kernel::BlockOrientedKernel {
public:
    BixSplit(LLVMTypeSystemInterface & ts, StreamSet * bixData, StreamSet * bixPairData);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
private:
    unsigned mInputBits;
    unsigned mOutputBits;
};

//
//  Given a 4 x i1 input stream, generate a corresponding 8 x i1 ASCII output
//  stream consisting of the hex representation of the input.
//
class Bix4toHex final : public pablo::PabloKernel {
public:
    Bix4toHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * hexBasis, bool LowerHex = false);
protected:
    void generatePabloMethod() override;
private:
    bool mLowerHex;
};

void UnicodeHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void JavaHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void CHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void XMLHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

}

/*
 * Part of the Parabix Project, under the Open Software License 3.0.
 * SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/core/kernel.h>
namespace kernel { class Scalar; }
namespace kernel { class StreamSet; }
namespace kernel { class PipelineBuilder; }

namespace kernel {

/**
 * Kernel driving `AssertEQ` and `AssertNE` functions.
 * 
* Should NOT be used directly. Use either `AssertEQ` or `AssertNE` instead.
 */
class StreamEquivalenceKernel : public MultiBlockKernel {
public:
    enum class Mode { EQ, NE };

    StreamEquivalenceKernel(LLVMTypeSystemInterface & ts, Mode mode, StreamSet * x, StreamSet * y, Scalar * outPtr);
    void generateInitializeMethod(KernelBuilder & b) override;
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
    void generateFinalizeMethod(KernelBuilder & b) override;
private:
    const Mode mMode;
};

}

namespace testing {

/**
 * Compares two `StreamsSets` to see if they are equal setting the test case's 
 * state to `failing` if they are not.
 * 
 * Preconditions:
 *  - `lhs` and `rhs` must have the same field width
 *  - `lhs` and `rhs` must have the same number of elements
 */
void AssertEQ(kernel::PipelineBuilder & P, kernel::StreamSet * lhs, kernel::StreamSet * rhs);

/**
 * Compares two `StreamsSets` to see if they are equal setting the test case's 
 * state to `failing` if they are.
 * 
 * Preconditions:
 *  - `lhs` and `rhs` must have the same field width
 *  - `lhs` and `rhs` must have the same number of elements
 */
void AssertNE(kernel::PipelineBuilder & P, kernel::StreamSet * lhs, kernel::StreamSet * rhs);

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/core/kernel.h>

namespace IDISA { class IDISA_Builder; }

namespace kernel {

class StdOutKernel final : public SegmentOrientedKernel {
public:
    StdOutKernel(LLVMTypeSystemInterface & ts, StreamSet * codeUnitBuffer);
private:
    void linkExternalMethods(KernelBuilder & b) override;
    void generateDoSegmentMethod(KernelBuilder & b) override;
private:
    const unsigned mCodeUnitWidth;

};

// Writes its input to the file descriptor fd (a uint32_t scalar) with
// toolio_write, which repeats short writes and records the first error for
// toolio::finishOutput to report.  One compiled kernel serves the standard
// output and any file.
class FDSinkKernel final : public SegmentOrientedKernel {
public:
    FDSinkKernel(LLVMTypeSystemInterface & ts, Scalar * fd, StreamSet * codeUnitBuffer);
private:
    void linkExternalMethods(KernelBuilder & b) override;
    void generateDoSegmentMethod(KernelBuilder & b) override;
private:
    const unsigned mCodeUnitWidth;
};

class FileSink final : public SegmentOrientedKernel {
public:
    FileSink(LLVMTypeSystemInterface & ts, Scalar * outputFileName, StreamSet * codeUnitBuffer);
protected:
    void linkExternalMethods(KernelBuilder & b) override;
    void generateInitializeMethod(KernelBuilder & b) override;
    void generateDoSegmentMethod(KernelBuilder & b) override;
    void generateFinalizeMethod(KernelBuilder & b) override;
private:
    const unsigned mCodeUnitWidth;

};
}




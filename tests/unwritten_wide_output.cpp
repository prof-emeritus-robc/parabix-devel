/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// Test driver for the clearing of unwritten output data of wide (multi-bit item) streams.
//
// A producer computes N byte streams from the input bytes (stream s holds each byte plus 1 + s), so
// that its final block writes nonzero data past the end of input.  The pipeline must clear that data
// when the producer terminates.  A consumer combines each pack with the next one (its lookahead of
// 64 items covers one pack of byte items for block widths up to 512), so its output near the end of input depends on the cleared data, and writes one byte
// stream to stdout.  unwritten_wide_output_test.py checks the output against a reference.

#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <llvm/Support/CommandLine.h>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>

using namespace llvm;
using namespace kernel;

static cl::OptionCategory testOptions("unwritten_wide_output Options", "Test options.");

static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(testOptions));

static cl::opt<unsigned> NumOfStreams("streams", cl::desc("Number of byte streams between the producer and the consumer"),
                                      cl::init(1), cl::cat(testOptions));

class WideIncrementKernel final : public BlockOrientedKernel {
public:
    WideIncrementKernel(LLVMTypeSystemInterface & ts, StreamSet * const input, StreamSet * const output)
    : BlockOrientedKernel(ts, "WideIncrement" + std::to_string(output->getNumElements()),
                          {Binding{"input", input}},
                          {Binding{"output", output}},
                          {}, {}, {})
    , mNumOfStreams(output->getNumElements()) { }
protected:
    void generateDoBlockMethod(KernelBuilder & b) override {
        for (unsigned s = 0; s < mNumOfStreams; ++s) {
            for (unsigned j = 0; j < 8; ++j) {
                Value * const in = b.loadInputStreamPack("input", b.getInt32(0), b.getInt32(j));
                Value * const out = b.simd_add(8, in, b.simd_fill(8, b.getInt8(1 + s)));
                b.storeOutputStreamPack("output", b.getInt32(s), b.getInt32(j), out);
            }
        }
    }
private:
    const unsigned mNumOfStreams;
};

class PackLookAheadKernel final : public BlockOrientedKernel {
public:
    PackLookAheadKernel(LLVMTypeSystemInterface & ts, StreamSet * const input, StreamSet * const output)
    : BlockOrientedKernel(ts, "PackLookAhead" + std::to_string(input->getNumElements()),
                          {Binding{"input", input, FixedRate(1), LookAhead(64)}},
                          {Binding{"output", output}},
                          {}, {}, {})
    , mNumOfStreams(input->getNumElements()) { }
protected:
    // output pack j = input[0] pack j ^ (the pack after pack j of every input stream)
    void generateDoBlockMethod(KernelBuilder & b) override {
        auto nextPack = [&](unsigned s, unsigned j) {
            if (j + 1 < 8) {
                return b.loadInputStreamPack("input", b.getInt32(s), b.getInt32(j + 1));
            }
            return b.loadInputStreamPack("input", b.getInt32(s), b.getInt32(0), b.getSize(1));
        };
        for (unsigned j = 0; j < 8; ++j) {
            Value * out = b.loadInputStreamPack("input", b.getInt32(0), b.getInt32(j));
            for (unsigned s = 0; s < mNumOfStreams; ++s) {
                out = b.simd_xor(out, nextPack(s, j));
            }
            b.storeOutputStreamPack("output", b.getInt32(0), b.getInt32(j), out);
        }
    }
private:
    const unsigned mNumOfStreams;
};

typedef void (*TestFunctionType)(uint32_t fd);

static TestFunctionType generatePipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    Scalar * const fileDescriptor = P.getInputScalar("fd");
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, bytes);
    StreamSet * const wide = P.CreateStreamSet(NumOfStreams, 8);
    P.CreateKernelCall<WideIncrementKernel>(bytes, wide);
    StreamSet * const result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<PackLookAheadKernel>(wide, result);
    P.CreateKernelCall<StdOutKernel>(result);
    return P.compile();
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&testOptions, codegen::codegen_flags()});
    CPUDriver driver("unwritten_wide_output");
    auto fn = generatePipeline(driver);
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        std::cerr << "Error: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// Test driver for the ProvisionalLookAheadStride kernel attribute.
//
// Reads a file, transposes it to 8 basis bit streams, passes them through a chain
// of Pablo kernels that each combine Lookahead, Advance and MatchStar, and writes
// the transposed result to stdout.  The chain kernels receive the
// ProvisionalLookAheadStride attribute automatically; running with
// -ProvisionalLookAheadStride=false must give identical output, and
// provisional_lookahead_test.py compares the two.

#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <pablo/pablo.h>
#include <llvm/Support/CommandLine.h>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>

using namespace llvm;
using namespace kernel;
using namespace pablo;

static cl::OptionCategory testOptions("provisional_lookahead Options", "Test options.");

static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(testOptions));

static cl::list<unsigned> LookAheads("la", cl::desc("Lookahead amount of each kernel in the chain"),
                                     cl::CommaSeparated, cl::cat(testOptions));

static cl::opt<bool> Invert("invert", cl::desc("Complement the output of each chain kernel, so that it is nonzero past the end of input"),
                            cl::init(false), cl::cat(testOptions));

class LookAheadMixKernel final : public PabloKernel {
public:
    LookAheadMixKernel(LLVMTypeSystemInterface & ts, StreamSet * const input, StreamSet * const output,
                       const unsigned lookAhead, const unsigned index, const bool invert)
    : PabloKernel(ts, "LookAheadMix" + std::to_string(lookAhead) + "_" + std::to_string(index) + (invert ? "_inv" : ""),
                  {Binding{"input", input, FixedRate(1), LookAhead(lookAhead)}},
                  {Binding{"output", output}})
    , mLookAhead(lookAhead)
    , mInvert(invert) { }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> in = getInputStreamSet("input");
        Var * const output = getOutputStreamVar("output");
        const unsigned n = in.size();
        for (unsigned i = 0; i < n; ++i) {
            PabloAST * la = pb.createLookahead(in[(i + 1) % n], mLookAhead);
            PabloAST * adv = pb.createAdvance(in[(i + 2) % n], 3);
            PabloAST * star = pb.createMatchStar(in[i], in[(i + 3) % n]);
            PabloAST * out = pb.createXor(pb.createXor(la, adv), star);
            if (mInvert) {
                out = pb.createNot(out);
            }
            pb.createAssign(pb.createExtract(output, pb.getInteger(i)), out);
        }
    }
private:
    const unsigned mLookAhead;
    const bool mInvert;
};

typedef void (*TestFunctionType)(uint32_t fd);

static TestFunctionType generatePipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    Scalar * const fileDescriptor = P.getInputScalar("fd");
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, bytes);
    StreamSet * basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    unsigned index = 0;
    for (const auto la : LookAheads) {
        StreamSet * const next = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<LookAheadMixKernel>(basis, next, la, index++, Invert);
        basis = next;
    }
    StreamSet * const result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(basis, result);
    P.CreateKernelCall<StdOutKernel>(result);
    return P.compile();
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&testOptions, codegen::codegen_flags()});
    if (LookAheads.empty()) {
        LookAheads.push_back(1);
    }
    CPUDriver driver("provisional_lookahead");
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

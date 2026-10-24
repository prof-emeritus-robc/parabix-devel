/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// Test driver for the LocalCarries kernel attribute.
//
// Reads a file, transposes it to 8 basis bit streams, passes them through a chain
// of Pablo kernels that each combine an Advance (short, long or of more than a block)
// with an If-block containing further Advances, and writes the transposed result to
// stdout.  Each chain kernel declares LocalCarries, so it rebuilds its carries at
// the start of each segment; running with -LocalCarries=false must give identical
// output, and local_carries_test.py compares the two.

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

static cl::OptionCategory testOptions("local_carries Options", "Test options.");

static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(testOptions));

static cl::list<unsigned> Advances("adv", cl::desc("Advance amount of each kernel in the chain"),
                                   cl::CommaSeparated, cl::cat(testOptions));

class AdvanceIfMixKernel final : public PabloKernel {
public:
    AdvanceIfMixKernel(LLVMTypeSystemInterface & ts, StreamSet * const input, StreamSet * const output,
                       const unsigned amount, const unsigned index)
    : PabloKernel(ts, "AdvanceIfMix" + std::to_string(amount) + "_" + std::to_string(index),
                  {Binding{"input", input}},
                  {Binding{"output", output}})
    , mAmount(amount) {
        // The longest path is the Advance by amount followed by the Advance by 2 in the If-body.
        addAttribute(LocalCarries(amount + 2));
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> in = getInputStreamSet("input");
        Var * const output = getOutputStreamVar("output");
        const unsigned n = in.size();
        for (unsigned i = 0; i < n; ++i) {
            PabloAST * const adv = pb.createAdvance(in[(i + 2) % n], mAmount);
            PabloAST * const cond = in[(i + 1) % n];
            Var * const inner = pb.createVar("inner" + std::to_string(i), pb.createZeroes());
            PabloBuilder body = pb.createScope();
            PabloAST * const follows = body.createAnd(body.createAdvance(cond, 1), adv);
            body.createAssign(inner, body.createAdvance(follows, 2));
            pb.createIf(cond, body);
            PabloAST * const out = pb.createXor(pb.createXor(in[i], adv), inner);
            pb.createAssign(pb.createExtract(output, pb.getInteger(i)), out);
        }
    }
private:
    const unsigned mAmount;
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
    for (const auto amount : Advances) {
        StreamSet * const next = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<AdvanceIfMixKernel>(basis, next, amount, index++);
        basis = next;
    }
    StreamSet * const result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(basis, result);
    P.CreateKernelCall<StdOutKernel>(result);
    return P.compile();
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&testOptions, codegen::codegen_flags()});
    if (Advances.empty()) {
        Advances.push_back(1);
    }
    CPUDriver driver("local_carries");
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

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// A cat program of the Parabix input and output conventions
// (toolchain/toolio.h): FDSourceKernel to FDSinkKernel, from a file or the
// standard input, to the standard output or --output.  QA/toolio_test.py
// tests them with it.
//
//   toolio_cat [--mmap] [--output=<file>] [--trailer=<text>] [<input file>]

#include <string>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>

using namespace kernel;
using namespace llvm;

static toolio::RegisterInputOptions InputOptions;
static toolio::RegisterOutputOptions OutputOptions;

static cl::OptionCategory CatOptions("toolio_cat Options", "toolio_cat Options.");
static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input file>"), cl::init("-"), cl::cat(CatOptions));
static cl::opt<std::string> Trailer("trailer", cl::desc("Text written with toolio::out() after the input."), cl::cat(CatOptions));

typedef void (*CatFunctionType)(uint32_t useMMap, uint32_t inputFD, uint32_t outputFD);

static CatFunctionType generatePipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>{"useMMap"}, Input<uint32_t>{"inputFD"}, Input<uint32_t>{"outputFD"});
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<FDSourceKernel>(P.getInputScalar("useMMap"), P.getInputScalar("inputFD"), bytes);
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), bytes);
    return P.compile();
}

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&CatOptions, &toolio::InputOutputOptions});
    toolio::Input in;
    if (!toolio::openInput(InputFile, in) || !toolio::openOutput()) {
        return 1;
    }
    CPUDriver driver("toolio_cat");
    const CatFunctionType cat = generatePipeline(driver);
    cat(in.useMMap, in.fd, toolio::outputFD());
    toolio::closeInput(in);
    if (!Trailer.empty()) {
        toolio::out() << Trailer;
    }
    return toolio::finishOutput() ? 0 : 1;
}

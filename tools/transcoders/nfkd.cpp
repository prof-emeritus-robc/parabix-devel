/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <string>
#include <fcntl.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/streamutils/focused_transform.h>
#include <kernel/unicode/normalization/normalization.h>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>

using namespace kernel;
using namespace llvm;

//  These declarations are for command line processing.
//  See the LLVM CommandLine Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory NFKD_Options("Decomposition Options", "Decomposition Options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(NFKD_Options));
static cl::opt<bool> NoFocus("NoFocus", cl::desc("Process the entire file without filtering to narrow the focus of work"), cl::init(false), cl::cat(NFKD_Options));
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)

void source_input_stage(PipelineBuilder & P, Scalar *const fileDescriptor, StreamSet * ByteStream, StreamSet * BasisBits) {
    P.SetStageLabel("source");
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    Selected_S2P(P, ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);
}

typedef void (*XfrmFunctionType)(uint32_t fd);

XfrmFunctionType generate_NFKD_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>("inputFileDecriptor"));
    NFD_PipelineBuilder NFD_builder(P);
    FocusOptions options = FocusOptions::fromCommandLine();
    options.Focus = !NoFocus;
    const auto NFKD = NFKD_FocusedTransform(NFD_builder, options);
    Scalar * fileDescriptor = P.getInputScalar("inputFileDecriptor");

    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    source_input_stage(P, fileDescriptor, ByteStream, BasisBits);

    StreamSet * const OutputBytes = NFKD.build(P, ByteStream, BasisBits);
    P.CreateKernelCall<StdOutKernel>(OutputBytes);
    return P.compile();
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&NFKD_Options, &FocusedTransformOptions(), &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing.\n";
        exit(-1);
    }
    CPUDriver driver("NFKD_function");
    XfrmFunctionType fn = generate_NFKD_pipeline(driver);
    fn(fd);
    close(fd);
    return 0;
}

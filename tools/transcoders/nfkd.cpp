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
#include <kernel/basis/p2s_kernel.h>
#include <kernel/unicode/utf8_wellformed.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/streamutils/focused_transform.h>
#include <kernel/unicode/normalization/normalization.h>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <kernel/pipeline/driver/cpudriver.h>

using namespace kernel;
using namespace llvm;

//  These declarations are for command line processing.
//  See the LLVM CommandLine Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory NFKD_Options("Decomposition Options", "Decomposition Options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::cat(NFKD_Options));
static toolio::RegisterInputOptions InputOptions;
static toolio::RegisterOutputOptions OutputOptions;
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)

//  The source: the input made well-formed UTF-8 (see kernel::WellFormedUTF8 and
//  --utf8-errors), as basis bits and bytes.
void source_input_stage(PipelineBuilder & P, Scalar *const fileDescriptor, StreamSet * ByteStream, StreamSet * BasisBits) {
    P.SetStageLabel("source");
    StreamSet * const InputBytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<FDSourceKernel>(P.getInputScalar("useMMap"), fileDescriptor, InputBytes);
    StreamSet * const InputBasis = P.CreateStreamSet(8, 1);
    Selected_S2P(P, InputBytes, InputBasis);
    WellFormedUTF8(P, InputBasis, BasisBits);
    P.CreateKernelCall<P2SKernel>(BasisBits, ByteStream);
    SHOW_BIXNUM(BasisBits);
}

//  With --utf8-errors=abort-on-error, the input is processed up to its first
//  ill-formed UTF-8: the error is reported, with its offset.
static bool reportUTF8Abort(size_t offset, const std::string & name) {
    llvm::errs() << toolio::programName() << ": " << name << ": ill-formed UTF-8 at byte " << offset
                 << ": the input after it is not processed (--utf8-errors=abort-on-error)\n";
    return true;
}

static bool reportUTF8Abort(const toolio::Input & in) {
    size_t offset = 0;
    return codegen::UTF8Errors == codegen::UTF8ErrorMode::Abort && UTF8_FileError(in.fd, in.useMMap, offset) &&
           reportUTF8Abort(offset, in.name);
}


typedef void (*XfrmFunctionType)(uint32_t useMMap, uint32_t fd, uint32_t outputFD);

XfrmFunctionType generate_NFKD_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>("useMMap"), Input<uint32_t>("inputFileDecriptor"), Input<uint32_t>("outputFD"));
    NFD_PipelineBuilder NFD_builder(P);
    const auto NFKD = NFKD_FocusedTransform(NFD_builder, FocusOptions::fromCommandLine());
    Scalar * fileDescriptor = P.getInputScalar("inputFileDecriptor");

    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    source_input_stage(P, fileDescriptor, ByteStream, BasisBits);

    StreamSet * const OutputBytes = NFKD.build(P, ByteStream, BasisBits);
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), OutputBytes);
    return P.compile();
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&NFKD_Options, &toolio::InputOutputOptions, &FocusedTransformOptions(), &NormalizationOptions(), &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});
    toolio::Input in;
    if (!toolio::openInput(inputFile, in) || !toolio::openOutput()) {
        return 1;
    }
    CPUDriver driver("NFKD_function");
    XfrmFunctionType fn = generate_NFKD_pipeline(driver);
    fn(in.useMMap, in.fd, toolio::outputFD());
    const bool aborted = reportUTF8Abort(in);
    toolio::closeInput(in);
    return (toolio::finishOutput() && !aborted) ? 0 : 1;
}

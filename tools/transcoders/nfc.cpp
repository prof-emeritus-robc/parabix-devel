/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <fcntl.h>
#include <string>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/streamutils/focused_transform.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/unicode/normalization/normalization.h>
#include <toolchain/fileutil.h>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/streamsetptr.h>

using namespace kernel;
using namespace llvm;

//  These declarations are for command line processing.
//  See the LLVM CommandLine Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory NFC_Options("Composition Options", "Composition Options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(NFC_Options));
static cl::opt<bool> PreferMMap("useMMap", cl::desc("Use mmap for file input, if possible"), cl::init(false), cl::cat(NFC_Options));
static cl::opt<bool> MultiStage("MultiStage", cl::desc("Use separate pipelines for the focus, transform and reintegration stages"), cl::init(false), cl::cat(NFC_Options));
static cl::opt<bool> ByteFiltering("ByteFiltering", cl::desc("Use byte filtering for focused work (alias of -focus-filter=byte)"), cl::init(false), cl::cat(NFC_Options));

static cl::opt<bool> UseLayers("UseLayers", cl::desc("Use pipeline layers (alias of -focus-layers=filter)"), cl::init(false), cl::cat(NFC_Options));

#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)

//  The focused-transform options of nfc: the library options with the earlier nfc
//  options as aliases.
static FocusOptions NFC_FocusOptions() {
    FocusOptions options = FocusOptions::fromCommandLine();
    if (UseLayers) {
        options.addLayerAfter(FocusStage::Filter);
    }
    if (ByteFiltering) {
        options.FocusFilter = FocusFilterMode::Byte;
    }
    return options;
}

void source_logic(PipelineBuilder & P, StreamSet * ByteStream, StreamSet * BasisBits) {
    P.SetStageLabel("source");
    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");
    P.CreateKernelCall<MemorySourceKernel>(buffer, length, ByteStream);

    Selected_S2P(P, ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);
}

//  MultiStage: the focus, transform and reintegration stages as separate pipelines.

typedef void (*FocusStageFunctionType)(StreamSetPtr &, StreamSetPtr &, StreamSetPtr &, const char * buffer, const size_t length);

FocusStageFunctionType focus_stage_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver,
                            Output<streamset_t>("ByteStream", 1, 8, ReturnedBuffer(1)),
                            Output<streamset_t>("WorkSelectionMask", 1, 1, ReturnedBuffer(1)),
                            Output<streamset_t>("FocusedBasis", 8, 1, ReturnedBuffer(1)),
                            Input<const char*>{"buffer"}, Input<size_t>{"length"}
                            );
    const auto NFC = NFC_FocusedTransform(NFC_FocusOptions());
    StreamSet * const ByteStream = P.getOutputStreamSet("ByteStream");
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    source_logic(P, ByteStream, BasisBits);
    StreamSet * const WorkSelectionMask = P.getOutputStreamSet("WorkSelectionMask");
    NFC.focus(P, BasisBits, WorkSelectionMask);
    NFC.filter(P, ByteStream, BasisBits, WorkSelectionMask, P.getOutputStreamSet("FocusedBasis"));
    return P.compile();
}

typedef void (*TransformStageFunctionType)(const StreamSetPtr &, StreamSetPtr &, StreamSetPtr &);

TransformStageFunctionType transform_stage_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver,
                            Input<streamset_t>("FocusedBasis", 8, 1),
                            Output<streamset_t>("TransformedBasis", 8, 1, ReturnedBuffer(1)),
                            Output<streamset_t>("ValidWorkMask", 1, 1, ReturnedBuffer(1))
                            );
    const auto NFC = NFC_FocusedTransform(NFC_FocusOptions());
    NFC.transform(P, P.getInputStreamSet("FocusedBasis"), P.getOutputStreamSet("TransformedBasis"), P.getOutputStreamSet("ValidWorkMask"));
    return P.compile();
}

typedef void (*FinalStageFunctionType)(const StreamSetPtr &, const StreamSetPtr &, const StreamSetPtr &, const StreamSetPtr &);

FinalStageFunctionType final_stage_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver,
                            Input<streamset_t>("ByteStream", 1, 8),
                            Input<streamset_t>("WorkSelectionMask", 1, 1),
                            Input<streamset_t>("ValidWorkMask", 1, 1),
                            Input<streamset_t>("TransformedBasis", 8, 1)
                            );
    const auto NFC = NFC_FocusedTransform(NFC_FocusOptions());
    StreamSet * const ByteStream = P.getInputStreamSet("ByteStream");
    StreamSet * const WorkSelectionMask = P.getInputStreamSet("WorkSelectionMask");
    P.SetStageLabel("placement");
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    Selected_S2P(P, ByteStream, BasisBits);
    StreamSet * const PlacementMask = NFC.validatedPlacement(P, BasisBits, WorkSelectionMask, P.getInputStreamSet("ValidWorkMask"));
    StreamSet * const OutputBytes = NFC.reintegrate(P, WorkSelectionMask, PlacementMask, ByteStream, BasisBits,
                                                    P.getInputStreamSet("TransformedBasis"));
    P.CreateKernelCall<StdOutKernel>(OutputBytes);
    return P.compile();
}

typedef void (*XfrmFunctionType)(const char * buffer, const size_t length);

XfrmFunctionType generate_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<const char*>{"buffer"}, Input<size_t>{"length"});
    const auto NFC = NFC_FocusedTransform(NFC_FocusOptions());

    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    source_logic(P, ByteStream, BasisBits);

    StreamSet * OutputBytes = nullptr;
    if (!NFC.getOptions().Focus) {
        // The whole text is transformed, with room for decompositions inserted only
        // within the work regions.
        P.SetStageLabel("focus");
        StreamSet * const WorkSelectionMask = P.CreateStreamSet(1, 1);
        DetermineNFC_WorkSelection(P, BasisBits, WorkSelectionMask);
        P.SetStageLabel("transform");
        StreamSet * const TransformedBasis = P.CreateStreamSet(8, 1);
        StreamSet * const ValidWorkMask = P.CreateStreamSet(1, 1);
        NFC_Transform(P, BasisBits, TransformedBasis, ValidWorkMask, WorkSelectionMask);
        P.SetStageLabel("output");
        OutputBytes = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(TransformedBasis, OutputBytes);
    } else {
        OutputBytes = NFC.build(P, ByteStream, BasisBits);
    }
    P.CreateKernelCall<StdOutKernel>(OutputBytes);
    return P.compile();
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&NFC_Options, &FocusedTransformOptions(), &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});
    if (MultiStage && !NFC_FocusOptions().Focus) {
        llvm::report_fatal_error("-MultiStage requires focus (not -NoFocus)");
    }

    bool useMMap = PreferMMap && canMMap(inputFile);
    AlignedFileBuffer buf;
    buf.load(inputFile, useMMap);
    size_t bytes_read = buf.getBufSize();
    if (bytes_read <= 0) {
        llvm::errs() << "Error: unable to read " << inputFile << " for processing.\n";
        exit(-1);
    }

    CPUDriver driver("NFC_function");
    if (MultiStage) {
        FocusStageFunctionType focus_stage = focus_stage_pipeline(driver);
        TransformStageFunctionType transform_stage = transform_stage_pipeline(driver);
        FinalStageFunctionType final_stage = final_stage_pipeline(driver);
        kernel::StreamSetPtr ByteStream;
        kernel::StreamSetPtr WorkSelectionMask;
        kernel::StreamSetPtr FocusedBasis;
        kernel::StreamSetPtr TransformedBasis;
        kernel::StreamSetPtr ValidWorkMask;
        focus_stage(ByteStream, WorkSelectionMask, FocusedBasis, buf.getBuf(), buf.getBufSize());
        transform_stage(FocusedBasis, TransformedBasis, ValidWorkMask);
        final_stage(ByteStream, WorkSelectionMask, ValidWorkMask, TransformedBasis);
    } else {
        XfrmFunctionType fn = generate_pipeline(driver);
        fn(buf.getBuf(), buf.getBufSize());
    }
    buf.release();
    return 0;
}

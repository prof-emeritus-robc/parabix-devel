/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <fcntl.h>
#include <string>
#include <cerrno>
#include <cstring>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/unicode/utf8_wellformed.h>
#include <kernel/streamutils/focused_transform.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/unicode/normalization/normalization.h>
#include <kernel/unicode/normalization/nfc_tables.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/bitwise/bixnum_kernel.h>
#include <re/adt/re_cc.h>
#include <re/cc/cc_compiler.h>
#include <toolchain/fileutil.h>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/streamsetptr.h>

using namespace kernel;
using namespace llvm;

//  These declarations are for command line processing.
//  See the LLVM CommandLine Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory NFC_Options("Composition Options", "Composition Options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::cat(NFC_Options));
static toolio::RegisterInputOptions InputOptions;
static toolio::RegisterOutputOptions OutputOptions;
static cl::opt<bool> MultiStage("MultiStage", cl::desc("Use separate pipelines for the focus, transform and reintegration stages"), cl::init(false), cl::cat(NFC_Options));
static cl::opt<bool> ByteFiltering("ByteFiltering", cl::desc("Use byte filtering for focused work (alias of -focus-filter=byte)"), cl::init(false), cl::cat(NFC_Options));

static cl::opt<bool> DumpNFC_Tables("dump-nfc-tables", cl::desc("Print the NFC analysis (NFC_Tables) in the text form of "
                                    "scripts/UCD/nfc_generator.py --dump-tables, and exit"), cl::init(false), cl::cat(NFC_Options), cl::Hidden);
static cl::opt<std::string> NFC_MaskSet("nfc-mask", cl::desc("Normalize only within the characters of a set (hexadecimal "
                                       "ranges, e.g. 41-5a,300-36f): each run of them by itself, the others unchanged "
                                       "(NFC_Masked, focused or with -NoFocus in place; for testing)"), cl::init(""), cl::cat(NFC_Options), cl::Hidden);
static cl::opt<bool> NFC_U21Mode("nfc-u21", cl::desc("Normalize on the 21-bit basis (NFC_U21): the input decoded, and the "
                                 "result encoded to UTF-8 (for testing)"), cl::init(false), cl::cat(NFC_Options), cl::Hidden);
static cl::opt<bool> NFC_U21ViaUTF8("nfc-u21-via-utf8", cl::desc("With -nfc-u21, normalize the 21-bit basis by encoding it to "
                                    "UTF-8, the focused NFC of the bytes, and decoding the result (for measurement)"),
                                    cl::init(false), cl::cat(NFC_Options), cl::Hidden);
enum class MaskSelect {All, Inside, Outside};
static cl::opt<MaskSelect> NFC_MaskSelect("nfc-mask-select", cl::desc("With -nfc-mask, print only the output bytes inside or "
                                          "outside the output mask (for testing it)"),
    cl::values(clEnumValN(MaskSelect::All, "all", "all the output (default)"),
               clEnumValN(MaskSelect::Inside, "inside", "the bytes within the output mask"),
               clEnumValN(MaskSelect::Outside, "outside", "the bytes outside it")),
    cl::init(MaskSelect::All), cl::cat(NFC_Options), cl::Hidden);
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

//  The source: the input made well-formed UTF-8 (see kernel::WellFormedUTF8 and
//  --utf8-errors), as basis bits and bytes.
void source_logic(PipelineBuilder & P, StreamSet * ByteStream, StreamSet * BasisBits) {
    P.SetStageLabel("source");
    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");
    StreamSet * const InputBytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<MemorySourceKernel>(buffer, length, InputBytes);
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

typedef void (*FinalStageFunctionType)(const StreamSetPtr &, const StreamSetPtr &, const StreamSetPtr &, const StreamSetPtr &, uint32_t outputFD);

FinalStageFunctionType final_stage_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver,
                            Input<streamset_t>("ByteStream", 1, 8),
                            Input<streamset_t>("WorkSelectionMask", 1, 1),
                            Input<streamset_t>("ValidWorkMask", 1, 1),
                            Input<streamset_t>("TransformedBasis", 8, 1),
                            Input<uint32_t>("outputFD")
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
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), OutputBytes);
    return P.compile();
}

typedef void (*XfrmFunctionType)(const char * buffer, const size_t length, uint32_t outputFD);

XfrmFunctionType generate_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<const char*>{"buffer"}, Input<size_t>{"length"}, Input<uint32_t>("outputFD"));
    const auto NFC = NFC_FocusedTransform(NFC_FocusOptions());

    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    source_logic(P, ByteStream, BasisBits);

    StreamSet * OutputBytes = nullptr;
    //  (-nfc-mask: the bytes of the characters of the set.)
    StreamSet * Mask = nullptr;
    if (!NFC_MaskSet.empty()) {
        //  (-nfc-mask: the bytes of the characters of the set.)
        UCD::UnicodeSet set;
        const std::string spec = NFC_MaskSet;
        for (size_t pos = 0; pos < spec.size(); ) {
            size_t comma = spec.find(',', pos);
            if (comma == std::string::npos) comma = spec.size();
            const std::string r = spec.substr(pos, comma - pos);
            const size_t dash = r.find('-');
            const unsigned lo = std::stoul(r.substr(0, dash), nullptr, 16);
            const unsigned hi = dash == std::string::npos ? lo : std::stoul(r.substr(dash + 1), nullptr, 16);
            set = set + UCD::UnicodeSet(lo, hi);
            pos = comma + 1;
        }
        StreamSet * const firsts = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{re::makeCC(set, &cc::Unicode)}, BasisBits, firsts,
                                              pablo::BitMovementMode::LookAhead);
        StreamSet * const u8index = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<UTF8_index>(BasisBits, u8index);
        Mask = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<U8Spans>(firsts, u8index, Mask, pablo::BitMovementMode::Advance);
    }
    if (NFC_U21Mode) {
        //  (-nfc-u21: the characters decoded, normalized on the 21-bit basis, and encoded.)
        StreamSet * const U21_u8indexed = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);
        StreamSet * const u8index = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<UTF8_index>(BasisBits, u8index);
        StreamSet * const U21 = P.CreateStreamSet(21, 1);
        FilterByMask(P, u8index, U21_u8indexed, U21);
        StreamSet * Mask21 = nullptr;
        if (Mask) {
            Mask21 = P.CreateStreamSet(1, 1);
            FilterByMask(P, u8index, Mask, Mask21);
        }
        StreamSet * const OutputMask = Mask && NFC_MaskSelect != MaskSelect::All ? P.CreateStreamSet(1, 1) : nullptr;
        StreamSet * Result = nullptr;
        if (NFC_U21ViaUTF8) {
            StreamSet * const Basis8 = P.CreateStreamSet(8, 1);
            U21_to_UTF8(P, U21, Basis8);
            StreamSet * const Bytes8 = P.CreateStreamSet(1, 8);
            P.CreateKernelCall<P2SKernel>(Basis8, Bytes8);
            StreamSet * const Normalized = NFC.build(P, Bytes8, Basis8);
            StreamSet * const NormalizedBasis = P.CreateStreamSet(8, 1);
            Selected_S2P(P, Normalized, NormalizedBasis);
            StreamSet * const Decoded = P.CreateStreamSet(21, 1);
            P.CreateKernelCall<UTF8_Decoder>(NormalizedBasis, Decoded);
            StreamSet * const index = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<UTF8_index>(NormalizedBasis, index);
            Result = P.CreateStreamSet(21, 1);
            FilterByMask(P, index, Decoded, Result);
        } else {
            Result = NFC_U21(P, U21, Mask21, OutputMask);
        }
        if (OutputMask) {
            StreamSet * selected = OutputMask;
            if (NFC_MaskSelect == MaskSelect::Outside) {
                selected = P.CreateStreamSet(1, 1);
                P.CreateKernelCall<bixnum::EQ_immediate>(OutputMask, 0, selected);
            }
            StreamSet * const Selected = P.CreateStreamSet(21, 1);
            FilterByMask(P, selected, Result, Selected);
            Result = Selected;
        }
        StreamSet * const OutputBasis = P.CreateStreamSet(8, 1);
        U21_to_UTF8(P, Result, OutputBasis);
        OutputBytes = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    } else if (Mask) {
        StreamSet * const OutputMask = NFC_MaskSelect == MaskSelect::All ? nullptr : P.CreateStreamSet(1, 1);
        OutputBytes = NFC_Masked(P, NFC.getOptions(), ByteStream, BasisBits, Mask, OutputMask);
        if (OutputMask) {
            StreamSet * selected = OutputMask;
            if (NFC_MaskSelect == MaskSelect::Outside) {
                selected = P.CreateStreamSet(1, 1);
                P.CreateKernelCall<bixnum::EQ_immediate>(OutputMask, 0, selected);
            }
            StreamSet * const OutputBasis = P.CreateStreamSet(8, 1);
            Selected_S2P(P, OutputBytes, OutputBasis);
            StreamSet * const SelectedBasis = P.CreateStreamSet(8, 1);
            FilterByMask(P, selected, OutputBasis, SelectedBasis);
            OutputBytes = P.CreateStreamSet(1, 8);
            P.CreateKernelCall<P2SKernel>(SelectedBasis, OutputBytes);
        }
    } else if (!NFC.getOptions().Focus) {
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
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), OutputBytes);
    return P.compile();
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&NFC_Options, &toolio::InputOutputOptions, &FocusedTransformOptions(), &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});
    if (DumpNFC_Tables) {
        NFC_Tables::get().dump(llvm::outs());
        return 0;
    }
    if (MultiStage && !NFC_FocusOptions().Focus) {
        llvm::report_fatal_error("-MultiStage requires focus (not -NoFocus)");
    }

    toolio::Input in;
    if (!toolio::openInput(inputFile, in) || !toolio::openOutput()) {
        return 1;
    }
    AlignedFileBuffer buf;
    if (!buf.load(in.fd, in.useMMap)) {
        llvm::errs() << toolio::programName() << ": " << in.name << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    toolio::closeInput(in);

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
        final_stage(ByteStream, WorkSelectionMask, ValidWorkMask, TransformedBasis, toolio::outputFD());
    } else {
        XfrmFunctionType fn = generate_pipeline(driver);
        fn(buf.getBuf(), buf.getBufSize(), toolio::outputFD());
    }
    const size_t offset = UTF8_ErrorOffset(buf.getBuf(), buf.getBufSize());
    const bool aborted = codegen::UTF8Errors == codegen::UTF8ErrorMode::Abort && offset < buf.getBufSize() &&
                         reportUTF8Abort(offset, in.name);
    buf.release();
    return (toolio::finishOutput() && !aborted) ? 0 : 1;
}

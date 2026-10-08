/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  tconv: apply an LDML transform (UTS #35 Part 2) to a UTF-8 file.
//
//  tconv [options] <transform name> <input file>
//      Write the transformed file to standard output.
//  tconv [options] --list-transforms
//      List the known transforms: canonical name, aliases, defining file.
//
//  Transform names are the built-in transforms (Any-NFD, Any-Lower, ...)
//  and the names and aliases of the transforms defined by the CLDR files in
//  the --transforms-dir directory (default ~/cldr/common/transforms).
//  Names are matched ignoring case, and a name without a source means the
//  Any source ("NFD" is "Any-NFD").
//
//  Implemented so far:
//      Any-NFD, Any-NFKD   NFD_PipelineBuilder, as in the nfd and nfkd tools
//      Any-NFC             the focused NFC pipeline of the nfc tool
//      Any-NFKC            NFC applied to the output of NFKD
//      Any-Lower, Any-Upper, Any-Title
//                          full case mapping by the string override
//                          properties lc, uc and tc, as in xch -prop=...
//  Any other known name is reported with its canonical name as not yet
//  implemented.

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/bitwise/bixlogic.h>
#include <kernel/bitwise/bixnum_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/sorting.h>
#include <kernel/unicode/char_replacement.h>
#include <kernel/unicode/normalization/normalization.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <ldml/transform_registry.h>
#include <re/adt/re_name.h>
#include <re/unicode/resolve_properties.h>
#include <toolchain/toolchain.h>
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

static cl::OptionCategory TconvOptions("Transform Options", "LDML transform options.");
static cl::opt<std::string> TransformName(cl::Positional, cl::desc("<transform name>"), cl::cat(TconvOptions));
static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input file>"), cl::cat(TconvOptions));
static cl::opt<std::string> TransformsDir("transforms-dir", cl::desc("Directory of CLDR transform files (default ~/cldr/common/transforms)"),
                                          cl::init(""), cl::cat(TconvOptions));
static cl::opt<bool> ListTransforms("list-transforms", cl::desc("List the known transform names and exit"),
                                    cl::init(false), cl::cat(TconvOptions));
static cl::opt<bool> ShowWarnings("show-transform-warnings", cl::desc("Report problems found in the transform files"),
                                  cl::init(false), cl::cat(TconvOptions));

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

enum class Transform {NFD, NFKD, NFC, NFKC, Lower, Upper, Title};

static const std::vector<std::pair<std::string, Transform>> implementedTransforms = {
    {"Any-NFD", Transform::NFD}, {"Any-NFKD", Transform::NFKD},
    {"Any-NFC", Transform::NFC}, {"Any-NFKC", Transform::NFKC},
    {"Any-Lower", Transform::Lower}, {"Any-Upper", Transform::Upper}, {"Any-Title", Transform::Title},
};

//  Basis bits of a byte stream.
static StreamSet * BasisOf(PipelineBuilder & P, StreamSet * Bytes) {
    StreamSet * BasisBits = P.CreateStreamSet(8, 1);
    Selected_S2P(P, Bytes, BasisBits);
    SHOW_BIXNUM(BasisBits);
    return BasisBits;
}

static StreamSet * BytesOf(PipelineBuilder & P, StreamSet * BasisBits) {
    StreamSet * Bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(BasisBits, Bytes);
    SHOW_BYTES(Bytes);
    return Bytes;
}

//  The 21-bit codepoint basis of UTF-8 basis bits, one position per character.
static StreamSet * U21_Of(PipelineBuilder & P, StreamSet * BasisBits) {
    StreamSet * const U21_u8indexed = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);
    SHOW_BIXNUM(U21_u8indexed);

    StreamSet * const U8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, U8index);
    SHOW_STREAM(U8index);

    StreamSet * const U21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, U8index, U21_u8indexed, U21);
    SHOW_BIXNUM(U21);
    return U21;
}

static StreamSet * UTF8_Of(PipelineBuilder & P, StreamSet * U21) {
    StreamSet * const U8_Basis = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, U21, U8_Basis);
    SHOW_BIXNUM(U8_Basis);
    return U8_Basis;
}

//  NFD, as in the default (focused) pipeline of the nfd tool: only the
//  spans that may change are decomposed and merged back with the rest.
static StreamSet * NFD_Basis(PipelineBuilder & P, NFD_PipelineBuilder & NFD_builder, StreamSet * BasisBits) {
    StreamSet * const WorkSelectionMask = P.CreateStreamSet(1, 1);
    StreamSet * const WorkingBasis = P.CreateStreamSet(8, 1);
    NFD_builder.NFD_FilterStage(BasisBits, WorkSelectionMask, WorkingBasis);

    StreamSet * const FinalWorkPlacementMask = P.CreateStreamSet(1, 1);
    NFD_builder.ComputeWorkPlacementMask(BasisBits, WorkSelectionMask, FinalWorkPlacementMask);

    StreamSet * const TransformedBasis = P.CreateStreamSet(8, 1);
    NFD_builder.NFD_U8_Pipeline(WorkingBasis, TransformedBasis);

    StreamSet * const NonModifiedMask = P.CreateStreamSet(1, 1);
    Invert(P, WorkSelectionMask, NonModifiedMask);
    SHOW_STREAM(NonModifiedMask);

    StreamSet * const NonModifiedBasis = P.CreateStreamSet(8);
    FilterByMask(P, NonModifiedMask, BasisBits, NonModifiedBasis);
    SHOW_BIXNUM(NonModifiedBasis);

    StreamSet * const OutputBasis = P.CreateStreamSet(8);
    MergeByMask(P, FinalWorkPlacementMask, TransformedBasis, NonModifiedBasis, OutputBasis);
    SHOW_BIXNUM(OutputBasis);
    return OutputBasis;
}

//  NFKD, as in the nfkd tool.
static StreamSet * NFKD_Basis(PipelineBuilder & P, NFD_PipelineBuilder & NFD_builder, StreamSet * BasisBits) {
    StreamSet * const U21 = U21_Of(P, BasisBits);
    StreamSet * const NFKD_U21 = NFD_builder.NFKD_U21_Pipeline(U21);
    return UTF8_Of(P, NFKD_U21);
}

//  NFC, as in the default (focused, single-stage) pipeline of the nfc tool.

class NFC_Focus : public pablo::PabloKernel {
public:
    NFC_Focus(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * NFC_candidates, StreamSet * Focus);
protected:
    void generatePabloMethod() override;
};

NFC_Focus::NFC_Focus(LLVMTypeSystemInterface & ts,
                     StreamSet * Basis, StreamSet * NFC_candidates,
                     StreamSet * Focus)
: PabloKernel(ts, "NFC_Focus",
// inputs
{Binding{"Basis", Basis},
 Binding{"NFC_candidates", NFC_candidates, FixedRate(), LookAhead(4)}},
// output
{Binding{"Focus", Focus}}) {
}

void NFC_Focus::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> Basis = getInputStreamSet("Basis");
    std::vector<PabloAST *> NFC_candidates = getInputStreamSet("NFC_candidates");
    PabloAST * composable_seconds = NFC_candidates[0];
    PabloAST * excluded_composites = NFC_candidates[1];
    BixNumCompiler bnc(pb);
    PabloAST * pfx4 = bnc.UGE(Basis, 0xF0);
    PabloAST * pfx3or4 = bnc.UGE(Basis, 0xE0);
    PabloAST * pfx = bnc.UGE(Basis, 0xC2);
    PabloAST * pfx3 = pb.createXor(pfx3or4, pfx4);
    PabloAST * pfx2 = pb.createXor(pfx, pfx3or4);
    PabloAST * ASCII = bnc.ULE(Basis, 0x7F);
    PabloAST * focus = pb.createOr(composable_seconds, pb.createAnd(ASCII, pb.createLookahead(composable_seconds, 1)));
    focus = pb.createOr(focus, pb.createAnd(pfx2, pb.createLookahead(composable_seconds, 2)));
    focus = pb.createOr(focus, pb.createAnd(pfx3, pb.createLookahead(composable_seconds, 3)));
    focus = pb.createOr(focus, pb.createAnd(pfx4, pb.createLookahead(composable_seconds, 4)));
    focus = pb.createOr(focus, excluded_composites);
    pb.createAssign(pb.createExtract(getOutputStreamVar("Focus"), pb.getInteger(0)), focus);
}

static void DetermineNFC_WorkSpans(PipelineBuilder & P, StreamSet * U8_Basis, StreamSet * WorkSelectionMask) {
    StreamSet * NFC_Candidates = P.CreateStreamSet(2, 1);
    P.CreateKernelCall<NFC_CandidateClass>(U8_Basis, NFC_Candidates);
    SHOW_BIXNUM(NFC_Candidates);

    StreamSet * NFC_WorkItems = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<NFC_Focus>(U8_Basis, NFC_Candidates, NFC_WorkItems);
    SHOW_STREAM(NFC_WorkItems);

    StreamSet * const u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(U8_Basis, u8index);
    SHOW_STREAM(u8index);

    P.CreateKernelCall<U8Spans>(NFC_WorkItems, u8index, WorkSelectionMask, BitMovementMode::Advance);
    SHOW_STREAM(WorkSelectionMask);
}

static void NFC_U8_logic(PipelineBuilder & P, StreamSet * ExpansionMask, StreamSet * U8_Basis, StreamSet * FinalSelectionMask, StreamSet * TransformedBytes) {
    // The SourceNull stream identifies null bytes in the original source
    // stream, rather than ones generated by insertion or by zeroing out
    // characters to be deleted.
    StreamSet * const NullStream = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::EQ_immediate>(U8_Basis, 0, NullStream);
    StreamSet * const SourceNull = P.CreateStreamSet(1, 1);
    AndCombine(P, NullStream, ExpansionMask, SourceNull);
    SHOW_STREAM(SourceNull);

    StreamSet * EC_Basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<ExcludedCompositeStage>(U8_Basis, EC_Basis);
    SHOW_BIXNUM(EC_Basis);

    re::PropertyExpression * CCC0_Prop = re::makePropertyExpression("CCC", "NR");
    CCC0_Prop = cast<re::PropertyExpression>(UCD::linkAndResolve(CCC0_Prop));

    StreamSet * const ccc_NR0 = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(CCC0_Prop, EC_Basis, ccc_NR0, BitMovementMode::LookAhead);
    SHOW_STREAM(ccc_NR0);

    StreamSet * const ccc_NR = P.CreateStreamSet(1, 1);
    AndCombine(P, ccc_NR0, ExpansionMask, ccc_NR);
    SHOW_STREAM(ccc_NR);

    StreamSet * CanonBasis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<SingletonCanonicalization>(EC_Basis, CanonBasis);
    SHOW_BIXNUM(CanonBasis);

    StreamSet * ShortBasis = P.CreateStreamSet(8, 1);
    ShortComposablePipeline(P, CanonBasis, ShortBasis);
    SHOW_BIXNUM(ShortBasis);

    StreamSet * FinalBasis = P.CreateStreamSet(8, 1);
    LongComposablePipeline(P, ShortBasis, ccc_NR, FinalBasis);
    SHOW_BIXNUM(FinalBasis);

    StreamSet * L_V_T = P.CreateStreamSet(Hangul_Composables::HC_Kind::Count);
    P.CreateKernelCall<Hangul_Composables>(FinalBasis, L_V_T, pablo::BitMovementMode::LookAhead);
    SHOW_BIXNUM(L_V_T);

    StreamSet * TranslatedBasis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<Hangul_Composition>(FinalBasis, L_V_T, TranslatedBasis);
    SHOW_BIXNUM(TranslatedBasis);

    StreamSet * NonZeroResults = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(TranslatedBasis, 0, NonZeroResults);

    OrCombine(P, NonZeroResults, SourceNull, FinalSelectionMask);
    SHOW_STREAM(FinalSelectionMask);

    StreamSet * TransformedBasis = P.CreateStreamSet(8, 1);
    FilterByMask(P, FinalSelectionMask, TranslatedBasis, TransformedBasis);

    UCD::EnumeratedPropertyObject * enumObj = llvm::cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    StreamSet * const CCC_Basis = P.CreateStreamSet(enumObj->GetEnumerationBasisSets().size(), 1);
    P.CreateKernelCall<UnicodePropertyBasis>(enumObj, TransformedBasis, CCC_Basis);
    SHOW_BIXNUM(CCC_Basis);

    StreamSet * const u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(TransformedBasis, u8index);
    SHOW_STREAM(u8index);

    StreamSet * const CCC_Spans = P.CreateStreamSet(enumObj->GetEnumerationBasisSets().size(), 1);
    P.CreateKernelCall<U8Spans>(CCC_Basis, u8index, CCC_Spans);
    SHOW_BIXNUM(CCC_Spans);

    StreamSet * const CCC_NonZero = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(CCC_Spans, 0, CCC_NonZero);
    SHOW_STREAM(CCC_NonZero);

    StreamSets ToSort = {CCC_Spans, TransformedBasis};
    StreamSets SortResults = BitonicSortRuns(P, 32, CCC_NonZero, ToSort);
    SHOW_BIXNUM(SortResults[0]);
    SHOW_BIXNUM(SortResults[1]);

    P.CreateKernelCall<P2SKernel>(SortResults[1], TransformedBytes);
    SHOW_BYTES(TransformedBytes);
}

static StreamSet * NFC_Bytes(PipelineBuilder & P, StreamSet * ByteStream, StreamSet * BasisBits) {
    // Focus: select the spans that may change.
    StreamSet * const WorkSelectionMask = P.CreateStreamSet(1, 1);
    DetermineNFC_WorkSpans(P, BasisBits, WorkSelectionMask);

    StreamSet * const SelectedWorkBasis = P.CreateStreamSet(8, 1);
    FilterByMask(P, WorkSelectionMask, BasisBits, SelectedWorkBasis);

    StreamSet * const NonModifiedMask = P.CreateStreamSet(1, 1);
    Invert(P, WorkSelectionMask, NonModifiedMask);
    SHOW_STREAM(NonModifiedMask);

    StreamSet * const NonModifiedBytes = P.CreateStreamSet(1, 8);
    FilterByMask(P, NonModifiedMask, ByteStream, NonModifiedBytes);

    // Working space for the selected spans.
    StreamSet * const WorkingInsertionBixNum = P.CreateStreamSet(4, 1);
    P.CreateKernelCall<NFC_Initial_Insertion>(SelectedWorkBasis, WorkingInsertionBixNum);
    SHOW_BIXNUM(WorkingInsertionBixNum);

    StreamSet * const WorkingExpansionMask = P.CreateStreamSet(1, 1);
    InsertionSpreadMask(P, WorkingInsertionBixNum, WorkingExpansionMask, kernel::InsertPosition::After);
    SHOW_STREAM(WorkingExpansionMask);

    StreamSet * const WorkingBasis = P.CreateStreamSet(8, 1);
    SpreadByMask(P, WorkingExpansionMask, SelectedWorkBasis, WorkingBasis);

    StreamSet * const ValidWorkMask = P.CreateStreamSet(1, 1);
    StreamSet * const TransformedBytes = P.CreateStreamSet(1, 8);
    NFC_U8_logic(P, WorkingExpansionMask, WorkingBasis, ValidWorkMask, TransformedBytes);
    SHOW_STREAM(ValidWorkMask);

    // Merge the composed spans with the unmodified bytes.
    StreamSet * const FinalInsertionBixNum = P.CreateStreamSet(4, 1);
    P.CreateKernelCall<NFC_Initial_Insertion>(BasisBits, FinalInsertionBixNum, WorkSelectionMask);
    SHOW_BIXNUM(FinalInsertionBixNum);

    StreamSet * const SourceExpansionMask = P.CreateStreamSet(1, 1);
    InsertionSpreadMask(P, FinalInsertionBixNum, SourceExpansionMask, kernel::InsertPosition::After);
    SHOW_STREAM(SourceExpansionMask);

    StreamSet * const ExpandedWorkMask = P.CreateStreamSet(1, 1);
    ExpandFilter(P, SourceExpansionMask, WorkSelectionMask, ExpandedWorkMask);
    SHOW_STREAM(ExpandedWorkMask);

    StreamSet * const FinalWorkSelectionMask = P.CreateStreamSet(1, 1);
    ExpandFilter(P, ExpandedWorkMask, ValidWorkMask, FinalWorkSelectionMask);
    SHOW_STREAM(FinalWorkSelectionMask);

    StreamSet * const FinalWorkPlacementMask = P.CreateStreamSet(1, 1);
    FilterByMask(P, FinalWorkSelectionMask, ExpandedWorkMask, FinalWorkPlacementMask);

    StreamSet * const OutputBytes = P.CreateStreamSet(1, 8);
    MergeByMask(P, FinalWorkPlacementMask, TransformedBytes, NonModifiedBytes, OutputBytes);
    return OutputBytes;
}

//  Full case mapping by a string override property (lc, uc or tc), as in
//  the U21 pipeline of the xch tool.
static StreamSet * CaseMapBasis(PipelineBuilder & P, UCD::property_t prop, StreamSet * BasisBits) {
    UCD::StringOverridePropertyObject * propObj = cast<UCD::StringOverridePropertyObject>(UCD::getPropertyObject(prop));
    std::vector<unicode::BitTranslationSets> xfrms;
    for (unsigned i = 0; i < propObj->MaxUnicodeInsertLength(); i++) {
        xfrms.push_back(propObj->GetBitTransformSets(i));
    }
    unicode::BitTranslationSets insertion_bixnum = propObj->GetUnicodeInsertLengthBixNumSets();

    StreamSet * const U21 = U21_Of(P, BasisBits);
    StreamSet * const MappedU21 = U21_CharToShortStringPipeline(P, insertion_bixnum, xfrms, U21);
    return UTF8_Of(P, MappedU21);
}

typedef void (*TransformFunctionType)(uint32_t fd);

static TransformFunctionType generatePipeline(CPUDriver & driver, Transform t) {
    auto P = CreatePipeline(driver, Input<uint32_t>("inputFileDescriptor"));
    Scalar * const fileDescriptor = P.getInputScalar("inputFileDescriptor");

    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    SHOW_BYTES(ByteStream);

    StreamSet * const BasisBits = BasisOf(P, ByteStream);

    NFD_PipelineBuilder NFD_builder(P);
    StreamSet * OutputBytes = nullptr;
    switch (t) {
        case Transform::NFD:
            OutputBytes = BytesOf(P, NFD_Basis(P, NFD_builder, BasisBits));
            break;
        case Transform::NFKD:
            OutputBytes = BytesOf(P, NFKD_Basis(P, NFD_builder, BasisBits));
            break;
        case Transform::NFC:
            OutputBytes = NFC_Bytes(P, ByteStream, BasisBits);
            break;
        case Transform::NFKC: {
            StreamSet * const NFKD_Basis_ = NFKD_Basis(P, NFD_builder, BasisBits);
            OutputBytes = NFC_Bytes(P, BytesOf(P, NFKD_Basis_), NFKD_Basis_);
            break;
        }
        case Transform::Lower:
            OutputBytes = BytesOf(P, CaseMapBasis(P, UCD::lc, BasisBits));
            break;
        case Transform::Upper:
            OutputBytes = BytesOf(P, CaseMapBasis(P, UCD::uc, BasisBits));
            break;
        case Transform::Title:
            OutputBytes = BytesOf(P, CaseMapBasis(P, UCD::tc, BasisBits));
            break;
    }
    P.CreateKernelCall<StdOutKernel>(OutputBytes);
    return P.compile();
}

static void listTransforms(const ldml::TransformRegistry & registry) {
    for (const ldml::TransformEntry & e : registry.entries()) {
        llvm::outs() << e.canonicalName;
        for (const std::string & a : e.aliases) llvm::outs() << "  " << a;
        if (e.isBuiltIn()) {
            llvm::outs() << "  [built-in]";
        } else {
            llvm::outs() << "  [" << e.file << (e.direction == ldml::TransformDirection::Backward ? ", backward" : "") << "]";
        }
        if (e.internal) llvm::outs() << "  [internal]";
        llvm::outs() << "\n";
    }
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&TconvOptions, &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});

    ldml::TransformRegistry registry;
    const std::string dir = TransformsDir.empty() ? ldml::defaultTransformDirectory() : TransformsDir;
    std::vector<std::string> warnings;
    if (!registry.loadDirectory(dir, warnings)) {
        llvm::errs() << "Warning: cannot read transform directory " << dir << "; only built-in transforms are known.\n";
    }
    if (ShowWarnings) {
        for (const std::string & w : warnings) llvm::errs() << "Warning: " << w << "\n";
    }
    if (ListTransforms) {
        listTransforms(registry);
        return 0;
    }
    if (TransformName.empty() || InputFile.empty()) {
        llvm::errs() << "Usage: " << argv[0] << " [options] <transform name> <input file>\n"
                     << "       " << argv[0] << " [options] --list-transforms\n";
        return 1;
    }

    const ldml::TransformEntry * entry = registry.lookup(TransformName);
    if (entry == nullptr) {
        llvm::errs() << "Error: unknown transform " << TransformName << "\n";
        return 1;
    }
    auto impl = std::find_if(implementedTransforms.begin(), implementedTransforms.end(),
                             [&](const std::pair<std::string, Transform> & p) {return p.first == entry->canonicalName;});
    if (impl == implementedTransforms.end()) {
        llvm::errs() << "Transform " << TransformName << " (canonical name " << entry->canonicalName
                     << (entry->isBuiltIn() ? ", built-in" : ", defined in " + entry->file)
                     << ") is known but not yet implemented.\n";
        return 2;
    }

    const int fd = open(InputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << InputFile << " for processing.\n";
        return 1;
    }
    CPUDriver driver("tconv");
    TransformFunctionType fn = generatePipeline(driver, impl->second);
    fn(fd);
    close(fd);
    return 0;
}

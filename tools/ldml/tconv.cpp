/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  tconv: apply an LDML transform (UTS #35 Part 2) to a UTF-8 file.
//
//  tconv [options] <transform name> <input file>
//      Write the transformed file to standard output.
//  tconv [options] --rules-file=<rules file> <input file>
//      Parse and compile the LDML transform rules (plain tRule text, as
//      ldml_trules reads without --xml) of <rules file>, as the forward
//      rules of an ad hoc transform, and apply it in place of a named
//      transform.
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
//      Any-Hex/Unicode     each character as U+ and its codepoint in
//                          (uppercase) hexadecimal, of at least 4 digits
//      Any-Hex/Java        each character first decoded to UTF-16 and then
//                          each code unit as \u and 4 hex digits
//      Any-Hex             == Any-Hex/Java
//      Any-Hex/C           \uXXXX for codepoints < 0x10000, otherwise \UXXXXXXXX
//      Transforms defined by rules (see TransformAnalyzer), with global
//      filters (:: [set] ;) and filtered transform rules (:: [set] ID ;):
//      the filter is computed once, as a mask of the positions of the text
//      where the transform may change characters (see RulePipelineBuilder).
//  Any other known name is reported with its canonical name as not yet
//  implemented.

#include <fcntl.h>
#include <unistd.h>
#include <thread>
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
#include <kernel/streamutils/stream_shift.h>
#include <kernel/streamutils/run_index.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/unicode/char_replacement.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/normalization/normalization.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/util/hex_convert.h>
#include <ldml/transform_registry.h>
#include <kernel/re/regexp_engine.h>
#include <re/adt/adt.h>
#include <re/cc/cc_kernel.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <ucd/utf/transchar.h>
#include <ldml/transform_plan.h>
#include <ldml/transform_rules_parser.h>
#include <ldml/transform_rules_printer.h>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <re/adt/re_name.h>
#include <re/unicode/resolve_properties.h>
#include <re/unicode/boundaries.h>
#include <toolchain/toolchain.h>
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/algo/normalization.h>
#include <ucd/data/PropertyObjectTable.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

static cl::OptionCategory TconvOptions("Transform Options", "LDML transform options.");
static cl::opt<std::string> TransformName(cl::Positional, cl::desc("<transform name>"), cl::cat(TconvOptions));
static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input file>"), cl::cat(TconvOptions));
static cl::opt<std::string> TransformsDir("transforms-dir", cl::desc("Directory of CLDR transform files (default ~/cldr/common/transforms)"),
                                          cl::init(""), cl::cat(TconvOptions));
static cl::opt<std::string> RulesFile("rules-file", cl::desc("File of LDML transform rules (plain tRule text) to parse and compile, applied "
                                                              "in place of a named transform"),
                                      cl::init(""), cl::cat(TconvOptions));
static cl::opt<bool> ListTransforms("list-transforms", cl::desc("List the known transform names and exit"),
                                    cl::init(false), cl::cat(TconvOptions));
static cl::opt<unsigned> ConvertedContextRounds("converted-context-rounds",
    cl::desc("Rounds of matching before contexts against the converted text, which the conversions of a group may change "
             "(exact for chains of up to this many such rules; 0 matches them against the text itself)"),
    cl::init(8), cl::cat(TconvOptions));
static cl::opt<unsigned> DeferredRounds("deferred-rule-rounds",
    cl::desc("Rounds of applying the rules deferred after the BeforeCaptureCopy rules of a group, to the converted text "
             "(exact for chains of up to this many triggers whose before contexts reach back to each other; at most 6)"),
    cl::init(4), cl::cat(TconvOptions));
static cl::opt<bool> ShowPlan("plan", cl::desc("Report how the transform would be implemented and exit"),
                              cl::init(false), cl::cat(TconvOptions));
//  The classes of the bits of multiplexed mapping numbers (see FusedSubgroupPlan)
//  are computed by kernels of their own, as the cost of compiling a kernel of
//  classes of scattered characters grows faster than their number (for Hani-Latn,
//  11 bits: 140s with the other classes, 33s in 3 kernels, 38s in 11).
static cl::opt<unsigned> MuxClassKernels("mux-class-kernels",
    cl::desc("The number of kernels computing the classes of multiplexed mapping numbers (0: with the other classes)"),
    cl::init(3), cl::cat(TconvOptions), cl::Hidden);
static cl::opt<bool> Lines("lines", cl::desc("Transform each line of the input as a text of its own (as the cases of the CLDR test data)"),
                           cl::init(false), cl::cat(TconvOptions));
static cl::opt<bool> DisableCaptureSimplification("disable-capture-simplification",
    cl::desc("Do not simplify the captures of the rules (unreferenced capture removal, trivial and nullable capture elimination)"),
    cl::init(false), cl::cat(TconvOptions));
static cl::opt<bool> ShowWarnings("show-transform-warnings", cl::desc("Report problems found in the transform files"),
                                  cl::init(false), cl::cat(TconvOptions));

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

enum class Transform {NFD, NFKD, NFC, NFKC, FCD, FCC, Lower, Upper, Title, CaseFold, HexJava, HexUnicode, HexC, HexXML, HexXML10, HexPerl, HexDecode, Remove};

static const std::vector<std::pair<std::string, Transform>> implementedTransforms = {
    {"Any-NFD", Transform::NFD}, {"Any-NFKD", Transform::NFKD},
    {"Any-NFC", Transform::NFC}, {"Any-NFKC", Transform::NFKC}, {"Any-FCD", Transform::FCD}, {"Any-FCC", Transform::FCC},
    {"Any-Lower", Transform::Lower}, {"Any-Upper", Transform::Upper}, {"Any-Title", Transform::Title}, {"Any-CaseFold", Transform::CaseFold},
    {"Any-Hex", Transform::HexJava},{"Any-Hex/Java", Transform::HexJava},
    {"Any-Hex/Unicode", Transform::HexUnicode}, {"Any-Hex/C", Transform::HexC},
    {"Any-Hex/XML", Transform::HexXML}, {"Any-Hex/XML10", Transform::HexXML10},  {"Any-Hex/Perl", Transform::HexPerl},
    {"Hex-Any", Transform::HexDecode}, {"Hex-Any/Java", Transform::HexDecode}, {"Hex-Any/C", Transform::HexDecode},
    {"Hex-Any/Perl", Transform::HexDecode}, {"Hex-Any/Unicode", Transform::HexDecode},
    {"Hex-Any/XML", Transform::HexDecode}, {"Hex-Any/XML10", Transform::HexDecode},
    {"Any-Remove", Transform::Remove},
};

//  The forms of escapes decoded by each Hex-Any transform (see kernel::HexEscapeForm).
static unsigned hexDecodeForms(const std::string & name) {
    if (name == "Hex-Any") return HexEscapeAll;
    if (name == "Hex-Any/Java") return HexEscapeJava;
    if (name == "Hex-Any/C") return HexEscapeC;
    if (name == "Hex-Any/Perl") return HexEscapePerl;
    if (name == "Hex-Any/Unicode") return HexEscapeUnicode;
    if (name == "Hex-Any/XML") return HexEscapeXML;
    if (name == "Hex-Any/XML10") return HexEscapeXML10;
    return 0;
}

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

    // Canonical ordering is a sort of runs of nonzero ccc characters.  Sort
    // in the 21-bit representation, one position per character, so that the
    // sort never separates the bytes of a UTF-8 sequence.
    StreamSet * const U21_Basis = U21_Of(P, TransformedBasis);

    UCD::EnumeratedPropertyObject * enumObj = llvm::cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    StreamSet * const CCC_Basis = P.CreateStreamSet(enumObj->GetEnumerationBasisSets().size(), 1);
    P.CreateKernelCall<UnicodePropertyBasis>(enumObj, U21_Basis, CCC_Basis);
    SHOW_BIXNUM(CCC_Basis);

    StreamSet * const CCC_NonZero = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(CCC_Basis, 0, CCC_NonZero);
    SHOW_STREAM(CCC_NonZero);

    StreamSet * const Sorted_U21 = P.CreateStreamSet(21, 1);
    SortRuns(P, CCC_NonZero, CCC_Basis, U21_Basis, Sorted_U21);
    SHOW_BIXNUM(Sorted_U21);

    P.CreateKernelCall<P2SKernel>(UTF8_Of(P, Sorted_U21), TransformedBytes);
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
        case Transform::CaseFold:
            OutputBytes = BytesOf(P, CaseMapBasis(P, UCD::cf, BasisBits));
            break;
        case Transform::FCD:
        case Transform::FCC:
        case Transform::Title:
        case Transform::HexJava:
        case Transform::HexUnicode:
        case Transform::HexC:
        case Transform::HexXML:
        case Transform::HexXML10:
        case Transform::HexPerl:
        case Transform::HexDecode:
        case Transform::Remove:
            llvm::report_fatal_error("tconv: Any-FCD, Any-FCC, Any-Title, Any-Hex and Hex-Any (/Java, /Unicode, /C, /XML, /XML10, /Perl) and Any-Remove are implemented by the rule pipeline");
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

//  Analysis of the transforms defined by rules: the rules of the transform in
//  the direction of the registry entry (as forward rules) are rewritten by
//  trivial and nullable capture elimination and order disambiguation, and
//  planned (see ldml/transform_plan.h).  A transform is implementable if its
//  conversion rules are implementable and every transform its transform rules
//  invoke is implementable (with or without filters).

struct TransformAnalysis {
    bool implementable = false;
    ldml::TransformPlan plan;
    std::vector<std::string> problems;
};

class TransformAnalyzer {
public:
    explicit TransformAnalyzer(const ldml::TransformRegistry & registry) : mRegistry(registry) {}
    const TransformAnalysis & analyze(const ldml::TransformEntry * entry);
    //  Analyze rules not tied to a registry entry (e.g. from --rules-file), as
    //  the forward rules of an ad hoc transform.
    TransformAnalysis analyzeRulesFile(const std::vector<ldml::Rule *> & parsed);
private:
    //  The common analysis of a set of parsed rules (trivial and nullable
    //  capture elimination, order disambiguation, planning, and checking the
    //  transforms the plan's steps invoke), given the rules to extract as
    //  forward rules.
    TransformAnalysis analyzeRules(const std::vector<ldml::Rule *> & parsed, ldml::TransformDirection direction);
    //  The value of a transform called by a rule (&T($k)) on the characters of a
    //  set (see ldml::ExpandFunctionCalls).
    bool evaluateFunction(const ldml::TransformID & transform, const UCD::UnicodeSet & chars,
                          std::vector<std::pair<UCD::UnicodeSet, std::u32string>> & groups,
                          UCD::UnicodeSet & unchanged, std::string & reason);
    const ldml::TransformRegistry & mRegistry;
    std::map<std::string, TransformAnalysis> mResults;
    std::set<std::string> mInProgress;
};

static std::string readFile(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

static bool isImplementedBuiltIn(const std::string & canonicalName) {
    if (canonicalName == "Any-Null") return true;
    return std::any_of(implementedTransforms.begin(), implementedTransforms.end(),
                       [&](const std::pair<std::string, Transform> & p) {return p.first == canonicalName;});
}

static std::string printCodepoints(const std::u32string & s) {
    std::string out;
    for (char32_t c : s) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%sU+%04X", out.empty() ? "" : " ", static_cast<unsigned>(c));
        out += buf;
    }
    return out.empty() ? "(empty)" : out;
}

const TransformAnalysis & TransformAnalyzer::analyze(const ldml::TransformEntry * entry) {
    const std::string & name = entry->canonicalName;
    auto f = mResults.find(name);
    if (f != mResults.end()) return f->second;
    TransformAnalysis a;
    if (mInProgress.count(name)) {
        a.problems.push_back("recursive reference to " + name);
        return mResults.emplace(name + " (recursive)", std::move(a)).first->second;
    }
    if (entry->isBuiltIn()) {
        a.implementable = isImplementedBuiltIn(name);
        if (!a.implementable) a.problems.push_back("built-in transform " + name + " is not implemented");
        return mResults.emplace(name, std::move(a)).first->second;
    }
    mInProgress.insert(name);
    try {
        const std::vector<ldml::Rule *> parsed = ldml::parseTransformRules(ldml::extractTRules(readFile(entry->file)));
        a = analyzeRules(parsed, entry->direction);
    } catch (const std::exception & e) {
        a.problems.push_back(std::string("rules not loaded: ") + e.what());
        a.implementable = false;
    }
    mInProgress.erase(name);
    return mResults.emplace(name, std::move(a)).first->second;
}

//  The value of a transform, as planned, on a single character: through its
//  conversion groups, each mapping the character by a single-character rule
//  without contexts, or leaving it unchanged; false (with the reason) if a rule
//  with contexts may apply to the character, or other rules or steps, or a step
//  applies to the text of several characters a mapping made.
static bool evaluatePlan(const ldml::TransformPlan & plan, const ldml::TransformRegistry & registry, UCD::codepoint_t cp,
                         std::u32string & out, std::string & reason) {
    std::u32string s(1, static_cast<char32_t>(cp));
    if (plan.filter && !plan.filterSet.contains(cp)) {
        out = s;
        return true;
    }
    for (const ldml::TransformStep & step : plan.steps) {
        if (s.empty()) break;
        if (step.kind == ldml::TransformStep::Kind::Transform) {
            const ldml::TransformEntry * used = registry.lookup(step.transform.getText());
            if (used && used->canonicalName == "Any-Null") continue;
            if (used && used->canonicalName == "Any-Remove" && s.size() == 1) {
                if (!step.filter || step.filterSet.contains(s[0])) s.clear();
                continue;
            }
            reason = "it invokes " + step.transform.getText();
            return false;
        }
        if (s.size() != 1) {
            reason = "a conversion group applies to a text of several characters";
            return false;
        }
        const UCD::codepoint_t c = s[0];
        if (!step.stringRules.empty() || !step.captureReplacements.empty() || !step.insertions.empty() || !step.deferred.empty()) {
            reason = "it has rules other than single-character rules";
            return false;
        }
        const ldml::CharMapSubgroup * applying = nullptr;
        for (const ldml::CharMapSubgroup & g : step.subgroups) {
            if (!g.characters().contains(c)) continue;
            if (g.before || g.after) {
                reason = "a single-character rule with contexts may apply";
                return false;
            }
            if (applying == nullptr) applying = &g;
        }
        if (applying == nullptr) continue;
        for (const ldml::CharMapping & m : applying->mappings) {
            if (m.chars.contains(c)) {
                s = m.replacement;
                break;
            }
        }
    }
    out = s;
    return true;
}

bool TransformAnalyzer::evaluateFunction(const ldml::TransformID & transform, const UCD::UnicodeSet & chars,
                                         std::vector<std::pair<UCD::UnicodeSet, std::u32string>> & groups,
                                         UCD::UnicodeSet & unchanged, std::string & reason) {
    const ldml::TransformEntry * const entry = mRegistry.lookup(transform.getText());
    if (entry == nullptr) {
        reason = "unknown transform " + transform.getText();
        return false;
    }
    const std::string & name = entry->canonicalName;
    std::map<std::u32string, UCD::UnicodeSet> byValue;
    auto add = [&](UCD::codepoint_t cp, const std::u32string & v) {
        if (v.size() == 1 && v[0] == cp) unchanged.insert(cp); else byValue[v].insert(cp);
    };
    if (name == "Any-Lower" || name == "Any-Upper" || name == "Any-Title") {
        //  The full case mappings (of a character alone: no Final_Sigma context).
        const UCD::property_t prop = name == "Any-Lower" ? UCD::lc : name == "Any-Upper" ? UCD::uc : UCD::tc;
        auto * const p = cast<UCD::StringOverridePropertyObject>(UCD::getPropertyObject(prop));
        const UCD::UnicodeSet same = chars & p->GetReflexiveSet();
        unchanged = unchanged + same;
        for (const auto & range : chars - same) {
            for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) add(cp, p->GetU32StringValue(cp));
        }
    } else {
        if (chars.count() > 4096) {
            reason = "the capture has too many characters to evaluate " + name + " on";
            return false;
        }
        const TransformAnalysis & a = analyze(entry);
        if (!a.implementable) {
            reason = name + " is not implementable";
            return false;
        }
        for (const auto & range : chars) {
            for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) {
                std::u32string v;
                std::string why;
                if (!evaluatePlan(a.plan, mRegistry, cp, v, why)) {
                    reason = name + " is not evaluated on single characters: " + why;
                    return false;
                }
                add(cp, v);
            }
        }
    }
    for (const auto & kv : byValue) groups.emplace_back(kv.second, kv.first);
    return true;
}

static void captureContextProblems(const ldml::TransformStep & step, std::vector<std::string> & problems);
static void insertionContextProblems(const ldml::TransformStep & step, std::vector<std::string> & problems);

TransformAnalysis TransformAnalyzer::analyzeRules(const std::vector<ldml::Rule *> & parsed, ldml::TransformDirection direction) {
    TransformAnalysis a;
    try {
        std::vector<ldml::Rule *> rules = direction == ldml::TransformDirection::Forward
                                          ? ldml::ExtractForwardRules(parsed) : ldml::ExtractReverseBackwardRules(parsed);
        //  (Before revisits are composed: a result calling a function may have
        //  text to revisit.)
        ldml::FunctionCallStats cstats;
        rules = ldml::ExpandFunctionCalls(rules, [&](const ldml::TransformID & t, const UCD::UnicodeSet & chars,
                                                     std::vector<std::pair<UCD::UnicodeSet, std::u32string>> & groups,
                                                     UCD::UnicodeSet & unchanged, std::string & reason) {
            return evaluateFunction(t, chars, groups, unchanged, reason);
        }, &cstats);
        ldml::RevisitStats rstats;
        rules = ldml::ComposeRevisits(rules, &rstats);
        //  Capture simplification.  (Rules whose text is only a capture, or
        //  continues after it, are kept as MultipleCaptureMultipleReplacement
        //  rules, rather than insertion rules or rules with the capture as a
        //  before context, other than (X) X+.)
        if (!DisableCaptureSimplification) {
            rules = ldml::RemoveUnreferencedCaptures(rules);
            rules = ldml::TrivialCaptureElimination(rules, nullptr, false, false);
            rules = ldml::NullableCaptureElimination(rules);
        }
        ldml::FiniteTextStats fstats;
        rules = ldml::ExpandFiniteTexts(rules, &fstats);
        ldml::DisambiguationStats dstats;
        rules = ldml::DisambiguateOrder(rules, &dstats);
        a.plan = ldml::planTransform(rules);
        //  The reasons the rules with text to revisit were not composed: these
        //  rules remain, in order, as the rules with that problem.
        //  Likewise for the rules whose texts were not expanded.
        size_t notComposed = 0;
        auto endsWith = [](const std::string & s, const std::string & e) {
            return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
        };
        for (std::string & problem : a.plan.problems) {
            if (endsWith(problem, "(the result is not a fixed string)")) {
                //  The function call not expanded (the rule is unchanged).
                for (const auto & n : cstats.notExpanded) {
                    if (problem.compare(0, ldml::printRule(n.first).size(), ldml::printRule(n.first)) == 0) {
                        problem += " (function call not expanded: " + n.second + ")";
                        break;
                    }
                }
            }
            if (endsWith(problem, "(the result has text to revisit)") && notComposed < rstats.notComposed.size()) {
                problem += " (not composed: " + rstats.notComposed[notComposed++].second + ")";
            } else if (endsWith(problem, "(the text to replace is not a fixed-length sequence of characters)")) {
                //  The rule not expanded with the same text and result (its contexts
                //  may have been changed since).
                for (const auto & n : fstats.notExpanded) {
                    const ldml::ConversionRule * c = llvm::cast<ldml::ConversionRule>(n.first);
                    const std::string text = ldml::printPattern(c->getSourceSide(ldml::Direction::Forward)->getText());
                    const std::string result = ldml::printRuleSide(c->getResultSide(ldml::Direction::Forward));
                    const size_t arrow = problem.find(" → ");
                    if (arrow != std::string::npos && problem.find(text) < arrow && problem.compare(arrow + 5, result.size(), result) == 0) {
                        problem += " (not expanded: " + n.second + ")";
                        break;
                    }
                }
            }
        }
        for (const ldml::TransformStep & step : a.plan.steps) {
            captureContextProblems(step, a.plan.problems);
            insertionContextProblems(step, a.plan.problems);
        }
        a.problems = a.plan.problems;
        //  The bit changes of the subgroups of a group are combined, assuming that
        //  no two rules of a group may apply at the same position.
        if (dstats.overlapsAfter > 0) {
            a.problems.push_back(std::to_string(dstats.overlapsAfter) + " pairs of rules remain order dependent");
        }
        for (const ldml::TransformStep & step : a.plan.steps) {
            if (step.kind != ldml::TransformStep::Kind::Transform) continue;
            const std::string id = step.transform.getText();
            const ldml::TransformEntry * used = mRegistry.lookup(id);
            if (used == nullptr) {
                a.problems.push_back("unknown transform " + id);
            } else if (!analyze(used).implementable) {
                a.problems.push_back("unimplemented transform " + used->canonicalName);
            }
        }
    } catch (const std::exception & e) {
        a.problems.push_back(std::string("rules not loaded: ") + e.what());
    }
    a.implementable = a.problems.empty();
    return a;
}

TransformAnalysis TransformAnalyzer::analyzeRulesFile(const std::vector<ldml::Rule *> & parsed) {
    return analyzeRules(parsed, ldml::TransformDirection::Forward);
}

//  A set of codepoints, as its first few ranges (U+0041, U+0061-U+007A, ...).
static std::string printRanges(const UCD::UnicodeSet & set) {
    std::string result;
    unsigned shown = 0;
    for (const auto & range : set) {
        if (shown++ == 3) {result += " ..."; break;}
        char buf[32];
        if (range.first == range.second) {
            snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(range.first));
        } else {
            snprintf(buf, sizeof(buf), "U+%04X-U+%04X", static_cast<unsigned>(range.first), static_cast<unsigned>(range.second));
        }
        result += (shown == 1 ? "" : " ") + std::string(buf);
    }
    return set.count() == 1 ? result : "[" + result + "]";
}

static void printBeforeContextNotes(const ldml::TransformStep & step);

static void printPlan(const std::string & label, const TransformAnalysis & a) {
    llvm::outs() << label << ": " << (a.implementable ? "implementable" : "not implementable") << "\n";
    for (const std::string & p : a.problems) {
        std::string shown = p;
        if (p.size() > 160) {
            size_t cut = 150;
            while (cut > 0 && (static_cast<unsigned char>(p[cut]) & 0xC0) == 0x80) cut--;   // a UTF-8 boundary
            const size_t reason = p.rfind("  (");
            shown = p.substr(0, cut) + " ... " + (reason == std::string::npos ? "" : p.substr(reason));
        }
        llvm::outs() << "  problem: " << shown << "\n";
    }
    if (a.plan.filter) llvm::outs() << "  filter " << ldml::printUnicodeSet(a.plan.filter) << "\n";
    for (const ldml::TransformStep & step : a.plan.steps) {
        if (step.kind == ldml::TransformStep::Kind::Transform) {
            llvm::outs() << "  transform " << (step.filter ? ldml::printUnicodeSet(step.filter) + " " : "")
                         << step.transform.getText() << "\n";
            continue;
        }
        llvm::outs() << "  conversion group: " << step.subgroups.size() << " subgroups, "
                     << step.stringRules.size() << " string rules\n";
        //  The overlaps of the string rules (see ldml::StringRuleOverlaps).
        const ldml::StringRuleOverlaps overlaps = ldml::analyzeStringRuleOverlaps(step);
        if (!overlaps.pairs.empty()) {
            llvm::outs() << "    string rule overlaps: " << overlaps.pairs.size() << " pairs (" << overlaps.oneWay << " one-way, "
                         << overlaps.twoWay << " two-way, " << overlaps.self << " self); ";
            if (overlaps.singlePass()) {
                llvm::outs() << "single pass in " << std::max(overlaps.singlePassStages(), 1u) << " stages"
                             << (overlaps.self ? " (with two-character self-overlaps)" : "") << "\n";
            } else {
                if (overlaps.twoWay > 0) {
                    llvm::outs() << "selected sequentially: two-way overlaps";
                } else if (overlaps.acyclicApartFromSelf) {
                    llvm::outs() << "selected sequentially: self-overlaps (otherwise single pass in "
                                 << std::max(overlaps.stagesApartFromSelf, 1u) << " stages)";
                } else {
                    llvm::outs() << "selected sequentially: cycle of " << overlaps.cycle.size() << " rules";
                }
                llvm::outs() << "; " << overlaps.cyclicComponents << " cyclic components (" << overlaps.pairComponents
                             << " of 2-character rules, largest " << overlaps.largestComponent << " rules)"
                             << (overlaps.pairSelectable ? ": pair selectable" : "") << "\n";
            }
            for (size_t i = 0; i < overlaps.cycle.size(); i++) {
                llvm::outs() << "      cycle: " << ldml::printRule(step.stringRules[overlaps.cycle[i]].rule) << "\n";
            }
            for (const ldml::StringRuleOverlaps::Pair & pair : overlaps.pairs) {
                const std::string outer = ldml::printRule(step.stringRules[pair.outer].rule);
                const std::string inner = ldml::printRule(step.stringRules[pair.inner].rule);
                switch (pair.kind) {
                    case ldml::StringRuleOverlaps::Kind::OneWay:
                        llvm::outs() << "      overlap one-way: " << outer << "  →  " << inner << "\n"; break;
                    case ldml::StringRuleOverlaps::Kind::TwoWay:
                        llvm::outs() << "      overlap two-way: " << outer << "  ↔  " << inner << "\n"; break;
                    case ldml::StringRuleOverlaps::Kind::Self:
                        llvm::outs() << "      overlap self: " << outer << "\n"; break;
                }
            }
        }
        printBeforeContextNotes(step);
        for (const ldml::StringRule & sr : step.stringRules) {
            llvm::outs() << "    string rule: " << ldml::printRule(sr.rule) << "\n";
        }
        if (step.jointSelection) {
            llvm::outs() << "    string rules and MultipleCaptureMultipleReplacement rules selected together\n";
        }
        for (const ldml::MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
            llvm::outs() << "    " << (set.captures.size() == 1 ? "SingleCaptureDoubleReplacement" : "MultipleCaptureMultipleReplacement")
                         << " rules, matched together (" << set.rules.size() << ", starts by "
                         << (set.startsByFirst ? "their first characters" : set.singleCharacter ? "their ends (one character)"
                             : set.exactStarts ? "the engine (all starts)" : "the ends of matches") << ")"
                         << (set.reversed ? ", captures reversed (SwapBack " + std::to_string(set.swapLength) + ", " + std::to_string(set.swapDistance) + ")" : "")
                         << (set.moved ? ", captures " + std::to_string(set.moveFirst + 1) + " to " + std::to_string(set.moveCapture)
                                         + " moved after capture " + std::to_string(set.moveCapture + 1) : "")
                         << ":\n";
            for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) llvm::outs() << "      " << ldml::printRule(r.rule) << "\n";
        }
        for (const ldml::InsertionRule & ins : step.insertions) {
            llvm::outs() << "    insertion rule: " << ldml::printRule(ins.rule) << "\n";
        }
        if (!step.deferred.empty()) {
            llvm::outs() << "    deferred rules (" << step.deferred.size() << ", " << step.copyCaptures.size()
                         << " captures copied, triggers " << printRanges(step.deferredTriggers) << "):\n";
            for (const ldml::DeferredRule & d : step.deferred) {
                llvm::outs() << "      " << (d.selfReproducing ? "self-reproducing copy: " : d.capture >= 0 ? "copy: " : "")
                             << ldml::printRule(d.rule) << "\n";
            }
        }
        for (const ldml::CharMapSubgroup & s : step.subgroups) {
            llvm::outs() << "    [" << s.contextKey << "] " << s.rules << " rules, " << s.characters().count() << " characters";
            unsigned shown = 0;
            for (const ldml::CharMapping & m : s.mappings) {
                if (shown++ == 3) {llvm::outs() << " ..."; break;}
                llvm::outs() << (shown == 1 ? ": " : ", ") << printRanges(m.chars) << " -> " << printCodepoints(m.replacement);
            }
            llvm::outs() << "\n";
        }
    }
}

//  Pipelines for transforms defined by rules.  For now, the whole text is
//  transformed in the 21-bit representation of Unicode; each implementable
//  conversion rule replaces a single character by a fixed string.
//
//  For each conversion group, each subgroup (the rules with the same contexts)
//  computes the bits to change at each position: for each of the 21 bits, the
//  characters whose replacement differs from them in that bit, restricted to the
//  positions where the contexts hold (a match of before-lookbehind, the
//  characters of the subgroup, and after-lookahead).  As the rules are
//  disambiguated, no two subgroups change the same position: their changes are
//  combined by OR, and applied to the input of the group by XOR.

//  The codepoint marking the positions to be deleted at the end of a conversion
//  group: the positions inserted for replacements (until replacement characters
//  are written there), the characters of texts beyond their replacements, and the
//  characters replaced by the empty string.  It is a surrogate, which cannot occur
//  in decoded UTF-8, so that these positions are distinguished from the characters
//  of the text (including U+0000), also when matching contexts.
static constexpr UCD::codepoint_t DeletionMark = 0xD800;

//  Filled: the 21-bit basis with the filler codepoint at the positions not in
//  the spread mask (the inserted positions, which hold 0 in the basis).
class FillInsertedPositions : public pablo::PabloKernel {
public:
    FillInsertedPositions(LLVMTypeSystemInterface & ts, StreamSet * SpreadMask, StreamSet * Basis, StreamSet * Filled)
    : PabloKernel(ts, "FillInsertedPositions" + std::to_string(DeletionMark),
                  {Binding{"SpreadMask", SpreadMask}, Binding{"Basis", Basis}}, {Binding{"Filled", Filled}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const inserted = pb.createInFile(pb.createNot(getInputStreamSet("SpreadMask")[0]));
        std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        for (unsigned b = 0; b < basis.size(); b++) {
            if ((DeletionMark >> b) & 1) basis[b] = pb.createOr(basis[b], inserted);
        }
        writeOutputStreamSet("Filled", basis);
    }
};

//  The texts inserted by insertion rules (see RulePipelineBuilder::insertionGroup):
//  Shifted[j - 1] has a stream per rule, marking the j-th position inserted after
//  each position where it applies, which gets the j-th character of its text
//  (Basis has 0 at the inserted positions).
class InsertedTexts : public pablo::PabloKernel {
public:
    InsertedTexts(LLVMTypeSystemInterface & ts, const std::vector<std::u32string> & texts,
                  const std::vector<StreamSet *> & Shifted, StreamSet * Basis, StreamSet * Result)
    : PabloKernel(ts, "InsertedTexts" + [&] {
                      std::string sig;
                      for (const std::u32string & t : texts) {
                          sig += "_";
                          for (char32_t ch : t) sig += std::to_string(static_cast<unsigned>(ch)) + ".";
                      }
                      //  A long signature is hashed (the kernel name is a file name in the object cache).
                      return sig.size() > 64 ? "_" + getStringHash(sig) : sig;
                  }(),
                  [&] {
                      Bindings inputs{Binding{"Basis", Basis}};
                      for (size_t j = 0; j < Shifted.size(); j++) inputs.emplace_back("Shifted" + std::to_string(j), Shifted[j]);
                      return inputs;
                  }(),
                  {Binding{"Result", Result}}),
      mTexts(texts), mShifts(Shifted.size()) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        for (unsigned j = 0; j < mShifts; j++) {
            const std::vector<PabloAST *> marks = getInputStreamSet("Shifted" + std::to_string(j));
            for (size_t i = 0; i < mTexts.size(); i++) {
                if (j >= mTexts[i].size()) continue;
                for (unsigned b = 0; b < basis.size(); b++) {
                    if ((mTexts[i][j] >> b) & 1) basis[b] = pb.createOr(basis[b], marks[i]);
                }
            }
        }
        writeOutputStreamSet("Result", basis);
    }
private:
    const std::vector<std::u32string> mTexts;
    const unsigned mShifts;
};

//  The mask of the positions after insertions (see ExpandFilter): Spread is the
//  mask spread by the spread mask, and each inserted position (not in the spread
//  mask) inherits the mask of the character it follows.
class InheritMask : public pablo::PabloKernel {
public:
    InheritMask(LLVMTypeSystemInterface & ts, StreamSet * SpreadMask, StreamSet * Spread, StreamSet * Inherited)
    : PabloKernel(ts, "InheritMask",
                  {Binding{"SpreadMask", SpreadMask}, Binding{"Spread", Spread}}, {Binding{"Inherited", Inherited}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const inserted = pb.createInFile(pb.createNot(getInputStreamSet("SpreadMask")[0]));
        PabloAST * const spread = getInputStreamSet("Spread")[0];
        //  The runs of inserted positions after the characters of the mask.
        PabloAST * const follow = pb.createAnd(pb.createAdvance(spread, 1), inserted);
        PabloAST * const runs = pb.createAnd(pb.createMatchStar(follow, inserted), inserted);
        writeOutputStreamSet("Inherited", std::vector<PabloAST *>{pb.createOr(spread, runs)});
    }
};

//  The positions where the single-character rules of a conversion group may
//  apply, given the matches of its string rules that apply (see TransformStep::
//  coverStringRules): not those covered by the matches, from the position after
//  the first character of a match (Spans, without Starts) to its last.
//  The first positions of the matches of a pattern, given their spans (the
//  union of their positions) and their ends (their last positions), where no
//  match may end within another: the positions of the spans after a position
//  outside them, or after the end of a match.
class MatchStartsFromEnds : public pablo::PabloKernel {
public:
    MatchStartsFromEnds(LLVMTypeSystemInterface & ts, StreamSet * Spans, StreamSet * Ends, StreamSet * Starts)
    : PabloKernel(ts, "MatchStartsFromEnds", {Binding{"Spans", Spans}, Binding{"Ends", Ends}}, {Binding{"Starts", Starts}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const spans = getInputStreamSet("Spans")[0];
        PabloAST * const ends = getInputStreamSet("Ends")[0];
        PabloAST * const continuing = pb.createAdvance(pb.createAnd(spans, pb.createNot(ends)), 1);
        writeOutputStreamSet("Starts", std::vector<PabloAST *>{pb.createAnd(spans, pb.createNot(continuing))});
    }
};

//  The positions of the spans of matches after their first positions.
class SpanContinuation : public pablo::PabloKernel {
public:
    SpanContinuation(LLVMTypeSystemInterface & ts, StreamSet * Spans, StreamSet * Starts, StreamSet * Run)
    : PabloKernel(ts, "SpanContinuation", {Binding{"Spans", Spans}, Binding{"Starts", Starts}}, {Binding{"Run", Run}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        writeOutputStreamSet("Run", std::vector<PabloAST *>{pb.createAnd(getInputStreamSet("Spans")[0],
                                                                         pb.createNot(getInputStreamSet("Starts")[0]))});
    }
};

//  Of the matches of a set, given their starts, the rest of their spans (Run),
//  their ends and the starts selected (of all sets): the selected starts of the
//  set, the spans of its selected matches, and their ends.
class SelectedSpans : public pablo::PabloKernel {
public:
    SelectedSpans(LLVMTypeSystemInterface & ts, StreamSet * Starts, StreamSet * Run, StreamSet * Ends, StreamSet * Selected,
                  StreamSet * Chosen)
    : PabloKernel(ts, "SelectedSpans", {Binding{"Starts", Starts}, Binding{"Run", Run}, Binding{"Ends", Ends},
                                        Binding{"Selected", Selected}}, {Binding{"Chosen", Chosen}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const run = getInputStreamSet("Run")[0];
        PabloAST * const starts = pb.createAnd(getInputStreamSet("Starts")[0], getInputStreamSet("Selected")[0]);
        //  (The run begins after the first position.)
        PabloAST * const rest = pb.createAnd(pb.createMatchStar(pb.createAdvance(starts, 1), run), run);
        PabloAST * const spans = pb.createOr(starts, rest);
        PabloAST * const ends = pb.createAnd(getInputStreamSet("Ends")[0], spans);
        writeOutputStreamSet("Chosen", std::vector<PabloAST *>{starts, spans, ends});
    }
};

//  Single streams collected into a stream set.
class CollectStreams : public pablo::PabloKernel {
public:
    CollectStreams(LLVMTypeSystemInterface & ts, const std::vector<StreamSet *> & In, StreamSet * Out)
    : PabloKernel(ts, "CollectStreams" + std::to_string(In.size()),
                  [&] {
                      Bindings b;
                      for (size_t i = 0; i < In.size(); i++) b.emplace_back("In" + std::to_string(i), In[i]);
                      return b;
                  }(),
                  {Binding{"Out", Out}}), mCount(In.size()) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> out;
        for (size_t i = 0; i < mCount; i++) out.push_back(getInputStreamSet("In" + std::to_string(i))[0]);
        writeOutputStreamSet("Out", out);
    }
private:
    const size_t mCount;
};

//  The streams of several stream sets, in the order given (of the pairs of the
//  index of a stream set and of a stream in it).
class MergeStreamSets : public pablo::PabloKernel {
public:
    MergeStreamSets(LLVMTypeSystemInterface & ts, const std::vector<StreamSet *> & In,
                    const std::vector<std::pair<unsigned, unsigned>> & order, StreamSet * Out)
    : PabloKernel(ts, "MergeStreamSets" + [&] {
                      std::string sig;
                      for (StreamSet * ss : In) sig += "_" + std::to_string(ss->getNumElements());
                      sig += "o";
                      for (const auto & o : order) sig += std::to_string(o.first) + "." + std::to_string(o.second) + ",";
                      return sig.size() > 64 ? "_" + getStringHash(sig) : sig;
                  }(),
                  [&] {
                      Bindings b;
                      for (size_t i = 0; i < In.size(); i++) b.emplace_back("In" + std::to_string(i), In[i]);
                      return b;
                  }(),
                  {Binding{"Out", Out}}), mOrder(order) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> out;
        for (const auto & o : mOrder) out.push_back(getInputStreamSet("In" + std::to_string(o.first))[o.second]);
        writeOutputStreamSet("Out", out);
    }
private:
    const std::vector<std::pair<unsigned, unsigned>> mOrder;
};

//  Of two streams of positions: the positions of the first only, of the second
//  only, and of both.
class CoincidentPositions : public pablo::PabloKernel {
public:
    CoincidentPositions(LLVMTypeSystemInterface & ts, StreamSet * First, StreamSet * Second, StreamSet * Split)
    : PabloKernel(ts, "CoincidentPositions", {Binding{"First", First}, Binding{"Second", Second}}, {Binding{"Split", Split}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const a = getInputStreamSet("First")[0];
        PabloAST * const b = getInputStreamSet("Second")[0];
        writeOutputStreamSet("Split", std::vector<PabloAST *>{pb.createAnd(a, pb.createNot(b)), pb.createAnd(b, pb.createNot(a)),
                                                              pb.createAnd(a, b)});
    }
};

//  Streams within a mask.
class MaskStreams : public pablo::PabloKernel {
public:
    MaskStreams(LLVMTypeSystemInterface & ts, StreamSet * In, StreamSet * Mask, StreamSet * Out)
    : PabloKernel(ts, "MaskStreams" + std::to_string(In->getNumElements()),
                  {Binding{"In", In}, Binding{"Mask", Mask}}, {Binding{"Out", Out}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const mask = getInputStreamSet("Mask")[0];
        std::vector<PabloAST *> out;
        for (PabloAST * s : getInputStreamSet("In")) out.push_back(pb.createAnd(s, mask));
        writeOutputStreamSet("Out", out);
    }
};

class UncoveredPositions : public pablo::PabloKernel {
public:
    UncoveredPositions(LLVMTypeSystemInterface & ts, StreamSet * Spans, StreamSet * Starts, StreamSet * Uncovered)
    : PabloKernel(ts, "UncoveredPositions",
                  {Binding{"Spans", Spans}, Binding{"Starts", Starts}},
                  {Binding{"Uncovered", Uncovered}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const covered = pb.createAnd(getInputStreamSet("Spans")[0], pb.createNot(getInputStreamSet("Starts")[0]));
        writeOutputStreamSet("Uncovered", std::vector<PabloAST *>{pb.createInFile(pb.createNot(covered))});
    }
};

//  The bixnum of the numbers of positions to insert after the characters, given
//  the positions of the characters after which each number of positions is to be
//  inserted (disjoint).
class InsertionCounts : public pablo::PabloKernel {
public:
    InsertionCounts(LLVMTypeSystemInterface & ts, const std::vector<unsigned> & counts,
                    const std::vector<StreamSet *> & Positions, StreamSet * BixNum)
    : PabloKernel(ts, "InsertionCounts" + [&] {
                      std::string sig = "_b" + std::to_string(BixNum->getNumElements());
                      for (unsigned k : counts) sig += "_" + std::to_string(k);
                      //  A long signature is hashed (the kernel name is a file name in the object cache).
                      return sig.size() > 64 ? "_" + getStringHash(sig) : sig;
                  }(),
                  [&] {
                      Bindings inputs;
                      for (size_t i = 0; i < Positions.size(); i++) inputs.emplace_back("Positions" + std::to_string(i), Positions[i]);
                      return inputs;
                  }(),
                  {Binding{"BixNum", BixNum}}),
      mCounts(counts), mBits(BixNum->getNumElements()) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> bits(mBits, pb.createZeroes());
        for (size_t i = 0; i < mCounts.size(); i++) {
            PabloAST * const positions = getInputStreamSet("Positions" + std::to_string(i))[0];
            for (unsigned b = 0; b < mBits; b++) {
                if ((mCounts[i] >> b) & 1) bits[b] = pb.createOr(bits[b], positions);
            }
        }
        writeOutputStreamSet("BixNum", bits);
    }
private:
    const std::vector<unsigned> mCounts;
    const unsigned mBits;
};

//  The matches that apply of the string rules of a conversion group in which
//  string rules may match within the texts of others (see ldml::TransformStep::
//  stringOrder), given the matches of each rule, in the order to apply them,
//  marked at their first positions (Firsts, a stream of a stream set for each
//  rule), as a stream set of a stream per rule (Applied): a match of a rule applies only if
//  it does not begin within an applied match of a rule preceding it (at the
//  1 to n - 1 positions after the first of a match of n characters).  Of the
//  matches of a rule of two characters that may match within its own text,
//  which overlap where they begin at consecutive positions, the first of each
//  run and every other one after it apply (those at the positions of the same
//  parity as the first), as ICU, scanning from the left, applies the leftmost of
//  overlapping matches and continues after it.  Optionally, also the positions
//  within the applied matches after their first (Inside), and their first
//  positions (First), of all the rules.
class SelectStringMatches : public pablo::PabloKernel {
public:
    struct Rule {
        unsigned length;
        std::vector<size_t> predecessors;   // indices of preceding rules
        bool selfOverlapping;
    };
    struct Source {
        StreamSet * streamSet;
        unsigned index;
    };
    SelectStringMatches(LLVMTypeSystemInterface & ts, const std::vector<Rule> & rules,
                        const std::vector<Source> & Firsts, StreamSet * Applied,
                        StreamSet * Inside, StreamSet * First)
    : SelectStringMatches(ts, rules, Firsts, sourceSets(Firsts), Applied, Inside, First) {}
protected:
    static std::vector<StreamSet *> sourceSets(const std::vector<Source> & Firsts) {
        std::vector<StreamSet *> sets;
        for (const Source & f : Firsts) {
            if (std::find(sets.begin(), sets.end(), f.streamSet) == sets.end()) sets.push_back(f.streamSet);
        }
        return sets;
    }
    SelectStringMatches(LLVMTypeSystemInterface & ts, const std::vector<Rule> & rules,
                        const std::vector<Source> & Firsts, const std::vector<StreamSet *> & sets, StreamSet * Applied,
                        StreamSet * Inside, StreamSet * First)
    : PabloKernel(ts, "SelectStringMatches" + signature(rules, Firsts, sets) + (Inside ? "_spans" : ""),
                  [&] {
                      Bindings inputs;
                      for (size_t i = 0; i < sets.size(); i++) inputs.emplace_back("Firsts" + std::to_string(i), sets[i]);
                      return inputs;
                  }(),
                  [&] {
                      Bindings outputs;
                      outputs.emplace_back("Applied", Applied);
                      if (Inside) {
                          outputs.emplace_back("Inside", Inside);
                          outputs.emplace_back("First", First);
                      }
                      return outputs;
                  }()),
      mRules(rules), mSpans(Inside != nullptr),
      mFirsts([&] {
          std::vector<std::pair<unsigned, unsigned>> v;
          for (const Source & f : Firsts) {
              v.emplace_back(std::find(sets.begin(), sets.end(), f.streamSet) - sets.begin(), f.index);
          }
          return v;
      }()) {}
protected:
    //  The rules, as a hash of their lengths, predecessors and self-overlaps (the
    //  description itself may be too long for a kernel name).
    static std::string signature(const std::vector<Rule> & rules, const std::vector<Source> & Firsts,
                                 const std::vector<StreamSet *> & sets) {
        std::string sig;
        for (size_t i = 0; i < rules.size(); i++) {
            const Rule & r = rules[i];
            sig += "_" + std::to_string(r.length) + (r.selfOverlapping ? "s" : "");
            for (size_t p : r.predecessors) sig += "." + std::to_string(p);
            sig += "@" + std::to_string(std::find(sets.begin(), sets.end(), Firsts[i].streamSet) - sets.begin())
                   + ":" + std::to_string(Firsts[i].index);
        }
        for (StreamSet * ss : sets) sig += "#" + std::to_string(ss->getNumElements());
        char hex[17];
        snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(std::hash<std::string>{}(sig)));
        return "_" + std::to_string(rules.size()) + "_" + hex;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const odd = pb.createRepeat(1, pb.getInteger(0xAA, 8));
        std::vector<PabloAST *> applied;
        PabloAST * inside = pb.createZeroes();
        PabloAST * first = pb.createZeroes();
        //  The positions within the applied matches of a rule after their first.
        std::vector<PabloAST *> within;
        for (size_t i = 0; i < mRules.size(); i++) {
            const Rule & rule = mRules[i];
            PabloAST * matches = getInputStreamSet("Firsts" + std::to_string(mFirsts[i].first))[mFirsts[i].second];
            for (size_t p : rule.predecessors) matches = pb.createAnd(matches, pb.createNot(within[p]));
            if (rule.selfOverlapping) {
                PabloAST * const runFirst = pb.createAnd(matches, pb.createNot(pb.createAdvance(matches, 1)));
                PabloAST * const firstOdd = pb.createAnd(runFirst, odd);
                PabloAST * const firstEven = pb.createXor(runFirst, firstOdd);
                PabloAST * const oddSelected = pb.createAnd(pb.createMatchStar(firstOdd, matches), odd);
                PabloAST * const evenSelected = pb.createAnd(pb.createMatchStar(firstEven, matches), pb.createNot(odd));
                //  MatchStar also marks the position after each run, which is not a match.
                matches = pb.createAnd(pb.createOr(oddSelected, evenSelected), matches);
            }
            PabloAST * w = pb.createZeroes();
            for (unsigned j = 1; j < rule.length; j++) w = pb.createOr(w, pb.createAdvance(matches, j));
            within.push_back(w);
            inside = pb.createOr(inside, w);
            first = pb.createOr(first, matches);
            applied.push_back(matches);
        }
        writeOutputStreamSet("Applied", applied);
        if (mSpans) {
            writeOutputStreamSet("Inside", std::vector<PabloAST *>{inside});
            writeOutputStreamSet("First", std::vector<PabloAST *>{first});
        }
    }
private:
    const std::vector<Rule> mRules;
    const bool mSpans;
    const std::vector<std::pair<unsigned, unsigned>> mFirsts;   // the stream set and stream of each rule
};

//  The first positions of the matches of the string rules of a group, by the
//  lengths of their texts: a stream for each length, the union of the matches
//  of the rules of that length (given as for SelectStringMatches).
class MatchStartsByLength : public pablo::PabloKernel {
public:
    using Source = SelectStringMatches::Source;
    MatchStartsByLength(LLVMTypeSystemInterface & ts, const std::vector<std::vector<Source>> & byLength, StreamSet * Starts)
    : MatchStartsByLength(ts, byLength, sourceSets(byLength), Starts) {}
protected:
    static std::vector<StreamSet *> sourceSets(const std::vector<std::vector<Source>> & byLength) {
        std::vector<StreamSet *> sets;
        for (const auto & sources : byLength) {
            for (const Source & f : sources) {
                if (std::find(sets.begin(), sets.end(), f.streamSet) == sets.end()) sets.push_back(f.streamSet);
            }
        }
        return sets;
    }
    MatchStartsByLength(LLVMTypeSystemInterface & ts, const std::vector<std::vector<Source>> & byLength,
                        const std::vector<StreamSet *> & sets, StreamSet * Starts)
    : PabloKernel(ts, "MatchStartsByLength_" + [&] {
                      std::string sig;
                      for (const auto & sources : byLength) {
                          sig += "|";
                          for (const Source & f : sources) {
                              sig += std::to_string(std::find(sets.begin(), sets.end(), f.streamSet) - sets.begin()) + ":" + std::to_string(f.index) + ",";
                          }
                      }
                      for (StreamSet * ss : sets) sig += "#" + std::to_string(ss->getNumElements());
                      return getStringHash(sig);
                  }(),
                  [&] {
                      Bindings inputs;
                      for (size_t i = 0; i < sets.size(); i++) inputs.emplace_back("Firsts" + std::to_string(i), sets[i]);
                      return inputs;
                  }(),
                  {Binding{"Starts", Starts}}),
      mSources([&] {
          std::vector<std::vector<std::pair<unsigned, unsigned>>> v;
          for (const auto & sources : byLength) {
              v.emplace_back();
              for (const Source & f : sources) {
                  v.back().emplace_back(std::find(sets.begin(), sets.end(), f.streamSet) - sets.begin(), f.index);
              }
          }
          return v;
      }()) {}
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> starts;
        for (const auto & sources : mSources) {
            PabloAST * s = pb.createZeroes();
            for (const auto & f : sources) s = pb.createOr(s, getInputStreamSet("Firsts" + std::to_string(f.first))[f.second]);
            starts.push_back(pb.createInFile(s));
        }
        writeOutputStreamSet("Starts", starts);
    }
private:
    const std::vector<std::vector<std::pair<unsigned, unsigned>>> mSources;
};

//  The matches that apply, selected sequentially from the left as ICU applies
//  them: given the first positions of the matches by class (Starts, a stream
//  for each class: the matches of the texts of lengths[k], followed by a run of
//  the characters of Runs[runs[k]] if runs[k] >= 0), a match applies if it does
//  not begin within an applied match (after its first position: within its
//  text, or its run).  At most one match is expected at a position; if there
//  are several, the one of the earliest class applies.  The 64-bit words of
//  the matches are scanned in order, with the applied matches found by counting
//  trailing zeroes, and the end of a run by counting trailing ones; the
//  positions still covered by the last applied match (a number of positions,
//  then possibly a run) are carried to the next word (and stride).
class LeftmostMatches final : public MultiBlockKernel {
public:
    LeftmostMatches(LLVMTypeSystemInterface & ts, const std::vector<unsigned> & lengths, const std::vector<int> & runs,
                    StreamSet * Starts, StreamSet * Runs, StreamSet * Selected)
    : MultiBlockKernel(ts, "LeftmostMatches" + [&] {
                           std::string sig;
                           for (size_t k = 0; k < lengths.size(); k++) {
                               sig += "_" + std::to_string(lengths[k]) + (runs[k] >= 0 ? "r" + std::to_string(runs[k]) : "");
                           }
                           //  (A file name of the object cache.)
                           return sig.size() > 64 ? "_" + getStringHash(sig) : sig;
                       }(),
                       [&] {
                           Bindings inputs{Binding{"Starts", Starts}};
                           if (Runs) inputs.emplace_back("Runs", Runs);
                           return inputs;
                       }(),
                       {Binding{"Selected", Selected}}, {}, {},
                       {InternalScalar{ts.getInt64Ty(), "covered"}, InternalScalar{ts.getInt64Ty(), "run"}}),
      mLengths(lengths), mRuns(runs), mRunCount(Runs ? Runs->getNumElements() : 0) {
        setStride(ts.getBitBlockWidth());
    }
protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        const unsigned bw = b.getBitBlockWidth();
        Type * const sizeTy = b.getSizeTy();
        Type * const i64Ty = b.getInt64Ty();
        Constant * const ZERO = b.getSize(0);
        Constant * const ONE = b.getSize(1);
        Constant * const WORDS_PER_BLOCK = b.getSize(bw / 64);
        Constant * const W0 = b.getInt64(0);
        Constant * const W64 = b.getInt64(64);
        Constant * const NONE = b.getInt64(-1);
        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const blockLoop = b.CreateBasicBlock("blockLoop");
        BasicBlock * const wordLoop = b.CreateBasicBlock("wordLoop");
        BasicBlock * const selectLoop = b.CreateBasicBlock("selectLoop");
        BasicBlock * const selectOne = b.CreateBasicBlock("selectOne");
        BasicBlock * const wordDone = b.CreateBasicBlock("wordDone");
        BasicBlock * const blockDone = b.CreateBasicBlock("blockDone");
        BasicBlock * const done = b.CreateBasicBlock("done");
        Value * const covered0 = b.getScalarField("covered");
        Value * const run0 = b.CreateSub(b.getScalarField("run"), b.getInt64(1));   // stored + 1 (0: none)
        b.CreateCondBr(b.CreateICmpEQ(numOfStrides, ZERO), done, blockLoop);

        b.SetInsertPoint(blockLoop);
        PHINode * const blockNo = b.CreatePHI(sizeTy, 2);
        blockNo->addIncoming(ZERO, entry);
        PHINode * const blockCovered = b.CreatePHI(i64Ty, 2);
        blockCovered->addIncoming(covered0, entry);
        PHINode * const blockRun = b.CreatePHI(i64Ty, 2);
        blockRun->addIncoming(run0, entry);
        auto wordPtr = [&](const std::string & name, unsigned i) {
            return b.CreatePointerCast(b.getInputStreamBlockPtr(name, b.getInt32(i), blockNo), PointerType::getUnqual(b.getContext()));
        };
        std::vector<Value *> inWords, runPtrs;
        for (unsigned i = 0; i < mLengths.size(); i++) inWords.push_back(wordPtr("Starts", i));
        for (unsigned r = 0; r < mRunCount; r++) runPtrs.push_back(wordPtr("Runs", r));
        Value * const outWords = b.CreatePointerCast(b.getOutputStreamBlockPtr("Selected", b.getInt32(0), blockNo), PointerType::getUnqual(b.getContext()));
        b.CreateBr(wordLoop);

        //  Each word: the positions covered from before are excluded.
        b.SetInsertPoint(wordLoop);
        PHINode * const wordNo = b.CreatePHI(sizeTy, 2);
        wordNo->addIncoming(ZERO, blockLoop);
        PHINode * const covered = b.CreatePHI(i64Ty, 2);
        covered->addIncoming(blockCovered, blockLoop);
        PHINode * const run = b.CreatePHI(i64Ty, 2);
        run->addIncoming(blockRun, blockLoop);
        std::vector<Value *> words, runWords;
        Value * any = W0;
        for (Value * ptr : inWords) {
            words.push_back(b.CreateLoad(i64Ty, b.CreateGEP(i64Ty, ptr, wordNo)));
            any = b.CreateOr(any, words.back());
        }
        for (Value * ptr : runPtrs) runWords.push_back(b.CreateLoad(i64Ty, b.CreateGEP(i64Ty, ptr, wordNo)));
        //  The word of the run r (zero if none).
        auto runWord = [&](Value * r) {
            Value * w = W0;
            for (unsigned k = 0; k < mRunCount; k++) w = b.CreateSelect(b.CreateICmpEQ(r, b.getInt64(k)), runWords[k], w);
            return w;
        };
        //  The end of the run of r from position (< 64): the first position not in it.
        auto runEnd = [&](Value * r, Value * position) {
            Value * const ones = b.CreateNot(b.CreateLShr(runWord(r), position));
            return b.CreateAdd(position, b.CreateCountForwardZeroes(ones));
        };
        auto lowMask = [&](Value * n) {     // the positions below n (< 64)
            return b.CreateSub(b.CreateShl(b.getInt64(1), n), b.getInt64(1));
        };
        Value * const allCovered = b.CreateICmpUGE(covered, W64);
        Value * const base = b.CreateSelect(allCovered, W0, covered);
        Value * const hasRun = b.CreateICmpNE(run, NONE);
        Value * const end0 = b.CreateSelect(hasRun, runEnd(run, base), base);
        Value * const toEnd = b.CreateOr(allCovered, b.CreateICmpUGE(end0, W64));
        Value * const coverMask = b.CreateSelect(toEnd, b.getInt64(-1), lowMask(b.CreateSelect(toEnd, W0, end0)));
        Value * const candidates = b.CreateAnd(any, b.CreateNot(coverMask));
        Value * const carried = b.CreateSelect(allCovered, b.CreateSub(covered, W64), W0);
        Value * const carriedRun = b.CreateSelect(toEnd, run, NONE);
        BasicBlock * const wordStart = b.GetInsertBlock();
        b.CreateBr(selectLoop);

        //  The leftmost candidate applies; the candidates within it are excluded.
        b.SetInsertPoint(selectLoop);
        PHINode * const remaining = b.CreatePHI(i64Ty, 2);
        remaining->addIncoming(candidates, wordStart);
        PHINode * const selected = b.CreatePHI(i64Ty, 2);
        selected->addIncoming(W0, wordStart);
        PHINode * const nextCovered = b.CreatePHI(i64Ty, 2);
        nextCovered->addIncoming(carried, wordStart);
        PHINode * const nextRun = b.CreatePHI(i64Ty, 2);
        nextRun->addIncoming(carriedRun, wordStart);
        b.CreateCondBr(b.CreateICmpEQ(remaining, W0), wordDone, selectOne);

        b.SetInsertPoint(selectOne);
        Value * const pos = b.CreateCountForwardZeroes(remaining);
        Value * length = b.getInt64(mLengths.back());
        Value * matchRun = b.getInt64(mRuns.back());
        for (int i = static_cast<int>(mLengths.size()) - 2; i >= 0; i--) {
            Value * const bit = b.CreateICmpNE(b.CreateAnd(b.CreateLShr(words[i], pos), b.getInt64(1)), W0);
            length = b.CreateSelect(bit, b.getInt64(mLengths[i]), length);
            matchRun = b.CreateSelect(bit, b.getInt64(mRuns[i]), matchRun);
        }
        //  The end of the text, then of its run.
        Value * const textEnd = b.CreateAdd(pos, length);
        Value * const textBeyond = b.CreateICmpUGE(textEnd, W64);
        Value * const withRun = b.CreateICmpNE(matchRun, NONE);
        Value * const end = b.CreateSelect(b.CreateAnd(withRun, b.CreateNot(textBeyond)),
                                           runEnd(matchRun, b.CreateSelect(textBeyond, W0, textEnd)), textEnd);
        Value * const beyond = b.CreateICmpUGE(end, W64);
        Value * const keep = b.CreateSelect(beyond, W0, b.CreateShl(b.getInt64(-1), b.CreateSelect(beyond, W0, end)));
        remaining->addIncoming(b.CreateAnd(remaining, keep), selectOne);
        selected->addIncoming(b.CreateOr(selected, b.CreateShl(b.getInt64(1), pos)), selectOne);
        nextCovered->addIncoming(b.CreateSelect(beyond, b.CreateSub(textEnd, b.CreateSelect(textBeyond, W64, textEnd)), nextCovered), selectOne);
        nextRun->addIncoming(b.CreateSelect(beyond, matchRun, nextRun), selectOne);
        b.CreateBr(selectLoop);

        b.SetInsertPoint(wordDone);
        b.CreateStore(selected, b.CreateGEP(i64Ty, outWords, wordNo));
        Value * const nextWord = b.CreateAdd(wordNo, ONE);
        wordNo->addIncoming(nextWord, wordDone);
        covered->addIncoming(nextCovered, wordDone);
        run->addIncoming(nextRun, wordDone);
        b.CreateCondBr(b.CreateICmpULT(nextWord, WORDS_PER_BLOCK), wordLoop, blockDone);

        b.SetInsertPoint(blockDone);
        Value * const nextBlock = b.CreateAdd(blockNo, ONE);
        blockNo->addIncoming(nextBlock, blockDone);
        blockCovered->addIncoming(nextCovered, blockDone);
        blockRun->addIncoming(nextRun, blockDone);
        b.CreateCondBr(b.CreateICmpULT(nextBlock, numOfStrides), blockLoop, done);

        b.SetInsertPoint(done);
        PHINode * const finalCovered = b.CreatePHI(i64Ty, 2);
        finalCovered->addIncoming(covered0, entry);
        finalCovered->addIncoming(nextCovered, blockDone);
        PHINode * const finalRun = b.CreatePHI(i64Ty, 2);
        finalRun->addIncoming(run0, entry);
        finalRun->addIncoming(nextRun, blockDone);
        b.setScalarField("covered", finalCovered);
        b.setScalarField("run", b.CreateAdd(finalRun, b.getInt64(1)));
    }
private:
    const std::vector<unsigned> mLengths;
    const std::vector<int> mRuns;
    const unsigned mRunCount;
};

//  The codepoint standing for the characters outside the filter of a filtered
//  normalization (see RulePipelineBuilder::maskedNormalization): a starter
//  (ccc 0) without a decomposition, which does not compose with any character.
//  It is a noncharacter (rather than a surrogate, which NFC's UTF-8 processing
//  does not accept), which may occur in the text.
static constexpr UCD::codepoint_t BarrierMark = 0xFDD0;

//  The 21-bit basis with the barrier codepoint at the positions not in Mask.
class ReplaceUnmasked : public pablo::PabloKernel {
public:
    ReplaceUnmasked(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Mask, StreamSet * Replaced)
    : PabloKernel(ts, "ReplaceUnmasked" + std::to_string(BarrierMark),
                  {Binding{"Basis", Basis}, Binding{"Mask", Mask}}, {Binding{"Replaced", Replaced}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const mask = getInputStreamSet("Mask")[0];
        PabloAST * const unmasked = pb.createInFile(pb.createNot(mask));
        std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        for (unsigned b = 0; b < basis.size(); b++) {
            basis[b] = ((BarrierMark >> b) & 1) ? pb.createOr(basis[b], unmasked) : pb.createAnd(basis[b], mask);
        }
        writeOutputStreamSet("Replaced", basis);
    }
};

//  The characters followed by inserted positions: disjoint sets, by the
//  number of positions inserted after each.
using Insertions = std::map<unsigned, UCD::UnicodeSet>;

//  In a fused kernel (FusedRules), the mappings of at most this many characters
//  change by the per-bit sets of the characters whose bits change to their
//  targets; larger ones by their targets, at their positions.
static constexpr unsigned SmallMappingLimit = 64;
//  Unless the mappings of a subgroup have more characters than this in all
//  (with small mappings, see FusedSubgroupPlan): they are then multiplexed.
static constexpr unsigned MultiplexedMappingsLimit = 2048;

//  The positions of Unicode titlecasing (toTitlecase, Unicode 3.13, R3): for
//  each word boundary, the first cased character F at or after it is mapped to
//  its titlecase, and the characters after F up to the next word boundary to
//  their lowercase.  WordBoundaries marks the position after each boundary
//  (the first character of each word, and the end of the text).
class TitlecasePositions : public pablo::PabloKernel {
public:
    TitlecasePositions(LLVMTypeSystemInterface & ts, StreamSet * WordBoundaries, StreamSet * Cased,
                       StreamSet * Title, StreamSet * Lower)
    : PabloKernel(ts, "TitlecasePositions",
                  {Binding{"WordBoundaries", WordBoundaries}, Binding{"Cased", Cased}},
                  {Binding{"Title", Title}, Binding{"Lower", Lower}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const boundaries = getInputStreamSet("WordBoundaries")[0];
        PabloAST * const cased = getInputStreamSet("Cased")[0];
        //  The first cased character at or after each boundary: at a boundary, or
        //  at the end of a run of other characters from a boundary.  (Scanning
        //  only from the boundaries at other characters, within runs, so that
        //  the scans do not carry into the boundaries at cased characters.)
        PabloAST * const notCased = pb.createNot(cased);
        PabloAST * const scanned = pb.createScanThru(pb.createAnd(boundaries, notCased), notCased);
        PabloAST * const title = pb.createAnd(pb.createOr(scanned, boundaries), cased, "title");
        //  The characters after it, up to the first boundary after it.
        PabloAST * const next = pb.createAdvance(title, 1);
        PabloAST * const end = pb.createScanTo(next, boundaries);
        PabloAST * const lower = pb.createInFile(pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {next, end}), "lower");
        writeOutputStreamSet("Title", std::vector<PabloAST *>{title});
        writeOutputStreamSet("Lower", std::vector<PabloAST *>{lower});
    }
};

//  Fused conversion rules.
//
//  A conversion group computes, for each of its rules, where it applies and the
//  bits it changes there (see RulePipelineBuilder::conversionGroup).  Its rules
//  are compiled into a few fused kernels (FusedRules), each for a chunk of its
//  string rules or of its subgroups of single-character rules, rather than into
//  kernels per rule.
//
//  The rules whose contexts are fixed sequences of sets (tier A: no repetition
//  other than a fixed count, no alternatives of sequences), or expand into a few
//  of them (tier B: optional items and alternatives of sequences of different
//  lengths, see ContextExpansion), are matched in the fused kernel, without the
//  regular expression engine: a match is the AND of the character classes of
//  its text and of one alternative of each context at fixed offsets from the
//  first character of the text, before it by Advance, at and after it by the
//  classes looked ahead.  An item of a context may also hold beyond the text,
//  before its start or past its end (the text boundary, which negated sets
//  include).  The other rules (unbounded repetitions in contexts) are matched by
//  the engine, and their matches given to the fused kernels as input streams.

struct FusedItem {
    UCD::UnicodeSet chars;
    bool boundary = false;      // also holds beyond the text
    //  A possessive repetition of the set (zero or more of its characters, as
    //  many as there are): only in before contexts matched against the converted
    //  text (see ContextExpansion).
    bool star = false;
};
//  The items of a context, in the order of the text.
using FusedContext = std::vector<FusedItem>;

static bool isSetRE(re::RE * re) {
    using namespace re;
    if (isa<CC>(re) || isa<PropertyExpression>(re) || isa<re::Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)) return true;
    if (Name * n = dyn_cast<Name>(re)) {
        return !ldml::isFunctionCall(n) && n->getDefinition() && isSetRE(n->getDefinition());
    }
    if (Alt * alt = dyn_cast<Alt>(re)) {
        for (RE * a : *alt) {
            if (!isSetRE(a)) return false;
        }
        return alt->size() > 0;
    }
    if (Seq * seq = dyn_cast<Seq>(re)) return seq->size() == 1 && isSetRE(seq->front());
    return false;
}

static UCD::UnicodeSet setOfRE(re::RE * re) {
    UCD::UnicodeSet chars;
    for (const ldml::PatternItem & item : ldml::patternItems(re)) chars = chars + item.chars;
    return chars;
}

//  The alternatives of a context: each a fixed sequence of items, which the
//  context matches where one of them does.  A context without optional items
//  (tier A) has one alternative; with bounded repetitions or alternations of
//  sequences of different lengths (tier B), a few.
using FusedContexts = std::vector<FusedContext>;

//  At most this many alternatives of a context are matched in a fused kernel
//  (more, and the context is matched by the regular expression engine).
static constexpr size_t FusedMaxAlternatives = 32;

//  At most this many string rules, or subgroups, in a correction kernel for a
//  whole group (larger groups keep their before contexts matched against the text).
static constexpr size_t FusedMaxWholeGroupRules = 256;

namespace {

//  The expansion of a context (as given for the regular expression engine, see
//  ldml::engineContext) into alternatives, matching it outward from the text
//  (forward for an after context, backward for a before context) as ICU does,
//  with possessive repetitions (see ldml::possessiveContext): an optional item
//  is taken where it matches, and skipped only where it does not (or beyond the
//  text).  An alternative is built as the constraints at the positions outward
//  from the text (position 0 adjacent to it); the constraints at a position
//  combine by intersection.
class ContextExpansion {
public:
    //  With allowStar, a before context may also hold an unbounded repetition of
    //  a set (as a star item): at most one per alternative, with at most one item
    //  beyond it (from which its characters are removed, as the repetition takes
    //  all of them); a repetition with no item beyond it always matches, and is
    //  dropped.
    explicit ContextExpansion(bool before, bool allowStar = false) : mBefore(before), mAllowStar(allowStar && before) {}
    bool expand(re::RE * context, FusedContexts & alternatives) {
        alternatives.clear();
        mFailed = false;
        std::vector<FusedContext> out;
        std::vector<re::RE *> elements;
        if (context) elements.push_back(context);
        extend(elements, 0, FusedContext(), 0, out);
        if (mFailed || out.size() > FusedMaxAlternatives) return false;
        for (FusedContext & alt : out) {
            size_t stars = 0, star = 0;
            for (size_t k = 0; k < alt.size(); k++) {
                if (alt[k].star) {
                    stars++;
                    star = k;
                }
            }
            if (stars > 1 || (stars == 1 && alt.size() > star + 2)) return false;
            if (stars == 1) {
                if (alt.size() == star + 1) {
                    alt.pop_back();
                } else {
                    alt[star + 1].chars = alt[star + 1].chars - alt[star].chars;
                }
            }
        }
        for (FusedContext & alt : out) {
            //  (A before context in text order: farthest first.)
            if (mBefore) std::reverse(alt.begin(), alt.end());
            alternatives.push_back(std::move(alt));
        }
        return true;
    }
private:
    //  The items of a fixed sequence (no optional parts) outward from the text,
    //  if the element is one.
    bool fixedItems(re::RE * re, FusedContext & items) {
        ContextExpansion inner(mBefore, false);
        FusedContexts alts;
        if (!inner.expand(re, alts) || alts.size() != 1) return false;
        items = alts.front();
        if (mBefore) std::reverse(items.begin(), items.end());     // outward order
        return true;
    }
    bool constrain(FusedContext & alt, size_t pos, const FusedItem & item) {
        if (alt.size() <= pos) alt.resize(pos + 1, FusedItem{UCD::UnicodeSet(0, 0x10FFFF), true});
        if (alt[pos].star) {
            mFailed = true;
            return false;
        }
        alt[pos].chars = alt[pos].chars & item.chars;
        alt[pos].boundary = alt[pos].boundary && item.boundary;
        return !alt[pos].chars.empty() || alt[pos].boundary;
    }
    //  Extend the alternative alt (positions before pos assigned) by the
    //  elements from index i (in outward order).
    void extend(const std::vector<re::RE *> & elements, size_t i, FusedContext alt, size_t pos, std::vector<FusedContext> & out) {
        using namespace re;
        if (mFailed) return;
        if (out.size() > FusedMaxAlternatives) {
            mFailed = true;
            return;
        }
        if (i == elements.size()) {
            //  Positions beyond pos may hold the constraints of skipped items.
            out.push_back(std::move(alt));
            return;
        }
        RE * const e = elements[i];
        auto rest = [&](std::vector<RE *> replacement) {
            std::vector<RE *> next(replacement);
            next.insert(next.end(), elements.begin() + i + 1, elements.end());
            return next;
        };
        auto item = [&](FusedItem it) {
            if (constrain(alt, pos, it)) extend(elements, i + 1, alt, pos + 1, out);
        };
        if (isSetRE(e)) {
            item(FusedItem{setOfRE(e), false});
        } else if (isa<Start>(e) || isa<End>(e)) {
            item(FusedItem{UCD::UnicodeSet(), true});
        } else if (Seq * seq = dyn_cast<Seq>(e)) {
            std::vector<RE *> items(seq->begin(), seq->end());
            if (mBefore) std::reverse(items.begin(), items.end());
            extend(rest(items), 0, alt, pos, out);
        } else if (Alt * a = dyn_cast<Alt>(e)) {
            //  A set (with the boundary) is one item; otherwise each alternative.
            bool sets = true;
            FusedItem it;
            for (RE * x : *a) {
                if (isa<Start>(x) || isa<End>(x)) it.boundary = true;
                else if (isSetRE(x)) it.chars = it.chars + setOfRE(x);
                else sets = false;
            }
            if (sets) {
                item(it);
            } else {
                for (RE * x : *a) extend(rest({x}), 0, alt, pos, out);
            }
        } else if (Assertion * as = dyn_cast<Assertion>(e)) {
            //  (?!X) or (?<!X): a character not in X, or the text boundary.
            if (as->getSense() == Assertion::Sense::Negative && isSetRE(as->getAsserted())
                    && (as->getKind() == Assertion::Kind::LookAhead || as->getKind() == Assertion::Kind::LookBehind)) {
                item(FusedItem{UCD::UnicodeSet(0, 0x10FFFF) - setOfRE(as->getAsserted()), true});
            } else {
                mFailed = true;
            }
        } else if (Name * n = dyn_cast<Name>(e)) {
            if (ldml::isFunctionCall(n) || n->getDefinition() == nullptr) mFailed = true;
            else extend(rest({n->getDefinition()}), 0, alt, pos, out);
        } else if (Capture * c = dyn_cast<Capture>(e)) {
            extend(rest({c->getCapturedRE()}), 0, alt, pos, out);
        } else if (Rep * r = dyn_cast<Rep>(e)) {
            if (r->getUB() == Rep::UNBOUNDED_REP) {
                if (!mAllowStar || !isSetRE(r->getRE())) {
                    mFailed = true;
                    return;
                }
                if (r->getLB() > 0) {
                    //  x{lb,}: lb copies of x, then x*.
                    std::vector<RE *> copies(r->getLB(), r->getRE());
                    copies.push_back(makeRep(r->getRE(), 0, Rep::UNBOUNDED_REP));
                    extend(rest(copies), 0, alt, pos, out);
                    return;
                }
                if (alt.size() > pos) {     // a constraint at its position
                    mFailed = true;
                    return;
                }
                FusedItem it{setOfRE(r->getRE()), false};
                it.star = true;
                alt.push_back(it);
                extend(elements, i + 1, alt, pos + 1, out);
                return;
            }
            const int lb = r->getLB(), ub = r->getUB();
            if (lb > 0 || ub > 1) {
                //  x{lb,ub}: lb copies of x, then ub - lb optional copies.
                std::vector<RE *> copies(lb, r->getRE());
                for (int k = lb; k < ub; k++) copies.push_back(makeRep(r->getRE(), 0, 1));
                extend(rest(copies), 0, alt, pos, out);
                return;
            }
            if (ub == 0) {
                extend(elements, i + 1, alt, pos, out);
                return;
            }
            //  x?, x a fixed sequence of k items: taken where x matches; skipped
            //  where x does not, at one of its k positions.
            FusedContext body;
            if (!fixedItems(r->getRE(), body)) {
                mFailed = true;
                return;
            }
            FusedContext taken = alt;
            bool ok = true;
            for (size_t k = 0; k < body.size() && ok; k++) ok = constrain(taken, pos + k, body[k]);
            if (ok) extend(elements, i + 1, taken, pos + body.size(), out);
            for (size_t k = 0; k < body.size(); k++) {
                FusedContext skipped = alt;
                FusedItem notItem{UCD::UnicodeSet(0, 0x10FFFF) - body[k].chars, !body[k].boundary};
                if (constrain(skipped, pos + k, notItem)) extend(elements, i + 1, skipped, pos, out);
            }
        } else {
            mFailed = true;
        }
    }
    const bool mBefore;
    const bool mAllowStar;
    bool mFailed = false;
};

}

//  The alternatives of a context, if they can be matched in a fused kernel.
static bool fusedContexts(re::RE * context, bool before, FusedContexts & alternatives, bool allowStar = false) {
    return ContextExpansion(before, allowStar).expand(context, alternatives);
}

//  The conversions of a group: the characters each may change (at a position of
//  a single-character rule, or of the text of a string rule) and the converted
//  text there (empty if deleted).
struct ConversionOutcome {
    UCD::UnicodeSet chars;
    std::u32string out;
    //  If keptAt >= 0: the character itself is kept, at index keptAt of the
    //  result (out is the rest).
    int keptAt = -1;
    size_t length() const {return out.size() + (keptAt >= 0 ? 1 : 0);}
    //  The characters of the result: those of out, and the characters kept.
    UCD::UnicodeSet resultChars() const {
        UCD::UnicodeSet r = keptAt >= 0 ? chars : UCD::UnicodeSet();
        for (char32_t c : out) r.insert(c);
        return r;
    }
};

static std::vector<ConversionOutcome> conversionOutcomes(const ldml::TransformStep & step) {
    std::vector<ConversionOutcome> outcomes;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        for (const ldml::CharMapping & m : s.mappings) {
            UCD::UnicodeSet chars = m.chars;
            if (m.replacement.size() == 1) chars = chars - UCD::UnicodeSet(m.replacement[0]);
            if (!chars.empty()) outcomes.push_back(ConversionOutcome{chars, m.replacement});
        }
    }
    for (const ldml::StringRule & sr : step.stringRules) {
        const size_t n = sr.text.size(), m = sr.replacement.size();
        for (size_t j = 0; j < n; j++) {
            std::u32string out;
            if (j + 1 < n) {
                if (j < m) out = sr.replacement.substr(j, 1);
            } else if (j < m) {
                out = sr.replacement.substr(j);
            }
            outcomes.push_back(ConversionOutcome{sr.text[j], out});
        }
        if (!sr.run.empty()) outcomes.push_back(ConversionOutcome{sr.run, std::u32string()});
    }
    //  The replacements of the MultipleCaptureMultipleReplacement rules (as string
    //  rules: Ti by Ri, or an insertion around a kept character).
    auto textOutcomes = [&](const std::vector<UCD::UnicodeSet> & text, const std::u32string & replacement) {
        const size_t n = text.size(), m = replacement.size();
        for (size_t j = 0; j < n; j++) {
            std::u32string out;
            if (j + 1 < n) {
                if (j < m) out = replacement.substr(j, 1);
            } else if (j < m) {
                out = replacement.substr(j);
            }
            outcomes.push_back(ConversionOutcome{text[j], out});
        }
    };
    for (const ldml::MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
        for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) textOutcomes(r.first, r.firstReplacement);
        for (const ldml::BoundaryReplacement & br : set.replacements) {
            if (br.kind == ldml::BoundaryReplacement::Kind::Replace) {
                textOutcomes(br.text, br.replacement);
            } else {
                outcomes.push_back(ConversionOutcome{br.text.front(), br.replacement, static_cast<int>(br.keepAt)});
            }
        }
    }
    return outcomes;
}

//  Whether the conversions of a group may make a before context (as fused
//  alternatives) match differently in the converted text, against which ICU
//  matches it, than in the text itself: a conversion changes whether a
//  character is in the set of an item (the last character of its converted
//  text, for the item adjacent to the rule), deletes a character (the context
//  then sees the characters before it), or (for the items further from the
//  rule) changes the length of the text.
static bool convertedContextMatters(const FusedContexts & before, const std::vector<ConversionOutcome> & outcomes) {
    for (const FusedContext & alt : before) {
        const size_t B = alt.size();
        for (size_t k = 0; k < B; k++) {
            const size_t d = B - k;     // 1: adjacent to the rule
            const UCD::UnicodeSet & S = alt[k].chars;
            for (const ConversionOutcome & o : outcomes) {
                if (o.length() == 0) return true;
                if (d > 1 && o.length() != 1) return true;
                //  The last character kept: whether it is in the set does not change.
                if (o.keptAt >= 0 && static_cast<size_t>(o.keptAt) == o.out.size()) continue;
                const bool in = S.contains(o.out.back());
                if (in ? !o.chars.subset(S) : o.chars.intersects(S)) return true;
            }
        }
    }
    return false;
}

//  The notes of tconv --plan on the before contexts of a conversion group that
//  its conversions may make match differently in the converted text, against
//  which ICU matches them: those matched against the converted text (see
//  FusedRules), and warnings for the others.
//  Whether a single-character rule of a group has its before context matched
//  against the converted text.  Such a rule may then match within the text of
//  a string rule (seeing the converted text before the match), where the
//  matches of the string rules, as given by the plan (which matches contexts
//  against the text itself), are not known to be covering: the single-character
//  rules then apply only outside the applied matches of the string rules.
static bool hasConvertedSubgroup(const ldml::TransformStep & step, const std::vector<ConversionOutcome> & outcomes) {
    if (ConvertedContextRounds == 0 || step.stringRules.empty()) return false;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        FusedContexts before, after;
        if (s.engineBefore && fusedContexts(s.engineAfter, false, after) && fusedContexts(s.engineBefore, true, before, true)
                && convertedContextMatters(before, outcomes)) return true;
    }
    return false;
}


//  The MultipleCaptureMultipleReplacement rules whose before contexts the
//  conversions of their group may make match differently in the converted text
//  (or of which this is not known): their contexts are matched against the
//  text itself, by the regular expression engine.
//  Whether a group has only insertion rules, with before contexts of one
//  character (applied by insertionGroup; otherwise, the insertion rules are
//  applied by the correction kernel of the group).
static bool insertionsOnly(const ldml::TransformStep & step) {
    if (step.insertions.empty() || !step.subgroups.empty()) return false;
    for (const ldml::InsertionRule & ins : step.insertions) {
        if (!ins.singleBefore) return false;
    }
    return true;
}

//  The insertion rules of a group applied by its correction kernel (see
//  insertionsOnly): their before contexts must be sequences of sets (possibly
//  repeated, see ContextExpansion), and
//  the before contexts of the single-character rules, which may see the texts
//  inserted, must be matched against the converted text.
static void insertionContextProblems(const ldml::TransformStep & step, std::vector<std::string> & problems) {
    if (step.insertions.empty() || insertionsOnly(step)) return;
    for (const ldml::InsertionRule & ins : step.insertions) {
        FusedContexts before;
        if (ConvertedContextRounds == 0) {
            problems.push_back(ldml::printRule(ins.rule) + "  (an insertion rule, but before contexts are not matched "
                               "against the converted text: --converted-context-rounds=0)");
        } else if (!fusedContexts(ins.engineBefore, true, before, true)) {
            problems.push_back(ldml::printRule(ins.rule) + "  (an insertion rule, but its before context is not a sequence "
                               "of sets and repeated sets)");
        }
    }
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        if (s.engineBefore == nullptr) continue;
        FusedContexts before, after;
        if (!fusedContexts(s.engineBefore, true, before, true) || !fusedContexts(s.engineAfter, false, after)) {
            for (const ldml::InsertionRule & ins : step.insertions) {
                problems.push_back(ldml::printRule(ins.rule) + "  (an insertion rule, but rules of its group, with contexts ["
                                   + s.contextKey + "], cannot be matched against the converted text)");
            }
            return;
        }
    }
}

//  The possessive walk over the text of a MultipleCaptureMultipleReplacement
//  rule from the start of each match (see CaptureWalk): an item is a set of
//  characters (one), an optional one (taken if there), a run of them (as many
//  as there are), or alternative strings (the longest that matches).
struct WalkItem {
    enum class Kind {Set, Optional, Star, Strings};
    Kind kind;
    UCD::UnicodeSet set;
    std::vector<std::vector<UCD::UnicodeSet>> strings;      // longest first
};

//  The items of a capture's pattern, as resolved by ldml::expandedContext, or false.
static bool walkItems(const re::RE * re, std::vector<WalkItem> & items) {
    if (const re::CC * cc = dyn_cast<re::CC>(re)) {
        items.push_back(WalkItem{WalkItem::Kind::Set, *cc, {}});
        return true;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) if (!walkItems(e, items)) return false;
        return true;
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        const re::CC * cc = dyn_cast<re::CC>(rep->getRE());
        if (cc == nullptr) return false;
        for (int i = 0; i < rep->getLB(); i++) items.push_back(WalkItem{WalkItem::Kind::Set, *cc, {}});
        if (rep->getUB() == re::Rep::UNBOUNDED_REP) {
            items.push_back(WalkItem{WalkItem::Kind::Star, *cc, {}});
        } else {
            for (int i = rep->getLB(); i < rep->getUB(); i++) items.push_back(WalkItem{WalkItem::Kind::Optional, *cc, {}});
        }
        return true;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        WalkItem w{WalkItem::Kind::Strings, {}, {}};
        for (const re::RE * a : *alt) {
            std::vector<UCD::UnicodeSet> str;
            if (const re::CC * cc = dyn_cast<re::CC>(a)) {
                str.push_back(*cc);
            } else if (const re::Seq * sq = dyn_cast<re::Seq>(a)) {
                for (const re::RE * e : *sq) {
                    const re::CC * cc = dyn_cast<re::CC>(e);
                    if (cc == nullptr) return false;
                    str.push_back(*cc);
                }
            } else {
                return false;
            }
            w.strings.push_back(std::move(str));
        }
        std::stable_sort(w.strings.begin(), w.strings.end(), [](const auto & a, const auto & b) {return a.size() > b.size();});
        items.push_back(std::move(w));
        return true;
    }
    return false;
}

static void captureContextProblems(const ldml::TransformStep & step, std::vector<std::string> & problems) {
    if (step.captureReplacements.empty()) return;
    //  The captures of the rules whose regions are moved must be found by a
    //  possessive walk (see CaptureWalk).
    for (const ldml::MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
        if (!set.moved) continue;
        bool walkable = true;
        for (re::RE * c : set.captures) {
            std::vector<WalkItem> items;
            walkable &= walkItems(ldml::expandedContext(c, Insertions(), DeletionMark), items);
        }
        if (walkable) continue;
        for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) {
            problems.push_back(ldml::printRule(r.rule) + "  (a MultipleCaptureMultipleReplacement rule, but it refers to its "
                               "captures out of order, and a capture is not a sequence of sets, optional sets, runs of sets "
                               "and sets with strings)");
        }
    }
    const std::vector<ConversionOutcome> outcomes = conversionOutcomes(step);
    for (const ldml::MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
        if (set.engineBefore == nullptr) continue;
        FusedContexts before;
        if (fusedContexts(set.engineBefore, true, before, true) && !convertedContextMatters(before, outcomes)) continue;
        for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) {
            problems.push_back(ldml::printRule(r.rule) + "  (a MultipleCaptureMultipleReplacement rule, but its before "
                               "context may match differently in the converted text)");
        }
    }
}

static void printBeforeContextNotes(const ldml::TransformStep & step) {
    const std::vector<ConversionOutcome> outcomes = conversionOutcomes(step);
    bool changingMappings = false;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        for (const ldml::CharMapping & m : s.mappings) {
            UCD::UnicodeSet chars = m.chars - UCD::UnicodeSet(DeletionMark);
            if (m.replacement.size() == 1) chars = chars - UCD::UnicodeSet(m.replacement[0]);
            changingMappings |= !chars.empty();
        }
    }
    const bool needCoverage = (step.coverStringRules || hasConvertedSubgroup(step, outcomes)) && changingMappings;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        FusedContexts before, after;
        if (s.engineBefore && fusedContexts(s.engineBefore, true, before, true) && fusedContexts(s.engineAfter, false, after)
                && convertedContextMatters(before, outcomes)) {
            llvm::outs() << "    before context matched against the converted text: [" << s.contextKey << "]\n";
        }
    }
    bool wholeGroup = false;
    for (const ldml::StringRule & sr : step.stringRules) {
        FusedContexts before, after;
        if (sr.engineBefore && fusedContexts(sr.engineBefore, true, before, true) && fusedContexts(sr.engineAfter, false, after)
                && convertedContextMatters(before, outcomes)) {
            const bool wholeGroupFits = step.stringRules.size() <= FusedMaxWholeGroupRules && step.subgroups.size() <= FusedMaxWholeGroupRules;
            wholeGroup |= (!step.stringOrder.empty() || needCoverage) && wholeGroupFits;
            if ((step.stringOrder.empty() && !needCoverage) || wholeGroupFits) {
                llvm::outs() << "    before context matched against the converted text: " << ldml::printRule(sr.rule) << "\n";
            } else {
                llvm::outs() << "    warning: the before context of " << ldml::printRule(sr.rule)
                             << " may match differently in the converted text\n";
            }
        }
    }
    if (wholeGroup && step.sequentialSelection) {
        llvm::outs() << "    string matches selected sequentially between the " << ConvertedContextRounds + 1
                     << " rounds of matching before contexts against the converted text\n";
    }
    //  The contexts that are not fused (with unbounded repetitions) are matched
    //  against the text itself.
        //  ICU matches before contexts against the converted text.  A before context
    //  may match differently in the unconverted text, as the regular expression
    //  engine sees it, if a conversion of the group:
    //    - replaces a character by one, where exactly one of them is in a set of
    //      the context;
    //    - replaces a character by a longer string, where the character or one of
    //      the string is in a set of the context;
    //    - deletes a character, unless the context is A S* with the character
    //      in S but not in A (a deleted character within a match is then in S*,
    //      as after the deletion closure of DisambiguateOrder).
    //  (The characters that a mapping does not change are excluded.)
    std::vector<ldml::CharMapping> conversions;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        for (const ldml::CharMapping & m : s.mappings) {
            UCD::UnicodeSet chars = m.chars;
            if (m.replacement.size() == 1) chars = chars - UCD::UnicodeSet(m.replacement[0]);
            if (!chars.empty()) conversions.push_back(ldml::CharMapping{chars, m.replacement});
        }
    }
    //  String rules convert and delete the characters of their texts, and produce
    //  those of their replacements.
    UCD::UnicodeSet stringRuleChars;
    for (const ldml::StringRule & sr : step.stringRules) {
        for (const UCD::UnicodeSet & t : sr.text) stringRuleChars = stringRuleChars + t;
        for (char32_t c : sr.replacement) stringRuleChars.insert(c);
    }
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        FusedContexts fusedBefore, fusedAfter;
        if (s.before == nullptr || (fusedContexts(s.engineBefore, true, fusedBefore, true) && fusedContexts(s.engineAfter, false, fusedAfter))) continue;
        const std::vector<ldml::PatternItem> items = ldml::patternItems(s.before);
        const bool closedForm = items.size() == 2 && !items[0].repeated && items[1].repeated;
        bool changes = false;
        for (const ldml::CharMapping & c : conversions) {
            if (c.replacement.empty()) {
                changes = !(closedForm && c.chars.subset(items[1].chars - items[0].chars));
            } else {
                for (const ldml::PatternItem & item : items) {
                    bool replacementInItem = false;
                    for (char32_t r : c.replacement) replacementInItem |= item.chars.contains(r);
                    if (c.replacement.size() == 1) {
                        //  Some character is in the item and its replacement not, or vice versa.
                        changes = replacementInItem ? !c.chars.subset(item.chars) : c.chars.intersects(item.chars);
                    } else {
                        changes = c.chars.intersects(item.chars) || replacementInItem;
                    }
                    if (changes) break;
                }
            }
            if (changes) break;
        }
        for (const ldml::PatternItem & item : items) changes |= !(item.chars & stringRuleChars).empty();
        if (changes) {
            llvm::outs() << "    warning: the before context of [" << s.contextKey
                         << "] may match differently in the converted text\n";
        }
    }
}

//  A match stream given as input to a fused kernel: a stream of a stream set.
struct FusedMatchSource {
    StreamSet * streamSet = nullptr;
    unsigned index = 0;
};

//  A string rule of a fused kernel.  Matched in the kernel, with its contexts
//  and within the mask, or (match >= 0, an index in FusedRulesConfig::sources)
//  given by an input stream of its matches at their first positions, already
//  restricted to the mask (and selected, where string rules overlap).
struct FusedStringRule {
    std::vector<UCD::UnicodeSet> text;
    std::u32string replacement;
    FusedContexts before{FusedContext()}, after{FusedContext()};
    int match = -1;
    bool converted = false;     // in a correction kernel: its before context is matched against the converted text
    UCD::UnicodeSet run;        // the set of the run following the text, deleted (see ldml::StringRule::run)
    //  If keep >= 0 (text of one character): the character is kept, with the
    //  replacement inserted around it, the character at index keep of the result
    //  (see ldml::BoundaryReplacement).
    int keep = -1;
    //  The length of the result.
    unsigned resultLength() const {return static_cast<unsigned>(replacement.size()) + (keep >= 0 ? 1 : 0);}
};

//  A subgroup of single-character rules: matched in the kernel, with its
//  contexts, or (match >= 0) given by an input stream of the positions where
//  its rules match.  Its rules apply there within Allowed (or the mask).
struct FusedSubgroup {
    FusedContexts before{FusedContext()}, after{FusedContext()};
    std::vector<ldml::CharMapping> mappings;
    int match = -1;
    bool converted = false;     // as for FusedStringRule
};

//  An insertion rule (see ldml::InsertionRule) in a correction kernel, at the
//  positions of the characters after which it inserts: its before context (of
//  fixed items) is matched against the converted text up to and including the
//  character (without the text this rule inserts there), and its after context
//  (if match < 0) against the text from the next position; or the positions
//  where the after context matches are given (match).  It applies only where
//  eligible (the source given; within a filter), and where no insertion rule
//  before it applies.
struct FusedInsertion {
    FusedContexts before{FusedContext()}, after{FusedContext()};
    int match = -1;
    int eligible = -1;
    std::u32string text;
};

//  A class of string rules for their sequential selection (see LeftmostMatches):
//  those with texts of a length, and the same run (if any).
struct MatchClass {
    unsigned length;
    UCD::UnicodeSet run;
    bool operator==(const MatchClass & other) const {return length == other.length && run == other.run;}
    bool includes(const FusedStringRule & r) const {return r.text.size() == length && r.run == run;}
};

struct FusedRulesConfig {
    std::vector<FusedStringRule> strings;
    std::vector<FusedSubgroup> subgroups;
    std::vector<FusedMatchSource> sources;
    //  Only the matches of the string rules (in the kernel), at their first
    //  positions (Firsts, a stream per rule), for SelectStringMatches.
    bool matchOnly = false;
    //  With matchOnly: the matches by the lengths of the texts instead, a stream
    //  for each length given (the union of the rules with texts of that length).
    std::vector<MatchClass> startsByLength;
    //  The matches of the string rules apply only where selected (the index of
    //  the source of the selected positions; see LeftmostMatches), or -1.
    int selected = -1;
    //  Also the spans and first positions of the matches of the string rules
    //  (Spans and Starts), for the coverage of the single-character rules.
    bool coverage = false;
    //  A correction kernel (rounds > 0): its rules have before contexts that the
    //  conversions of the group may make match differently in the converted
    //  text, against which ICU matches them.  Given the results of the other
    //  rules of the group (in the state), it matches their before contexts
    //  against the converted text, in this many rounds (see FusedRules).
    //  replacementChars: the characters of all the replacements of the group.
    unsigned rounds = 0;
    UCD::UnicodeSet replacementChars;
    //  In a correction kernel for a whole group: the string rules, in the order to
    //  apply them, with their selection (see SelectStringMatches; empty if their
    //  matches do not overlap), and whether the single-character rules apply only
    //  where not covered by the matches of the string rules that apply (and within
    //  the mask).  These are then computed in each round.
    struct Selection {
        unsigned length;
        std::vector<size_t> predecessors;
        bool selfOverlapping;
    };
    std::vector<Selection> selection;
    bool groupCoverage = false;
    //  In a correction kernel: the insertion rules of the group (see
    //  FusedInsertion), in order.
    std::vector<FusedInsertion> insertions;
    //  A round of a correction kernel for a whole group whose string matches are
    //  selected sequentially (see TransformStep::sequentialSelection), as a kernel
    //  of its own: the converted text of the previous round is given (as the
    //  state in; none for the first round), and the before contexts are matched
    //  against it once.  With matchOnly, the matches are given by length
    //  (startsByLength), for their selection; otherwise, the results of the
    //  round, of the rules applied where selected, are given (without the state
    //  in).
    bool staged = false;
};

//  The results of the rules of a conversion group, accumulated by its fused
//  kernels: the changes at each position i of the replacements (21 bits each;
//  see RulePipelineBuilder::conversionGroup), the positions after which k
//  positions are to be inserted, and the spans and first positions of the
//  matches of the string rules.  Each fused kernel ORs its results into the
//  state given to it.
struct FusedState {
    std::vector<StreamSet *> changes;
    std::map<unsigned, StreamSet *> insertions;
    StreamSet * spans = nullptr;
    StreamSet * starts = nullptr;
};

//  The input lookahead and outputs of a fused kernel.
struct FusedLayout {
    unsigned lookahead = 0;         // of Classes and InText
    unsigned maskLookahead = 0;
    unsigned changePositions = 0;   // the positions i of the replacements, with outputs
    std::set<unsigned> insertCounts;
    std::vector<StreamSet *> sourceSets;
    FusedLayout(const FusedRulesConfig & c, const FusedState & in) {
        for (const FusedStringRule & r : c.strings) {
            const unsigned n = r.text.size(), m = r.resultLength();
            if (r.match < 0) {
                for (const FusedContext & a : r.after) lookahead = std::max<unsigned>(lookahead, n - 1 + a.size());
                maskLookahead = std::max<unsigned>(maskLookahead, n - 1);
            }
            if (!c.matchOnly || c.staged) {
                changePositions = std::max<unsigned>(changePositions, m > n ? m - n + 1 : 1);
                if (m > n) insertCounts.insert(m - n);
            }
        }
        for (const FusedSubgroup & g : c.subgroups) {
            if (g.match < 0) {
                for (const FusedContext & a : g.after) lookahead = std::max<unsigned>(lookahead, a.size());
            }
            for (const ldml::CharMapping & m : g.mappings) {
                changePositions = std::max<unsigned>(changePositions, std::max<size_t>(m.replacement.size(), 1));
                if (m.replacement.size() > 1) insertCounts.insert(m.replacement.size() - 1);
            }
        }
        if (!c.matchOnly || c.staged) {
            changePositions = std::max<unsigned>(changePositions, in.changes.size());
            for (const auto & k : in.insertions) insertCounts.insert(k.first);
        }
        //  An insertion of m characters after a position whose converted text has
        //  k + 1 characters (k inserted after it) or none (deleted, the first
        //  replacing the deletion mark).
        if (!c.insertions.empty()) {
            std::set<unsigned> base = insertCounts;
            base.insert(0);
            const unsigned maxBase = *base.rbegin();
            for (const FusedInsertion & ins : c.insertions) {
                if (ins.match < 0) {
                    for (const FusedContext & a : ins.after) lookahead = std::max<unsigned>(lookahead, a.size());
                }
                const unsigned m = ins.text.size();
                for (unsigned k : base) insertCounts.insert(k + m);
                if (m > 1) insertCounts.insert(m - 1);
                changePositions = std::max<unsigned>(changePositions, maxBase + m + 1);
            }
        }
        for (const FusedMatchSource & s : c.sources) {
            if (std::find(sourceSets.begin(), sourceSets.end(), s.streamSet) == sourceSets.end()) sourceSets.push_back(s.streamSet);
        }
    }
};

//  The positions of the text (all ones within it), whose lookahead tells which
//  positions are past the end of the text.
class TextPositions : public pablo::PabloKernel {
public:
    TextPositions(LLVMTypeSystemInterface & ts, StreamSet * U21, StreamSet * InText)
    : PabloKernel(ts, "TextPositions", {Binding{"U21", U21}}, {Binding{"InText", InText}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        writeOutputStreamSet("InText", std::vector<PabloAST *>{pb.createInFile(pb.createOnes())});
    }
};

static std::string setKey(const UCD::UnicodeSet & set) {
    std::string key;
    for (const auto & r : set) key += std::to_string(r.first) + "-" + std::to_string(r.second) + ",";
    return key;
}

//  How a fused kernel changes the characters of a subgroup, at each position i
//  of the replacements: the small sets of characters by the per-bit sets of the
//  characters whose bits change (bitSets[i][b]), the large ones (more than
//  SmallMappingLimit characters) by their targets at their positions; and the
//  characters after which k positions are inserted (byInsertion[k]).
//
//  A subgroup whose mappings have more than MultiplexedMappingsLimit characters
//  in all, some in small mappings, is multiplexed instead: the per-bit sets
//  would each hold about half of the characters, scattered (as for the
//  mappings of Han characters to their readings), at each position.  The
//  mappings are numbered (in the order of their replacements, so that similar
//  replacements have close numbers), and the number of the mapping of each
//  character is given by its bits (mux[b], the characters of the mappings with
//  bit b in their numbers), the only sets of characters; the changes are then
//  sets of mapping numbers, compiled over these bits: at position 0, the
//  numbers whose targets have bit c (indexBits[0][c], to compare with the
//  character), at a position i > 0, those whose targets there differ from the
//  deletion mark in bit c (indexBits[i][c]); and the numbers of the mappings
//  after whose characters k positions are inserted (insertIndices[k]).
struct FusedSubgroupPlan {
    UCD::UnicodeSet all;
    std::vector<std::vector<UCD::UnicodeSet>> bitSets;
    struct Large {
        size_t position;
        UCD::UnicodeSet chars;
        UCD::codepoint_t target;
    };
    std::vector<Large> large;
    std::map<unsigned, UCD::UnicodeSet> byInsertion;
    bool multiplexed = false;
    std::vector<UCD::UnicodeSet> mux;
    std::vector<std::vector<UCD::UnicodeSet>> indexBits;
    std::map<unsigned, UCD::UnicodeSet> insertIndices;
    explicit FusedSubgroupPlan(const FusedSubgroup & g) {
        size_t total = 0;
        bool small = false;
        for (const ldml::CharMapping & mp : g.mappings) {
            total += mp.chars.count();
            small |= mp.chars.count() <= SmallMappingLimit;
        }
        if (small && total > MultiplexedMappingsLimit) {
            multiplexPlan(g);
            return;
        }
        size_t maxLength = 1;
        for (const ldml::CharMapping & mp : g.mappings) {
            all = all + mp.chars;
            maxLength = std::max(maxLength, mp.replacement.size());
            if (mp.replacement.size() > 1) {
                const unsigned k = static_cast<unsigned>(mp.replacement.size() - 1);
                byInsertion[k] = byInsertion[k] + mp.chars;
            }
        }
        bitSets.assign(maxLength, std::vector<UCD::UnicodeSet>(21));
        for (size_t i = 0; i < maxLength; i++) {
            for (const ldml::CharMapping & mp : g.mappings) {
                if (i > 0 && mp.replacement.size() <= i) continue;
                const UCD::codepoint_t target = mp.replacement.empty() ? DeletionMark : static_cast<UCD::codepoint_t>(mp.replacement[i]);
                if (mp.chars.count() > SmallMappingLimit) {
                    large.push_back(Large{i, mp.chars, target});
                    continue;
                }
                for (const auto & range : mp.chars) {
                    for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) {
                        const UCD::codepoint_t diff = (i == 0 ? cp : DeletionMark) ^ target;
                        for (unsigned b = 0; b < 21; b++) {
                            if ((diff >> b) & 1) bitSets[i][b].insert(cp);
                        }
                    }
                }
            }
        }
    }
    void multiplexPlan(const FusedSubgroup & g) {
        multiplexed = true;
        std::vector<const ldml::CharMapping *> order;
        for (const ldml::CharMapping & mp : g.mappings) order.push_back(&mp);
        std::stable_sort(order.begin(), order.end(), [](const ldml::CharMapping * a, const ldml::CharMapping * b) {
            return a->replacement < b->replacement;
        });
        unsigned bits = 1;
        while ((static_cast<size_t>(1) << bits) < order.size()) bits++;
        if (bits == 8) bits = 9;    // (the CC compiler of 8 bits is for UTF-8 code units)
        mux.assign(bits, UCD::UnicodeSet());
        size_t maxLength = 1;
        for (const ldml::CharMapping * mp : order) maxLength = std::max(maxLength, mp->replacement.size());
        indexBits.assign(maxLength, std::vector<UCD::UnicodeSet>(21));
        for (size_t n = 0; n < order.size(); n++) {
            const ldml::CharMapping & mp = *order[n];
            const UCD::codepoint_t index = static_cast<UCD::codepoint_t>(n);
            all = all + mp.chars;
            for (unsigned b = 0; b < bits; b++) {
                if ((n >> b) & 1) mux[b] = mux[b] + mp.chars;
            }
            const size_t length = mp.replacement.size();
            for (size_t i = 0; i < std::max<size_t>(length, 1); i++) {
                const UCD::codepoint_t target = length == 0 ? DeletionMark : static_cast<UCD::codepoint_t>(mp.replacement[i]);
                const UCD::codepoint_t bitsOf = i == 0 ? target : (DeletionMark ^ target);
                for (unsigned c = 0; c < 21; c++) {
                    if ((bitsOf >> c) & 1) indexBits[i][c].insert(index);
                }
            }
            if (length > 1) insertIndices[static_cast<unsigned>(length - 1)].insert(index);
        }
    }
};

//  The character classes that a fused kernel uses (computed by a
//  CharClassesKernel and looked ahead in the fused kernel), each once.
static std::vector<UCD::UnicodeSet> fusedClassSets(const FusedRulesConfig & c) {
    std::vector<UCD::UnicodeSet> sets;
    std::set<std::string> keys;
    auto add = [&](const UCD::UnicodeSet & set) {
        if (!set.empty() && keys.insert(setKey(set)).second) sets.push_back(set);
    };
    auto addContext = [&](const FusedContexts & alternatives) {
        for (const FusedContext & items : alternatives) {
            for (const FusedItem & item : items) add(item.chars);
        }
    };
    for (const FusedStringRule & r : c.strings) {
        add(r.run);
        if (r.match >= 0) continue;
        for (const UCD::UnicodeSet & t : r.text) add(t);
        addContext(r.before);
        addContext(r.after);
    }
    if (c.matchOnly) return sets;
    for (const FusedInsertion & ins : c.insertions) {
        addContext(ins.before);
        if (ins.match < 0) addContext(ins.after);
    }
    for (const FusedSubgroup & g : c.subgroups) {
        const FusedSubgroupPlan plan(g);
        if (g.match < 0) {
            add(plan.all);
            addContext(g.before);
            addContext(g.after);
        }
        for (const UCD::UnicodeSet & set : plan.mux) add(set);
        for (const auto & position : plan.bitSets) {
            for (const UCD::UnicodeSet & set : position) add(set);
        }
        for (const FusedSubgroupPlan::Large & l : plan.large) add(l.chars);
        for (const auto & k : plan.byInsertion) add(k.second);
    }
    return sets;
}

//  The fused kernel of a chunk of the rules of a conversion group (see above),
//  given the classes of fusedClassSets in Classes.  Out holds the stream sets
//  for its outputs, as given by FusedLayout.
class FusedRules final : public pablo::PabloKernel {
public:
    FusedRules(LLVMTypeSystemInterface & ts, FusedRulesConfig config, StreamSet * U21, StreamSet * Classes, StreamSet * InText,
               StreamSet * Mask, StreamSet * Allowed, const FusedState & in, const FusedState & out, StreamSet * Firsts)
    : FusedRules(ts, std::move(config), FusedLayout(config, in), U21, Classes, InText, Mask, Allowed, in, out, Firsts) {}
    bool hasSignature() const override { return true; }
    llvm::StringRef getSignature() const override { return mSignature; }
protected:
    void generatePabloMethod() override;
private:
    FusedRules(LLVMTypeSystemInterface & ts, FusedRulesConfig && config, FusedLayout && layout, StreamSet * U21, StreamSet * Classes,
               StreamSet * InText, StreamSet * Mask, StreamSet * Allowed, const FusedState & in, const FusedState & out,
               StreamSet * Firsts)
    : FusedRules(ts, signature(config, layout, Mask, Allowed, in), std::move(config), std::move(layout),
                 U21, Classes, InText, Mask, Allowed, in, out, Firsts) {}
    FusedRules(LLVMTypeSystemInterface & ts, std::string && sig, FusedRulesConfig && config, FusedLayout && layout,
               StreamSet * U21, StreamSet * Classes, StreamSet * InText, StreamSet * Mask, StreamSet * Allowed,
               const FusedState & in, const FusedState & out, StreamSet * Firsts)
    : PabloKernel(ts, "FusedRules_" + getStringHash(sig),
                  inputs(config, layout, U21, Classes, InText, Mask, Allowed, in),
                  outputs(config, layout, out, Firsts)),
      mConfig(std::move(config)), mLayout(std::move(layout)), mHasMask(Mask != nullptr), mHasAllowed(Allowed != nullptr),
      mClassIndex([&] {
          std::map<std::string, unsigned> index;
          for (const UCD::UnicodeSet & set : fusedClassSets(mConfig)) index.emplace(setKey(set), index.size());
          return index;
      }()),
      mChangesIn([&] {
          std::vector<bool> v;
          for (StreamSet * s : in.changes) v.push_back(s != nullptr);
          return v;
      }()),
      mInsertIn([&] {
          std::set<unsigned> v;
          for (const auto & k : in.insertions) v.insert(k.first);
          return v;
      }()),
      mCoverageIn(mConfig.coverage && in.spans != nullptr),
      mSignature(std::move(sig)) {}
    static Bindings inputs(const FusedRulesConfig & c, const FusedLayout & l, StreamSet * U21, StreamSet * Classes,
                           StreamSet * InText, StreamSet * Mask, StreamSet * Allowed, const FusedState & in) {
        Bindings b;
        b.emplace_back("U21", U21);
        if (l.lookahead > 0) {
            if (Classes) b.emplace_back("Classes", Classes, FixedRate(1), LookAhead(l.lookahead));
            b.emplace_back("InText", InText, FixedRate(1), LookAhead(l.lookahead));
        } else {
            if (Classes) b.emplace_back("Classes", Classes);
            b.emplace_back("InText", InText);
        }
        if (Mask) {
            if (l.maskLookahead > 0) b.emplace_back("Mask", Mask, FixedRate(1), LookAhead(l.maskLookahead));
            else b.emplace_back("Mask", Mask);
        }
        if (Allowed) b.emplace_back("Allowed", Allowed);
        for (size_t i = 0; i < l.sourceSets.size(); i++) b.emplace_back("Source" + std::to_string(i), l.sourceSets[i]);
        if (!c.matchOnly || c.staged) {
            for (size_t i = 0; i < in.changes.size(); i++) {
                if (in.changes[i]) b.emplace_back("ChangesIn" + std::to_string(i), in.changes[i]);
            }
            for (const auto & k : in.insertions) b.emplace_back("InsertIn" + std::to_string(k.first), k.second);
            if (c.coverage && in.spans) {
                b.emplace_back("SpansIn", in.spans);
                b.emplace_back("StartsIn", in.starts);
            }
        }
        return b;
    }
    static Bindings outputs(const FusedRulesConfig & c, const FusedLayout & l, const FusedState & out, StreamSet * Firsts) {
        Bindings b;
        if (c.matchOnly) {
            b.emplace_back("Firsts", Firsts);
            return b;
        }
        for (unsigned i = 0; i < l.changePositions; i++) b.emplace_back("Changes" + std::to_string(i), out.changes[i]);
        for (unsigned k : l.insertCounts) b.emplace_back("Insert" + std::to_string(k), out.insertions.at(k));
        if (c.coverage) {
            b.emplace_back("Spans", out.spans);
            b.emplace_back("Starts", out.starts);
        }
        return b;
    }
    static void appendSet(std::string & s, const UCD::UnicodeSet & set) {
        s += "[";
        for (const auto & r : set) s += std::to_string(r.first) + "-" + std::to_string(r.second) + ",";
        s += "]";
    }
    static void appendContext(std::string & s, const FusedContexts & alternatives) {
        s += "(";
        for (const FusedContext & items : alternatives) {
            s += "|";
            for (const FusedItem & item : items) {
                appendSet(s, item.chars);
                if (item.boundary) s += "$";
            }
        }
        s += ")";
    }
    //  Everything that shapes the generated code (hashed for the kernel name).
    static std::string signature(const FusedRulesConfig & c, const FusedLayout & l, StreamSet * Mask, StreamSet * Allowed,
                                 const FusedState & in) {
        std::string s = "FusedRulesCC" + std::to_string(DeletionMark) + "_" + std::to_string(SmallMappingLimit) + "x"
                        + std::to_string(MultiplexedMappingsLimit);
        s += c.matchOnly ? "M" : "C";
        for (const MatchClass & k : c.startsByLength) {
            s += "n" + std::to_string(k.length);
            if (!k.run.empty()) appendSet(s, k.run);
        }
        if (c.rounds) {
            s += "R" + std::to_string(c.rounds);
            appendSet(s, c.replacementChars);
            if (c.groupCoverage) s += "G";
            if (c.staged) s += "T";
            for (const FusedRulesConfig::Selection & sel : c.selection) {
                s += "/" + std::to_string(sel.length) + (sel.selfOverlapping ? "s" : "");
                for (size_t p : sel.predecessors) s += "." + std::to_string(p);
            }
        }
        if (c.coverage) s += "V";
        if (Mask) s += "K" + std::to_string(l.maskLookahead);
        if (Allowed) s += "A";
        s += "L" + std::to_string(l.lookahead) + "S";
        for (StreamSet * ss : l.sourceSets) s += std::to_string(ss->getNumElements()) + ",";
        s += "I";
        for (StreamSet * ss : in.changes) s += ss ? "1" : "0";
        s += "/";
        for (const auto & k : in.insertions) s += std::to_string(k.first) + ",";
        if (c.coverage && in.spans) s += "v";
        s += "O" + std::to_string(l.changePositions) + "/";
        for (unsigned k : l.insertCounts) s += std::to_string(k) + ",";
        auto source = [&](int match) {
            const FusedMatchSource & src = c.sources[match];
            return "@" + std::to_string(std::find(l.sourceSets.begin(), l.sourceSets.end(), src.streamSet) - l.sourceSets.begin())
                   + "." + std::to_string(src.index);
        };
        if (c.selected >= 0) s += "Q" + source(c.selected);
        for (const FusedStringRule & r : c.strings) {
            s += "|s";
            for (const UCD::UnicodeSet & t : r.text) appendSet(s, t);
            if (!r.run.empty()) {
                s += "+";
                appendSet(s, r.run);
            }
            if (r.keep >= 0) s += "k" + std::to_string(r.keep);
            s += ">";
            for (char32_t ch : r.replacement) s += std::to_string(ch) + ",";
            if (r.match >= 0) {
                s += source(r.match);
            } else {
                appendContext(s, r.before);
                appendContext(s, r.after);
            }
            if (r.converted) s += "c";
        }
        for (const FusedInsertion & ins : c.insertions) {
            s += "|i";
            appendContext(s, ins.before);
            if (ins.match >= 0) s += source(ins.match); else appendContext(s, ins.after);
            if (ins.eligible >= 0) s += "e" + source(ins.eligible);
            s += ">";
            for (char32_t ch : ins.text) s += std::to_string(ch) + ",";
        }
        for (const FusedSubgroup & g : c.subgroups) {
            s += "|g";
            if (g.match >= 0) {
                s += source(g.match);
            } else {
                appendContext(s, g.before);
                appendContext(s, g.after);
            }
            if (g.converted) s += "c";
            for (const ldml::CharMapping & m : g.mappings) {
                appendSet(s, m.chars);
                s += ">";
                for (char32_t ch : m.replacement) s += std::to_string(ch) + ",";
            }
        }
        return s;
    }
    const FusedRulesConfig mConfig;
    const FusedLayout mLayout;
    const bool mHasMask;
    const bool mHasAllowed;
    const std::map<std::string, unsigned> mClassIndex;
    const std::vector<bool> mChangesIn;
    const std::set<unsigned> mInsertIn;
    const bool mCoverageIn;
    const std::string mSignature;
};

void FusedRules::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const std::vector<PabloAST *> basis = getInputStreamSet("U21");
    PabloAST * const inText = getInputStreamSet("InText")[0];
    PabloAST * const ones = pb.createOnes();
    PabloAST * const zeroes = pb.createZeroes();

    //  The class of a set (in Classes, within the text) at an offset after the
    //  position.
    auto classAt = [&](unsigned offset, const UCD::UnicodeSet & set) -> PabloAST * {
        if (set.empty()) return zeroes;
        const auto f = mClassIndex.find(setKey(set));
        if (LLVM_UNLIKELY(f == mClassIndex.end())) llvm::report_fatal_error("FusedRules: a class not in fusedClassSets");
        PabloAST * const c = getInputStreamSet("Classes")[f->second];
        return offset ? pb.createLookahead(c, offset) : c;
    };
    //  The positions past the end of the text at an offset after them, and
    //  those before the start of the text at an offset before them.
    auto pastEnd = [&](unsigned offset) {
        return pb.createInFile(pb.createNot(pb.createLookahead(inText, offset)));
    };
    auto beforeStart = [&](unsigned offset) {
        return pb.createInFile(pb.createNot(pb.createAdvance(ones, offset)));
    };
    //  The positions at which a before context matches the B items of one of
    //  its alternatives before them, and an after context those of one of its
    //  alternatives from an offset after them.
    auto contexts = [&](const FusedContexts & before, const FusedContexts & after, unsigned afterOffset) {
        PabloAST * beforeMatch = zeroes;
        for (const FusedContext & alt : before) {
            PabloAST * m = ones;
            const unsigned B = alt.size();
            for (unsigned k = 0; k < B; k++) {
                const unsigned d = B - k;
                PabloAST * item = pb.createAdvance(classAt(0, alt[k].chars), d);
                if (alt[k].boundary) item = pb.createOr(item, beforeStart(d));
                m = pb.createAnd(m, item);
            }
            beforeMatch = pb.createOr(beforeMatch, m);
        }
        PabloAST * afterMatch = zeroes;
        for (const FusedContext & alt : after) {
            PabloAST * m = ones;
            for (unsigned k = 0; k < alt.size(); k++) {
                const unsigned o = afterOffset + k;
                PabloAST * item = classAt(o, alt[k].chars);
                if (alt[k].boundary) item = pb.createOr(item, pastEnd(o));
                m = pb.createAnd(m, item);
            }
            afterMatch = pb.createOr(afterMatch, m);
        }
        return pb.createAnd(beforeMatch, afterMatch);
    };
    auto source = [&](int match) -> PabloAST * {
        const FusedMatchSource & src = mConfig.sources[match];
        const size_t s = std::find(mLayout.sourceSets.begin(), mLayout.sourceSets.end(), src.streamSet) - mLayout.sourceSets.begin();
        return getInputStreamSet("Source" + std::to_string(s))[src.index];
    };
    PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
    PabloAST * const allowed = mHasAllowed ? getInputStreamSet("Allowed")[0] : mask;

    //  The matches of the string rules, at their first positions, and the
    //  positions where the rules of the subgroups apply.  In a correction kernel,
    //  without their before contexts (matched against the converted text below).
    const bool correction = mConfig.rounds > 0;
    std::vector<PabloAST *> starts;
    for (const FusedStringRule & r : mConfig.strings) {
        if (r.match >= 0) {
            starts.push_back(source(r.match));
            continue;
        }
        const unsigned n = r.text.size();
        PabloAST * m = contexts((correction && r.converted) ? FusedContexts{FusedContext()} : r.before, r.after, n);
        for (unsigned j = 0; j < n; j++) {
            m = pb.createAnd(m, classAt(j, r.text[j]));
            if (mask) m = pb.createAnd(m, j ? pb.createLookahead(mask, j) : mask);
        }
        starts.push_back(m);
    }
    if (mConfig.selected >= 0) {
        for (PabloAST *& m : starts) m = pb.createAnd(m, source(mConfig.selected));
    }
    if (mConfig.matchOnly && !mConfig.staged) {
        Var * const firsts = getOutputStreamVar("Firsts");
        if (!mConfig.startsByLength.empty()) {
            for (size_t k = 0; k < mConfig.startsByLength.size(); k++) {
                PabloAST * u = zeroes;
                for (size_t i = 0; i < starts.size(); i++) {
                    if (mConfig.startsByLength[k].includes(mConfig.strings[i])) u = pb.createOr(u, starts[i]);
                }
                pb.createAssign(pb.createExtract(firsts, pb.getInteger(k)), pb.createInFile(u));
            }
            return;
        }
        for (size_t i = 0; i < starts.size(); i++) {
            pb.createAssign(pb.createExtract(firsts, pb.getInteger(i)), pb.createInFile(starts[i]));
        }
        return;
    }
    std::vector<PabloAST *> ats;
    for (const FusedSubgroup & g : mConfig.subgroups) {
        const FusedSubgroupPlan plan(g);
        PabloAST * at = g.match >= 0 ? source(g.match)
                        : pb.createAnd(classAt(0, plan.all), contexts((correction && g.converted) ? FusedContexts{FusedContext()} : g.before, g.after, 1));
        if (allowed && !mConfig.groupCoverage) at = pb.createAnd(at, allowed);
        ats.push_back(at);
    }

    //  The insertion rules: where their after contexts match (from the next
    //  position) and they are eligible, and where they apply (given by the rounds
    //  below).
    std::vector<PabloAST *> insAfter, insAt(mConfig.insertions.size(), zeroes);
    for (const FusedInsertion & ins : mConfig.insertions) {
        PabloAST * a = ins.match >= 0 ? source(ins.match) : contexts(FusedContexts{FusedContext()}, ins.after, 1);
        if (ins.eligible >= 0) a = pb.createAnd(a, source(ins.eligible));
        insAfter.push_back(pb.createAnd(a, inText));
    }

    //  The results of the rules: the changes at each position i of the
    //  replacements, the insertions, and the coverage, added to the state given.
    struct Results {
        std::vector<std::vector<PabloAST *>> changes;
        std::map<unsigned, PabloAST *> insert;
        PabloAST * spans;
        PabloAST * firsts;
    };
    auto stateIn = [&]() {
        Results res;
        res.changes.assign(mLayout.changePositions, std::vector<PabloAST *>(21, zeroes));
        for (unsigned i = 0; i < mChangesIn.size(); i++) {
            if (mChangesIn[i]) res.changes[i] = getInputStreamSet("ChangesIn" + std::to_string(i));
        }
        for (unsigned k : mLayout.insertCounts) {
            res.insert[k] = mInsertIn.count(k) ? getInputStreamSet("InsertIn" + std::to_string(k))[0] : zeroes;
        }
        res.spans = mCoverageIn ? getInputStreamSet("SpansIn")[0] : zeroes;
        res.firsts = mCoverageIn ? getInputStreamSet("StartsIn")[0] : zeroes;
        return res;
    };
    //  The results of the rules applied at the positions given (the first
    //  positions of the matches of the string rules, and the positions of the
    //  subgroups), added to res.
    //  The run following the text of a rule (within the mask), given the last
    //  positions of its applied matches.
    auto runTail = [&](const FusedStringRule & rule, PabloAST * last) -> PabloAST * {
        PabloAST * runChars = classAt(0, rule.run);
        if (mask) runChars = pb.createAnd(runChars, mask);
        return pb.createAnd(pb.createMatchStar(pb.createAdvance(last, 1), runChars), runChars);
    };
    auto addResults = [&](Results & res, const std::vector<PabloAST *> & firstOf, const std::vector<PabloAST *> & atOf) {
        //  At the positions of At: changing the character there to the target
        //  (the character known if Known), or the deletion mark at an inserted
        //  position.
        auto changeChar = [&](PabloAST * at, UCD::codepoint_t target, const UCD::UnicodeSet * known) {
            if (known && known->count() == 1) {
                const UCD::codepoint_t diff = known->front().first ^ target;
                for (unsigned b = 0; b < 21; b++) {
                    if ((diff >> b) & 1) res.changes[0][b] = pb.createOr(res.changes[0][b], at);
                }
            } else {
                for (unsigned b = 0; b < 21; b++) {
                    PabloAST * const bit = ((target >> b) & 1) ? pb.createNot(basis[b]) : basis[b];
                    res.changes[0][b] = pb.createOr(res.changes[0][b], pb.createAnd(at, bit));
                }
            }
        };
        auto changeMark = [&](unsigned i, PabloAST * at, UCD::codepoint_t target) {
            const UCD::codepoint_t diff = DeletionMark ^ target;
            for (unsigned b = 0; b < 21; b++) {
                if ((diff >> b) & 1) res.changes[i][b] = pb.createOr(res.changes[i][b], at);
            }
        };
        for (size_t r = 0; r < mConfig.strings.size(); r++) {
            const FusedStringRule & rule = mConfig.strings[r];
            const unsigned n = rule.text.size(), m = rule.replacement.size();
            PabloAST * const first = firstOf[r];
            if (rule.keep >= 0) {
                //  The character kept at index keep of the result, and the
                //  replacement around it: the character itself at the position
                //  (if keep = 0) or in an inserted position, written from its bits.
                const unsigned k = static_cast<unsigned>(rule.keep), total = m + 1;
                for (unsigned j = 0; j < total; j++) {
                    if (j == k) {
                        if (j == 0) continue;
                        for (unsigned b = 0; b < 21; b++) {
                            PabloAST * const bit = ((DeletionMark >> b) & 1) ? pb.createNot(basis[b]) : basis[b];
                            res.changes[j][b] = pb.createOr(res.changes[j][b], pb.createAnd(first, bit));
                        }
                        continue;
                    }
                    const UCD::codepoint_t target = static_cast<UCD::codepoint_t>(rule.replacement[j < k ? j : j - 1]);
                    if (j == 0) changeChar(first, target, nullptr); else changeMark(j, first, target);
                }
                if (total > 1) res.insert[total - 1] = pb.createOr(res.insert[total - 1], first);
                continue;
            }
            //  The characters of the text change to those of the replacement (or
            //  the deletion mark), and the further characters of the replacement
            //  are written at the positions inserted after the last.
            PabloAST * last = first;
            for (unsigned j = 0; j < n; j++) {
                PabloAST * const at = j ? pb.createAdvance(first, j) : first;
                if (mConfig.coverage) res.spans = pb.createOr(res.spans, at);
                changeChar(at, j < m ? static_cast<UCD::codepoint_t>(rule.replacement[j]) : DeletionMark, &rule.text[j]);
                last = at;
            }
            for (unsigned j = n; j < m; j++) changeMark(j - n + 1, last, static_cast<UCD::codepoint_t>(rule.replacement[j]));
            if (m > n) res.insert[m - n] = pb.createOr(res.insert[m - n], last);
            if (!rule.run.empty()) {
                //  The run following the text (within the mask) is deleted.
                PabloAST * const tail = runTail(rule, last);
                changeChar(tail, DeletionMark, nullptr);
                if (mConfig.coverage) res.spans = pb.createOr(res.spans, tail);
            }
            if (mConfig.coverage) res.firsts = pb.createOr(res.firsts, first);
        }
        for (size_t g = 0; g < mConfig.subgroups.size(); g++) {
            const FusedSubgroupPlan plan(mConfig.subgroups[g]);
            PabloAST * const at = atOf[g];
            if (plan.multiplexed) {
                //  The number of the mapping of the character, and the sets of
                //  numbers compiled over its bits.
                std::vector<PabloAST *> index;
                for (const UCD::UnicodeSet & set : plan.mux) index.push_back(classAt(0, set));
                cc::Parabix_CC_Compiler_Builder indexCompiler(index);
                auto indexSet = [&](const UCD::UnicodeSet & set) -> PabloAST * {
                    if (set.empty()) return zeroes;
                    return indexCompiler.compileCC(re::makeCC(set, &cc::Unicode), pb);
                };
                for (size_t i = 0; i < plan.indexBits.size(); i++) {
                    for (unsigned c = 0; c < 21; c++) {
                        PabloAST * bit = indexSet(plan.indexBits[i][c]);
                        if (i == 0) bit = pb.createXor(bit, basis[c]);
                        if (bit != zeroes) res.changes[i][c] = pb.createOr(res.changes[i][c], pb.createAnd(at, bit));
                    }
                }
                for (const auto & k : plan.insertIndices) {
                    res.insert[k.first] = pb.createOr(res.insert[k.first], pb.createAnd(at, indexSet(k.second)));
                }
                continue;
            }
            for (size_t i = 0; i < plan.bitSets.size(); i++) {
                for (unsigned b = 0; b < 21; b++) {
                    const UCD::UnicodeSet & set = plan.bitSets[i][b];
                    if (!set.empty()) res.changes[i][b] = pb.createOr(res.changes[i][b], pb.createAnd(at, classAt(0, set)));
                }
            }
            for (const FusedSubgroupPlan::Large & l : plan.large) {
                PabloAST * const charsAt = pb.createAnd(at, classAt(0, l.chars));
                if (l.position == 0) changeChar(charsAt, l.target, nullptr); else changeMark(l.position, charsAt, l.target);
            }
            for (const auto & k : plan.byInsertion) {
                res.insert[k.first] = pb.createOr(res.insert[k.first], pb.createAnd(at, classAt(0, k.second)));
            }
        }
    };

    //  The deletion mark in slot 0 of the converted text of the positions (the
    //  character there, changed as given).
    auto markAt = [&](const std::vector<PabloAST *> & changes0) {
        PabloAST * m = inText;
        for (unsigned b = 0; b < 21; b++) {
            PabloAST * const bit = pb.createXor(basis[b], changes0[b]);
            m = pb.createAnd(m, ((DeletionMark >> b) & 1) ? bit : pb.createNot(bit));
        }
        return m;
    };
    //  The texts of the insertion rules applying at the positions given, added
    //  after the converted text of the positions in res: after its k + 1
    //  characters (k inserted after the character), or, where the character is
    //  deleted, from slot 0 (replacing the deletion mark).
    auto addInsertions = [&](Results & res, const std::vector<PabloAST *> & at) {
        if (mConfig.insertions.empty()) return;
        PabloAST * anyInsert = zeroes;
        for (const auto & k : res.insert) anyInsert = pb.createOr(anyInsert, k.second);
        PabloAST * const deleted = pb.createAnd(pb.createNot(anyInsert), markAt(res.changes[0]));
        const std::map<unsigned, PabloAST *> baseInsert = res.insert;
        std::map<unsigned, PabloAST *> withCount = baseInsert;    // the positions with k inserted after them
        withCount[0] = pb.createAnd(pb.createNot(anyInsert), pb.createNot(deleted));
        auto insertAt = [&](unsigned k) -> PabloAST *& {
            PabloAST *& v = res.insert[k];
            if (v == nullptr) v = zeroes;
            return v;
        };
        auto writeMark = [&](unsigned i, PabloAST * where, UCD::codepoint_t target) {
            const UCD::codepoint_t diff = DeletionMark ^ target;
            for (unsigned b = 0; b < 21; b++) {
                if ((diff >> b) & 1) res.changes[i][b] = pb.createXor(res.changes[i][b], where);
            }
        };
        for (size_t i = 0; i < mConfig.insertions.size(); i++) {
            const std::u32string & text = mConfig.insertions[i].text;
            const unsigned m = text.size();
            PabloAST * const d = pb.createAnd(at[i], deleted);
            for (unsigned j = 0; j < m; j++) writeMark(j, d, static_cast<UCD::codepoint_t>(text[j]));
            if (m > 1) insertAt(m - 1) = pb.createOr(insertAt(m - 1), d);
            for (const auto & k : withCount) {
                //  (The counts beyond those of the rules are only those of insertions.)
                if (k.first + m >= res.changes.size()) continue;
                PabloAST * const a = pb.createAnd(at[i], k.second);
                for (unsigned j = 0; j < m; j++) writeMark(k.first + 1 + j, a, static_cast<UCD::codepoint_t>(text[j]));
                if (k.first > 0) insertAt(k.first) = pb.createAnd(insertAt(k.first), pb.createNot(a));
                insertAt(k.first + m) = pb.createOr(insertAt(k.first + m), a);
            }
        }
    };

    std::vector<PabloAST *> firstOf = starts, atOf = ats;
    //  In a correction kernel for a whole group, given the matches of the string
    //  rules and the positions where the rules of the subgroups match: the matches
    //  that apply (selected as in SelectStringMatches) and the positions where the
    //  subgroups apply (not covered by them, and within the mask).
    PabloAST * const odd = pb.createRepeat(1, pb.getInteger(0xAA, 8));
    //  In a correction kernel: the positions within the matches applied in the
    //  previous round, after their first positions.  A before context
    //  matched against the converted text may let a string rule match within an
    //  applied match, where ICU applies no rule; such matches are dropped,
    //  round by round (as the matches covering them may themselves be dropped).
    PabloAST * appliedWithin = nullptr;
    //  The positions within the applied matches of a rule after their first:
    //  of its text, and its run.
    auto withinMatches = [&](size_t r, PabloAST * first) -> PabloAST * {
        const FusedStringRule & rule = mConfig.strings[r];
        PabloAST * w = zeroes;
        for (unsigned j = 1; j < rule.text.size(); j++) w = pb.createOr(w, pb.createAdvance(first, j));
        if (!rule.run.empty()) {
            PabloAST * const last = rule.text.size() > 1 ? pb.createAdvance(first, rule.text.size() - 1) : first;
            w = pb.createOr(w, runTail(rule, last));
        }
        return w;
    };
    auto settle = [&]() {
        if (appliedWithin) {
            for (size_t r = 0; r < mConfig.strings.size(); r++) firstOf[r] = pb.createAnd(firstOf[r], pb.createNot(appliedWithin));
        }
        if (!mConfig.selection.empty()) {
            std::vector<PabloAST *> within;
            for (size_t i = 0; i < mConfig.selection.size(); i++) {
                const FusedRulesConfig::Selection & sel = mConfig.selection[i];
                PabloAST * matches = firstOf[i];
                for (size_t p : sel.predecessors) matches = pb.createAnd(matches, pb.createNot(within[p]));
                if (sel.selfOverlapping) {
                    PabloAST * const runFirst = pb.createAnd(matches, pb.createNot(pb.createAdvance(matches, 1)));
                    PabloAST * const firstOdd = pb.createAnd(runFirst, odd);
                    PabloAST * const firstEven = pb.createXor(runFirst, firstOdd);
                    PabloAST * const oddSelected = pb.createAnd(pb.createMatchStar(firstOdd, matches), odd);
                    PabloAST * const evenSelected = pb.createAnd(pb.createMatchStar(firstEven, matches), pb.createNot(odd));
                    matches = pb.createAnd(pb.createOr(oddSelected, evenSelected), matches);
                }
                PabloAST * w = zeroes;
                for (unsigned j = 1; j < sel.length; j++) w = pb.createOr(w, pb.createAdvance(matches, j));
                within.push_back(w);
                firstOf[i] = matches;
            }
        }
        if (mConfig.groupCoverage) {
            //  Covered: within the applied matches of the string rules, other than
            //  at their first positions.
            PabloAST * inside = zeroes;
            PabloAST * first = zeroes;
            for (size_t r = 0; r < mConfig.strings.size(); r++) {
                inside = pb.createOr(inside, withinMatches(r, firstOf[r]));
                first = pb.createOr(first, firstOf[r]);
            }
            PabloAST * uncovered = pb.createInFile(pb.createNot(pb.createAnd(inside, pb.createNot(first))));
            if (mask) uncovered = pb.createAnd(uncovered, mask);
            for (size_t g = 0; g < atOf.size(); g++) atOf[g] = pb.createAnd(atOf[g], uncovered);
        }
        if (correction) {
            PabloAST * inside = zeroes;
            for (size_t r = 0; r < mConfig.strings.size(); r++) {
                inside = pb.createOr(inside, withinMatches(r, firstOf[r]));
            }
            appliedWithin = inside;
        }
    };
    if (correction) {
        PabloAST * const firstPosition = beforeStart(1);
        const UCD::UnicodeSet replaced = mConfig.replacementChars - UCD::UnicodeSet(DeletionMark);
        //  The before contexts of the rules matched against the converted text
        //  given by results (with no results, the text itself).
        auto matchConverted = [&](const Results & res, const Results & base) {
            //  The converted text at each position: its length (0 if deleted, or
            //  1 + the positions inserted after it) and its characters (slot 0, the
            //  character itself, and the slots inserted after it).
            unsigned maxInsert = 0;
            PabloAST * anyInsert = zeroes;
            for (const auto & k : res.insert) {
                maxInsert = std::max(maxInsert, k.first);
                anyInsert = pb.createOr(anyInsert, k.second);
            }
            std::vector<std::vector<PabloAST *>> slot(maxInsert + 1, std::vector<PabloAST *>(21));
            PabloAST * changed0 = zeroes;
            for (unsigned b = 0; b < 21; b++) {
                slot[0][b] = pb.createXor(basis[b], res.changes[0][b]);
                changed0 = pb.createOr(changed0, res.changes[0][b]);
            }
            for (unsigned i = 1; i <= maxInsert; i++) {
                for (unsigned b = 0; b < 21; b++) {
                    PabloAST * const c = i < res.changes.size() ? res.changes[i][b] : zeroes;
                    slot[i][b] = ((DeletionMark >> b) & 1) ? pb.createNot(c) : c;
                }
            }
            std::vector<std::unique_ptr<cc::Parabix_CC_Compiler_Builder>> slotCompiler(maxInsert + 1);
            for (unsigned i = 0; i <= maxInsert; i++) slotCompiler[i] = std::make_unique<cc::Parabix_CC_Compiler_Builder>(slot[i]);
            auto slotClass = [&](unsigned i, const UCD::UnicodeSet & set) -> PabloAST * {
                if (set.empty()) return zeroes;
                return pb.createInFile(slotCompiler[i]->compileCC(re::makeCC(set, &cc::Unicode), pb));
            };
            PabloAST * const deleted = pb.createAnd(pb.createNot(anyInsert),
                                                    pb.createAnd(changed0, slotClass(0, UCD::UnicodeSet(DeletionMark))));
            PabloAST * const kept = pb.createInFile(pb.createNot(deleted));
            std::vector<PabloAST *> length(maxInsert + 2, zeroes);     // length[l]: the converted text has l characters
            length[1] = pb.createAnd(kept, pb.createNot(anyInsert));
            for (const auto & k : res.insert) length[k.first + 1] = pb.createOr(length[k.first + 1], k.second);
            PabloAST * const noneKeptBefore = pb.createInFile(pb.createNot(pb.createMatchStar(pb.createAdvance(kept, 1), ones)));
            //  Whether the j-th last character of the converted text up to each
            //  position (j = 1, its last) is in a set (or, before the start of the
            //  text, whether the item holds there), for j = 1 .. depth.
            std::map<std::pair<std::string, bool>, std::vector<PabloAST *>> memo;
            auto lastIn = [&](const FusedItem & item, unsigned depth) -> PabloAST * {
                std::vector<PabloAST *> & V = memo[{setKey(item.chars), item.boundary}];
                const UCD::UnicodeSet inReplaced = item.chars & replaced;
                auto slotIn = [&](unsigned i) -> PabloAST * {
                    if (i == 0) {
                        return pb.createOr(pb.createAnd(pb.createNot(changed0), classAt(0, item.chars)),
                                           pb.createAnd(changed0, slotClass(0, inReplaced)));
                    }
                    return slotClass(i, inReplaced);
                };
                auto previous = [&](PabloAST * v) {
                    PabloAST * p = pb.createAdvance(v, 1);
                    return item.boundary ? pb.createOr(p, firstPosition) : p;
                };
                while (V.size() < depth) {
                    const unsigned j = V.size() + 1;
                    PabloAST * x = zeroes;
                    for (unsigned l = 1; l < length.size(); l++) {
                        if (length[l] == zeroes) continue;
                        PabloAST * const v = l >= j ? slotIn(l - j) : previous(V[j - l - 1]);
                        x = pb.createOr(x, pb.createAnd(length[l], v));
                    }
                    //  A deleted position: as the position before it.
                    PabloAST * carried = pb.createMatchStar(pb.createAnd(pb.createAdvance(x, 1), deleted), deleted);
                    if (item.boundary) carried = pb.createOr(carried, noneKeptBefore);
                    V.push_back(pb.createOr(x, pb.createAnd(deleted, carried)));
                }
                return V[depth - 1];
            };
            //  The positions at which a before context matches the converted text
            //  before them.
            //  Whether a character of the converted text (slot i of a position) is
            //  in a set: the character of the text itself if unchanged, or else one
            //  of the replacement characters.
            auto memberOf = [&](unsigned i, const UCD::UnicodeSet & set) -> PabloAST * {
                const UCD::UnicodeSet inReplaced = set & replaced;
                if (i == 0) {
                    return pb.createOr(pb.createAnd(pb.createNot(changed0), classAt(0, set)),
                                       pb.createAnd(changed0, slotClass(0, inReplaced)));
                }
                return slotClass(i, inReplaced);
            };
            //  For a possessive repetition of S at depth j, with T beyond it: whether,
            //  going back from the j-th last character of the converted text up to
            //  each position over the characters in S, the first other character is
            //  in T (or there is none, and T holds beyond the text).
            std::map<std::tuple<std::string, std::string, bool>, std::vector<PabloAST *>> runMemo;
            auto runIn = [&](const UCD::UnicodeSet & S, const FusedItem & T, unsigned depth) -> PabloAST * {
                std::vector<PabloAST *> & U = runMemo[std::make_tuple(setKey(S), setKey(T.chars), T.boundary)];
                std::vector<PabloAST *> inS(length.size()), inT(length.size());
                for (unsigned i = 0; i + 1 < length.size(); i++) {
                    inS[i] = memberOf(i, S);
                    inT[i] = memberOf(i, T.chars);
                }
                //  Of the slots 0 .. top of a position: the first other than S going
                //  back from top is in T; all are in S.
                auto within = [&](unsigned top) {
                    PabloAST * found = zeroes;
                    PabloAST * allS = ones;
                    for (int i = static_cast<int>(top); i >= 0; i--) {
                        found = pb.createOr(found, pb.createAnd(allS, pb.createAnd(pb.createNot(inS[i]), inT[i])));
                        allS = pb.createAnd(allS, inS[i]);
                    }
                    return std::make_pair(found, allS);
                };
                auto previous = [&](PabloAST * v) {
                    PabloAST * p = pb.createAdvance(v, 1);
                    return T.boundary ? pb.createOr(p, firstPosition) : p;
                };
                if (U.empty()) {
                    PabloAST * x = zeroes;
                    PabloAST * through = deleted;
                    for (unsigned l = 1; l < length.size(); l++) {
                        if (length[l] == zeroes) continue;
                        const auto w = within(l - 1);
                        x = pb.createOr(x, pb.createAnd(length[l], w.first));
                        through = pb.createOr(through, pb.createAnd(length[l], w.second));
                    }
                    PabloAST * carried = pb.createMatchStar(pb.createAdvance(x, 1), through);
                    if (T.boundary) {
                        PabloAST * const noneBefore = pb.createInFile(pb.createNot(
                            pb.createMatchStar(pb.createAdvance(pb.createInFile(pb.createNot(through)), 1), ones)));
                        carried = pb.createOr(carried, noneBefore);
                    }
                    U.push_back(pb.createOr(x, pb.createAnd(through, carried)));
                }
                while (U.size() < depth) {
                    const unsigned j = U.size() + 1;
                    PabloAST * y = zeroes;
                    for (unsigned l = 1; l < length.size(); l++) {
                        if (length[l] == zeroes) continue;
                        PabloAST * v;
                        if (l >= j) {
                            const auto w = within(l - j);
                            v = pb.createOr(w.first, pb.createAnd(w.second, previous(U[0])));
                        } else {
                            v = previous(U[j - l - 1]);
                        }
                        y = pb.createOr(y, pb.createAnd(length[l], v));
                    }
                    PabloAST * carried = pb.createMatchStar(pb.createAnd(pb.createAdvance(y, 1), deleted), deleted);
                    if (T.boundary) carried = pb.createOr(carried, noneKeptBefore);
                    U.push_back(pb.createOr(y, pb.createAnd(deleted, carried)));
                }
                return U[depth - 1];
            };
            auto convertedBefore = [&](const FusedContexts & before) {
                PabloAST * match = zeroes;
                for (const FusedContext & alt : before) {
                    PabloAST * m = ones;
                    const unsigned B = alt.size();
                    for (unsigned k = 0; k < B; k++) {
                        const unsigned d = B - k;
                        if (k + 1 < B && alt[k + 1].star) {
                            //  The item beyond a repetition: matched with it.
                            continue;
                        }
                        PabloAST * item;
                        bool boundary = alt[k].boundary;
                        if (alt[k].star) {
                            item = pb.createAdvance(runIn(alt[k].chars, alt[k - 1], d), 1);
                            boundary = alt[k - 1].boundary;
                        } else {
                            item = pb.createAdvance(lastIn(alt[k], d), 1);
                        }
                        if (boundary) item = pb.createOr(item, firstPosition);
                        m = pb.createAnd(m, item);
                    }
                    match = pb.createOr(match, m);
                }
                return match;
            };
            for (size_t r = 0; r < mConfig.strings.size(); r++) {
                firstOf[r] = mConfig.strings[r].converted ? pb.createAnd(starts[r], convertedBefore(mConfig.strings[r].before)) : starts[r];
            }
            for (size_t g = 0; g < mConfig.subgroups.size(); g++) {
                atOf[g] = mConfig.subgroups[g].converted ? pb.createAnd(ats[g], convertedBefore(mConfig.subgroups[g].before)) : ats[g];
            }
            //  The insertion rules: their before contexts matched against the converted
            //  text up to and including each position, as in res but without the text
            //  inserted at the position itself (as in base, where the rules other than
            //  the insertions apply): its characters (those of base), then the
            //  converted text before it (as in res).
            if (!mConfig.insertions.empty()) {
                PabloAST * baseAnyInsert = zeroes;
                for (const auto & k : base.insert) baseAnyInsert = pb.createOr(baseAnyInsert, k.second);
                const std::vector<PabloAST *> baseChanges0 = base.changes.empty() ? std::vector<PabloAST *>(21, zeroes) : base.changes[0];
                PabloAST * const baseDeleted = pb.createAnd(pb.createNot(baseAnyInsert), markAt(baseChanges0));
                std::map<unsigned, PabloAST *> baseLength;      // l: the position has l characters
                baseLength[1] = pb.createAnd(inText, pb.createNot(pb.createOr(baseAnyInsert, baseDeleted)));
                for (const auto & k : base.insert) baseLength[k.first + 1] = k.second;
                auto ownExcluded = [&](const FusedItem & item, unsigned j) -> PabloAST * {
                    lastIn(item, j);
                    const std::vector<PabloAST *> & V = memo[{setKey(item.chars), item.boundary}];
                    auto previous = [&](PabloAST * v) {
                        PabloAST * p = pb.createAdvance(v, 1);
                        return item.boundary ? pb.createOr(p, firstPosition) : p;
                    };
                    PabloAST * x = pb.createAnd(baseDeleted, previous(V[j - 1]));
                    for (const auto & l : baseLength) {
                        PabloAST * const v = l.first >= j ? memberOf(l.first - j, item.chars) : previous(V[j - l.first - 1]);
                        x = pb.createOr(x, pb.createAnd(l.second, v));
                    }
                    return x;
                };
                //  As runIn(S, T, j), at the j-th last character of the converted text
                //  up to and including each position, without its own insertion.
                auto ownExcludedRun = [&](const UCD::UnicodeSet & S, const FusedItem & T, unsigned j) -> PabloAST * {
                    runIn(S, T, j);
                    const std::vector<PabloAST *> & U = runMemo[std::make_tuple(setKey(S), setKey(T.chars), T.boundary)];
                    auto previous = [&](PabloAST * v) {
                        PabloAST * p = pb.createAdvance(v, 1);
                        return T.boundary ? pb.createOr(p, firstPosition) : p;
                    };
                    PabloAST * x = pb.createAnd(baseDeleted, previous(U[j - 1]));
                    for (const auto & l : baseLength) {
                        PabloAST * v;
                        if (l.first >= j) {
                            //  Going back over the characters of the position from its
                            //  (l - j)-th: the first not in S is in T, or all are in S.
                            PabloAST * found = zeroes;
                            PabloAST * allS = ones;
                            for (int i = static_cast<int>(l.first - j); i >= 0; i--) {
                                PabloAST * const inS = memberOf(i, S);
                                found = pb.createOr(found, pb.createAnd(allS, pb.createAnd(pb.createNot(inS), memberOf(i, T.chars))));
                                allS = pb.createAnd(allS, inS);
                            }
                            v = pb.createOr(found, pb.createAnd(allS, previous(U[0])));
                        } else {
                            v = previous(U[j - l.first - 1]);
                        }
                        x = pb.createOr(x, pb.createAnd(l.second, v));
                    }
                    return x;
                };
                PabloAST * applied = zeroes;
                for (size_t i = 0; i < mConfig.insertions.size(); i++) {
                    PabloAST * match = zeroes;
                    for (const FusedContext & alt : mConfig.insertions[i].before) {
                        //  (As convertedBefore, at the position itself.)
                        PabloAST * m = ones;
                        const unsigned B = alt.size();
                        for (unsigned k = 0; k < B; k++) {
                            const unsigned d = B - k;
                            if (k + 1 < B && alt[k + 1].star) continue;     // matched with the repetition
                            m = pb.createAnd(m, alt[k].star ? ownExcludedRun(alt[k].chars, alt[k - 1], d) : ownExcluded(alt[k], d));
                        }
                        match = pb.createOr(match, m);
                    }
                    insAt[i] = pb.createAnd(pb.createAnd(insAfter[i], match), pb.createNot(applied));
                    applied = pb.createOr(applied, insAt[i]);
                }
            }
        };
        Results none;
        none.changes.assign(mLayout.changePositions, std::vector<PabloAST *>(21, zeroes));
        if (mConfig.staged) {
            //  One round, against the converted text of the previous round.
            const Results in = stateIn();
            matchConverted(in, in);
            settle();
            if (mConfig.matchOnly) {
                Var * const firsts = getOutputStreamVar("Firsts");
                for (size_t k = 0; k < mConfig.startsByLength.size(); k++) {
                    PabloAST * u = zeroes;
                    for (size_t i = 0; i < firstOf.size(); i++) {
                        if (mConfig.startsByLength[k].includes(mConfig.strings[i])) u = pb.createOr(u, firstOf[i]);
                    }
                    pb.createAssign(pb.createExtract(firsts, pb.getInteger(k)), pb.createInFile(u));
                }
                return;
            }
            Results res = none;
            for (unsigned k : mLayout.insertCounts) res.insert[k] = zeroes;
            res.spans = zeroes;
            res.firsts = zeroes;
            addResults(res, firstOf, atOf);
            for (unsigned i = 0; i < mLayout.changePositions; i++) writeOutputStreamSet("Changes" + std::to_string(i), res.changes[i]);
            for (unsigned k : mLayout.insertCounts) writeOutputStreamSet("Insert" + std::to_string(k), std::vector<PabloAST *>{res.insert[k]});
            return;
        }
        //  Round 0: the before contexts matched against the text itself.
        matchConverted(none, none);
        settle();
        //  Each further round: the converted text from the results so far (with the
        //  texts inserted), and the before contexts matched against it.  (A rule
        //  whose before context sees the result of another such rule is right after
        //  as many rounds as such rules precede it in a chain.)
        for (unsigned round = 1; round <= mConfig.rounds; round++) {
            Results base = stateIn();
            addResults(base, firstOf, atOf);
            Results res = base;
            addInsertions(res, insAt);
            matchConverted(res, base);
            settle();
        }
    }

    Results res = stateIn();
    addResults(res, firstOf, atOf);
    addInsertions(res, insAt);
    for (unsigned i = 0; i < mLayout.changePositions; i++) writeOutputStreamSet("Changes" + std::to_string(i), res.changes[i]);
    for (unsigned k : mLayout.insertCounts) writeOutputStreamSet("Insert" + std::to_string(k), std::vector<PabloAST *>{res.insert[k]});
    if (mConfig.coverage) {
        writeOutputStreamSet("Spans", std::vector<PabloAST *>{res.spans});
        writeOutputStreamSet("Starts", std::vector<PabloAST *>{res.firsts});
    }
}

//  The chunks of the string rules of a conversion group for fused kernels:
//  the rules by (text length, replacement length), the buckets with fewer than
//  FusedSmallBucket rules merged with their neighbours of the same text length,
//  and at most FusedMaxRules rules per chunk.
static constexpr size_t FusedSmallBucket = 16;
static constexpr size_t FusedMaxRules = 256;
static constexpr size_t FusedMaxSubgroups = 64;

static std::vector<std::vector<size_t>> fusedStringChunks(const std::vector<std::pair<size_t, size_t>> & lengths,
                                                         const std::vector<size_t> & rules) {
    std::map<size_t, std::map<size_t, std::vector<size_t>>> buckets;     // n → m → rules
    for (size_t r : rules) buckets[lengths[r].first][lengths[r].second].push_back(r);
    std::vector<std::vector<size_t>> chunks;
    for (const auto & byN : buckets) {
        //  A bucket of FusedSmallBucket rules or more starts a chunk, unless the
        //  chunk so far has only small buckets, which join it.
        std::vector<size_t> current;
        bool currentHasLarge = false;
        for (const auto & byM : byN.second) {
            const std::vector<size_t> & bucket = byM.second;
            const bool large = bucket.size() >= FusedSmallBucket;
            if (large && currentHasLarge) {
                chunks.push_back(std::move(current));
                current.clear();
                currentHasLarge = false;
            }
            current.insert(current.end(), bucket.begin(), bucket.end());
            currentHasLarge = currentHasLarge || large;
            while (current.size() > FusedMaxRules) {
                chunks.emplace_back(current.begin(), current.begin() + FusedMaxRules);
                current.erase(current.begin(), current.begin() + FusedMaxRules);
            }
        }
        if (!current.empty()) chunks.push_back(std::move(current));
    }
    return chunks;
}

//  The walk over the text of the matches of a MultipleCaptureMultipleReplacement
//  set whose region is moved (see ldml::MultipleCaptureMultipleReplacementSet::
//  moved): from the start of each match (Starts), the positions of the first
//  characters of captures i and j and the position after capture j (Marks,
//  three streams).  The segments are the texts before the captures (of fixed
//  lengths) and the captures.
struct CaptureWalkConfig {
    std::vector<unsigned> textLengths;              // T0 ... Tk
    std::vector<std::vector<WalkItem>> captures;    // B1 ... Bk
    unsigned first;                                 // i (0-based)
    unsigned moved;                                 // j
    unsigned lookahead() const {
        unsigned l = 0;
        for (const auto & c : captures) {
            for (const WalkItem & w : c) {
                for (const auto & str : w.strings) l = std::max<unsigned>(l, str.size() - 1);
            }
        }
        return l;
    }
    std::string signature() const {
        std::string s = "W" + std::to_string(first) + "." + std::to_string(moved);
        auto set = [&](const UCD::UnicodeSet & u) {
            s += "[";
            for (const auto & r : u) s += std::to_string(r.first) + "-" + std::to_string(r.second) + ",";
            s += "]";
        };
        for (size_t c = 0; c < captures.size(); c++) {
            s += "|t" + std::to_string(textLengths[c]) + "|c";
            for (const WalkItem & w : captures[c]) {
                s += std::to_string(static_cast<int>(w.kind));
                if (w.kind == WalkItem::Kind::Strings) {
                    for (const auto & str : w.strings) {
                        s += "(";
                        for (const auto & u : str) set(u);
                        s += ")";
                    }
                } else {
                    set(w.set);
                }
            }
        }
        return s + "|t" + std::to_string(textLengths.back());
    }
};

class CaptureWalk final : public pablo::PabloKernel {
public:
    CaptureWalk(LLVMTypeSystemInterface & ts, CaptureWalkConfig config, StreamSet * U21, StreamSet * Starts, StreamSet * Marks)
    : PabloKernel(ts, "CaptureWalk_" + getStringHash(config.signature()),
                  {Binding{"U21", U21, FixedRate(1), LookAhead(std::max(1u, config.lookahead()))}, Binding{"Starts", Starts}},
                  {Binding{"Marks", Marks}}),
      mConfig(std::move(config)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> basis = getInputStreamSet("U21");
        std::vector<std::unique_ptr<cc::Parabix_CC_Compiler_Builder>> ahead(mConfig.lookahead() + 1);
        auto cls = [&](unsigned k, const UCD::UnicodeSet & set) -> PabloAST * {
            if (!ahead[k]) {
                std::vector<PabloAST *> bits(basis.size());
                for (unsigned b = 0; b < basis.size(); b++) bits[b] = k ? pb.createLookahead(basis[b], k) : basis[b];
                ahead[k] = std::make_unique<cc::Parabix_CC_Compiler_Builder>(bits);
            }
            return ahead[k]->compileCC(re::makeCC(set, &cc::Unicode), pb);
        };
        PabloAST * cursor = getInputStreamSet("Starts")[0];
        PabloAST * startI = nullptr, * startJ = nullptr, * afterJ = nullptr;
        for (size_t c = 0; c <= mConfig.captures.size(); c++) {
            if (mConfig.textLengths[c]) cursor = pb.createAdvance(cursor, mConfig.textLengths[c]);
            if (c == mConfig.captures.size()) break;
            if (c == mConfig.first) startI = cursor;
            if (c == mConfig.moved) startJ = cursor;
            for (const WalkItem & w : mConfig.captures[c]) {
                switch (w.kind) {
                    case WalkItem::Kind::Set:
                        cursor = pb.createAdvance(pb.createAnd(cursor, cls(0, w.set)), 1);
                        break;
                    case WalkItem::Kind::Optional: {
                        PabloAST * const in = cls(0, w.set);
                        cursor = pb.createOr(pb.createAdvance(pb.createAnd(cursor, in), 1), pb.createAnd(cursor, pb.createNot(in)));
                        break;
                    }
                    case WalkItem::Kind::Star: {
                        PabloAST * const in = cls(0, w.set);
                        cursor = pb.createAnd(pb.createMatchStar(cursor, in), pb.createNot(in));
                        break;
                    }
                    case WalkItem::Kind::Strings: {
                        PabloAST * taken = pb.createZeroes();
                        PabloAST * next = pb.createZeroes();
                        for (const auto & str : w.strings) {
                            PabloAST * m = pb.createAnd(cursor, pb.createNot(taken));
                            for (unsigned k = 0; k < str.size(); k++) m = pb.createAnd(m, cls(k, str[k]));
                            next = pb.createOr(next, pb.createAdvance(m, str.size()));
                            taken = pb.createOr(taken, m);
                        }
                        cursor = next;
                        break;
                    }
                }
            }
            if (c == mConfig.moved) afterJ = cursor;
        }
        Var * const marks = getOutputStreamVar("Marks");
        pb.createAssign(pb.createExtract(marks, pb.getInteger(0)), startI);
        pb.createAssign(pb.createExtract(marks, pb.getInteger(1)), startJ);
        pb.createAssign(pb.createExtract(marks, pb.getInteger(2)), afterJ);
    }
private:
    const CaptureWalkConfig mConfig;
};

//  The regions moved (see ldml::MultipleCaptureMultipleReplacementSet::moved),
//  given the first position of each (XStart), and the first (JStart) and last
//  (JEnd) positions of capture j after it: the region (X), capture j (J), and
//  the number of positions of the region (up to 64) at the last position of
//  capture j, given the run index of the region (Index).
class MoveRegions final : public pablo::PabloKernel {
public:
    MoveRegions(LLVMTypeSystemInterface & ts, StreamSet * XStart, StreamSet * JStart, StreamSet * JEnd, StreamSet * X, StreamSet * J)
    : PabloKernel(ts, "MoveRegions", {Binding{"XStart", XStart}, Binding{"JStart", JStart}, Binding{"JEnd", JEnd}},
                  {Binding{"X", X}, Binding{"J", J}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const jStart = getInputStreamSet("JStart")[0];
        PabloAST * const x = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {getInputStreamSet("XStart")[0], jStart});
        PabloAST * const j = pb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, {jStart, getInputStreamSet("JEnd")[0]});
        pb.createAssign(pb.createExtract(getOutputStreamVar("X"), pb.getInteger(0)), x);
        pb.createAssign(pb.createExtract(getOutputStreamVar("J"), pb.getInteger(0)), j);
    }
};

class MoveCounts final : public pablo::PabloKernel {
public:
    MoveCounts(LLVMTypeSystemInterface & ts, StreamSet * X, StreamSet * Index, StreamSet * JStart, StreamSet * J,
               StreamSet * JEnd, StreamSet * Counts)
    : PabloKernel(ts, "MoveCounts" + std::to_string(Index->getNumElements()),
                  {Binding{"X", X}, Binding{"Index", Index}, Binding{"JStart", JStart, FixedRate(1), LookAhead(1)},
                   Binding{"J", J}, Binding{"JEnd", JEnd}},
                  {Binding{"Counts", Counts}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        PabloAST * const x = getInputStreamSet("X")[0];
        PabloAST * const j = getInputStreamSet("J")[0];
        PabloAST * const jEnd = getInputStreamSet("JEnd")[0];
        PabloAST * const xLast = pb.createAnd(x, pb.createLookahead(getInputStreamSet("JStart")[0], 1));
        const BixNum index = getInputStreamSet("Index");
        //  The length of the region at its last position, carried through capture j.
        BixNum length = bnc.AddModular(bnc.ZeroExtend(index, index.size() + 1), 1);
        Var * const counts = getOutputStreamVar("Counts");
        for (unsigned b = 0; b < length.size(); b++) {
            PabloAST * const bit = pb.createAnd(length[b], xLast);
            pb.createAssign(pb.createExtract(counts, pb.getInteger(b)),
                            pb.createAnd(pb.createMatchStar(pb.createAdvance(bit, 1), j), jEnd));
        }
    }
};

//  The deferred rules of a group (see ldml::DeferredRule), applied in rounds to
//  the converted text.  In each round:
//    - DeferredPlan: at each trigger, the first deferred rule applying, where
//      no before context may reach back to a trigger not yet replaced (its
//      characters between them in deferredReach); the length of the capture it
//      copies, matched as ICU does, right to left from the trigger, possessively
//      (its sets, and the run of the set of its repetition, counted by RunIndex,
//      up to DeferredMaxRun characters, at the end of the run: a rule copying a
//      longer run does not apply); the number of positions to
//      insert after the trigger (the replacement R, then the copy); the
//      positions of the text of the rule (deleted); and, for each distance d up
//      to the longest copy, the triggers whose copies reach back d positions;
//    - DeferredRegion: the regions copied, by looking ahead from each position
//      to the triggers;
//    - the regions are copied into the positions inserted after R (FilterByMask,
//      MergeByMask), and R written (DeferredSlots), and the texts deleted.
constexpr unsigned DeferredBlockingDepth = 8;
//  (As each round may insert positions, the bounds of the buffers of the
//  pipeline grow with the largest number of positions inserted at one, to the
//  power of the number of rounds: the runs are limited to 16 characters.)
constexpr unsigned DeferredRunBits = 4;
constexpr unsigned DeferredMaxRun = 1u << DeferredRunBits;

struct DeferredConfig {
    std::vector<unsigned> textLengths;          // |T| of each rule
    std::vector<std::u32string> replacements;   // R of each rule
    std::vector<int> captures;                  // the capture of each rule, or -1
    std::vector<unsigned> prefix;               // the number of sets of each capture before its repetition
    std::vector<unsigned> suffix;               // and after it
    std::vector<int> starClass;                 // the class of its repetition (index in Classes), or -1
    unsigned numTails = 0;
    bool masked = false;
    bool written = false;
    unsigned maxCopy() const {
        unsigned m = 0;
        for (size_t k = 0; k < prefix.size(); k++) m = std::max(m, prefix[k] + suffix[k] + (starClass[k] >= 0 ? DeferredMaxRun : 0));
        return m;
    }
    //  The number of bits of the number of positions inserted at a trigger.
    unsigned countBits() const {
        size_t r = 0;
        for (const std::u32string & s : replacements) r = std::max(r, s.size());
        const unsigned max = maxCopy() + static_cast<unsigned>(r);
        unsigned bits = 1;
        while ((max >> bits) != 0) bits++;
        return bits;
    }
    std::string signature() const {
        std::string sig = std::to_string(numTails) + "t" + (masked ? "m" : "") + (written ? "w" : "");
        for (size_t k = 0; k < prefix.size(); k++) {
            sig += "/" + std::to_string(prefix[k]) + "," + std::to_string(suffix[k]) + "," + std::to_string(starClass[k]);
        }
        for (size_t i = 0; i < textLengths.size(); i++) {
            sig += "|" + std::to_string(textLengths[i]) + "," + std::to_string(captures[i]) + ",";
            for (char32_t ch : replacements[i]) sig += std::to_string(static_cast<unsigned>(ch)) + ".";
        }
        return sig;
    }
};

//  Classes: the triggers, the tails (numTails), the reach, the sets of the
//  repetitions of the captures.  The triggers, and the characters of the texts
//  after them.
static PabloAST * deferredTexts(PabloBuilder & pb, PabloAST * triggers, const std::vector<PabloAST *> & classes, unsigned numTails) {
    PabloAST * u = triggers;
    for (unsigned j = 0; j < numTails; j++) u = pb.createOr(u, pb.createAnd(pb.createAdvance(triggers, j + 1), classes[1 + j]));
    return u;
}

class DeferredPlan final : public pablo::PabloKernel {
public:
    DeferredPlan(LLVMTypeSystemInterface & ts, DeferredConfig config, const std::vector<StreamSet *> & Matches, StreamSet * Classes,
                 const std::vector<StreamSet *> & Runs, StreamSet * Mask, StreamSet * Written, StreamSet * Count, StreamSet * Info,
                 StreamSet * Reach)
    : PabloKernel(ts, "DeferredPlan_" + getStringHash(config.signature()),
                  [&] {
                      Bindings b;
                      for (size_t i = 0; i < Matches.size(); i++) {
                          b.emplace_back("M" + std::to_string(i), Matches[i], FixedRate(1),
                                         LookAhead(std::max(1u, config.textLengths[i] - 1)));
                      }
                      b.emplace_back("Classes", Classes);
                      for (size_t k = 0; k < Runs.size(); k++) b.emplace_back("Runs" + std::to_string(k), Runs[k]);
                      if (Mask) b.emplace_back("Mask", Mask);
                      if (Written) b.emplace_back("Written", Written);
                      return b;
                  }(),
                  {Binding{"Count", Count}, Binding{"Info", Info}, Binding{"Reach", Reach}}), mConfig(std::move(config)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        const size_t n = mConfig.textLengths.size();
        const unsigned Bits = mConfig.countBits();
        const std::vector<PabloAST *> classes = getInputStreamSet("Classes");
        //  (The text written by the deferred rules in earlier rounds is not
        //  matched again.)
        PabloAST * triggers = classes[0];
        if (mConfig.written) triggers = pb.createAnd(triggers, pb.createNot(getInputStreamSet("Written")[0]));
        PabloAST * const reach = classes[1 + mConfig.numTails];
        //  The length of each capture copied, at the triggers, and where it is not
        //  known (a run of more than 64 characters).
        std::vector<BixNum> lengths;
        std::vector<PabloAST *> unknown;
        unsigned run = 0;
        for (size_t k = 0; k < mConfig.prefix.size(); k++) {
            const unsigned fixed = mConfig.prefix[k] + mConfig.suffix[k];
            BixNum len = bnc.ZeroExtend(bnc.Create(fixed), Bits);
            PabloAST * over = pb.createZeroes();
            if (mConfig.starClass[k] >= 0) {
                const std::vector<PabloAST *> runs = getInputStreamSet("Runs" + std::to_string(run++));
                PabloAST * const inRun = classes[mConfig.starClass[k]];
                BixNum idx(runs.begin(), runs.begin() + DeferredRunBits);
                BixNum count = bnc.AddModular(bnc.ZeroExtend(idx, Bits), 1);
                //  The run ends just before the suffix, which ends just before the trigger.
                const unsigned distance = mConfig.suffix[k] + 1;
                for (unsigned b = 0; b < Bits; b++) count[b] = pb.createAdvance(pb.createAnd(count[b], inRun), distance);
                over = pb.createAdvance(pb.createAnd(runs[DeferredRunBits], inRun), distance);
                len = bnc.AddModular(count, fixed);
            }
            lengths.push_back(len);
            unknown.push_back(over);
        }
        //  The matches of the rules, at the triggers.
        std::vector<PabloAST *> starts;
        PabloAST * matched = pb.createZeroes();
        for (size_t i = 0; i < n; i++) {
            PabloAST * mk = getInputStreamSet("M" + std::to_string(i))[0];
            if (mConfig.textLengths[i] > 1) mk = pb.createLookahead(mk, mConfig.textLengths[i] - 1);
            starts.push_back(mk);
            matched = pb.createOr(matched, mk);
        }
        //  The triggers whose before contexts may not reach back to a trigger not yet
        //  replaced: the triggers that match a rule, or are not ready themselves
        //  (and may match once ready), block those they may reach.  A trigger
        //  that is ready and matches no rule stays as it is.  (Through chains of
        //  up to DeferredBlockingDepth triggers, each blocked by the one before.)
        auto blockedBy = [&](PabloAST * blocking) {
            return pb.createMatchStar(pb.createAdvance(deferredTexts(pb, blocking, classes, mConfig.numTails), 1), reach);
        };
        PabloAST * blocking = pb.createAnd(triggers, matched);
        PabloAST * blocked = blockedBy(blocking);
        for (unsigned j = 0; j < DeferredBlockingDepth; j++) {
            blocking = pb.createAnd(triggers, pb.createOr(matched, blocked));
            blocked = blockedBy(blocking);
        }
        PabloAST * ready = pb.createAnd(triggers, pb.createNot(blocked));
        if (mConfig.masked) ready = pb.createAnd(ready, getInputStreamSet("Mask")[0]);
        //  The first rule matching at each ready trigger (none, if the length of its
        //  capture is not known).
        PabloAST * taken = pb.createZeroes();
        std::vector<PabloAST *> applied;
        for (size_t i = 0; i < n; i++) {
            PabloAST * a = pb.createAnd(pb.createAnd(starts[i], ready), pb.createNot(taken));
            if (mConfig.captures[i] >= 0) a = pb.createAnd(a, pb.createNot(unknown[mConfig.captures[i]]));
            applied.push_back(a);
            taken = pb.createOr(taken, starts[i]);
        }
        BixNum copied = bnc.ZeroExtend(bnc.Create(0), Bits);
        BixNum rlen = bnc.ZeroExtend(bnc.Create(0), Bits);
        PabloAST * copying = pb.createZeroes();
        PabloAST * del = pb.createZeroes();
        for (size_t i = 0; i < n; i++) {
            if (mConfig.captures[i] >= 0) {
                copied = bnc.Select(applied[i], lengths[mConfig.captures[i]], copied);
                copying = pb.createOr(copying, applied[i]);
            }
            if (!mConfig.replacements[i].empty()) {
                rlen = bnc.Select(applied[i], bnc.ZeroExtend(bnc.Create(mConfig.replacements[i].size()), Bits), rlen);
            }
            for (unsigned k = 0; k < mConfig.textLengths[i]; k++) del = pb.createOr(del, pb.createAdvance(applied[i], k));
        }
        const BixNum count = bnc.AddModular(copied, rlen);
        writeOutputStreamSet("Count", std::vector<PabloAST *>(count.begin(), count.end()));
        std::vector<PabloAST *> info(applied.begin(), applied.end());
        info.push_back(del);
        writeOutputStreamSet("Info", info);
        std::vector<PabloAST *> reachOut;
        for (unsigned d = 1; d <= mConfig.maxCopy(); d++) reachOut.push_back(pb.createAnd(copying, bnc.UGE(copied, d)));
        writeOutputStreamSet("Reach", reachOut);
    }
private:
    const DeferredConfig mConfig;
};

//  The regions copied: the positions d positions before a trigger whose copy
//  reaches back d positions.
class DeferredRegion final : public pablo::PabloKernel {
public:
    DeferredRegion(LLVMTypeSystemInterface & ts, StreamSet * Reach, StreamSet * Region)
    : PabloKernel(ts, "DeferredRegion" + std::to_string(Reach->getNumElements()),
                  {Binding{"Reach", Reach, FixedRate(1), LookAhead(Reach->getNumElements())}},
                  {Binding{"Region", Region}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> reach = getInputStreamSet("Reach");
        PabloAST * region = pb.createZeroes();
        for (unsigned d = 1; d <= reach.size(); d++) region = pb.createOr(region, pb.createLookahead(reach[d - 1], d));
        pb.createAssign(pb.createExtract(getOutputStreamVar("Region"), pb.getInteger(0)), region);
    }
};

//  After the insertions: the positions of the copies (Slots), the replacements R
//  written at the positions inserted before them (Chars), and the positions to
//  keep (not those of the texts of the rules applied).
class DeferredSlots final : public pablo::PabloKernel {
public:
    DeferredSlots(LLVMTypeSystemInterface & ts, DeferredConfig config, StreamSet * Info, StreamSet * Spread,
                  StreamSet * Slots, StreamSet * Chars, StreamSet * Keep)
    : PabloKernel(ts, "DeferredSlots_" + getStringHash(config.signature()),
                  {Binding{"Info", Info}, Binding{"Spread", Spread}},
                  {Binding{"Slots", Slots}, Binding{"Chars", Chars}, Binding{"Keep", Keep}}), mConfig(std::move(config)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const size_t n = mConfig.textLengths.size();
        const std::vector<PabloAST *> info = getInputStreamSet("Info");
        PabloAST * const inserted = pb.createNot(getInputStreamSet("Spread")[0]);
        std::vector<PabloAST *> chars(21, pb.createZeroes());
        PabloAST * rSlots = pb.createZeroes();
        for (size_t i = 0; i < n; i++) {
            const std::u32string & r = mConfig.replacements[i];
            for (unsigned k = 0; k < r.size(); k++) {
                PabloAST * const at = pb.createAdvance(info[i], k + 1);
                rSlots = pb.createOr(rSlots, at);
                for (unsigned b = 0; b < 21; b++) {
                    if ((r[k] >> b) & 1) chars[b] = pb.createOr(chars[b], at);
                }
            }
        }
        pb.createAssign(pb.createExtract(getOutputStreamVar("Slots"), pb.getInteger(0)), pb.createAnd(inserted, pb.createNot(rSlots)));
        writeOutputStreamSet("Chars", chars);
        pb.createAssign(pb.createExtract(getOutputStreamVar("Keep"), pb.getInteger(0)), pb.createNot(info[n]));
    }
private:
    const DeferredConfig mConfig;
};

//  The runs of the texts of self-reproducing copy rules (see
//  ldml::DeferredRule::selfReproducing), after a round of the deferred rules:
//  given the text (Text: the codepoint bits, then other streams kept in step
//  with it, such as masks), the blocks written in the round by the
//  self-reproducing rules applied (Marks: [0] their first positions, [1] their
//  positions, then the bits of the rule of each block at its first position,
//  then for each rule the first positions of its texts where it would apply but
//  for its before context), each text that follows a block or a text of the
//  same rule replaced by a copy of the block, which a scalar scan keeps: the rule
//  applies there, its before context being reproduced.  The words without
//  marks are copied whole, shifted by the positions inserted so far.
class DeferredRuns final : public MultiBlockKernel {
public:
    DeferredRuns(LLVMTypeSystemInterface & ts, std::vector<unsigned> textLengths, unsigned maxBlock,
                 StreamSet * Text, StreamSet * Marks, StreamSet * Result)
    : MultiBlockKernel(ts, "DeferredRuns" + std::to_string(Text->getNumElements()) + "_" + std::to_string(maxBlock) + [&] {
                           std::string sig;
                           for (unsigned l : textLengths) sig += "_" + std::to_string(l);
                           return sig;
                       }(),
                       {Binding{"Text", Text}, Binding{"Marks", Marks}},
                       {Binding{"Result", Result, BoundedRate(0, std::max(1u, maxBlock)), EmptyWriteOverflow()}},
                       {}, {},
                       [&] {
                           InternalScalars v{InternalScalar{ts.getInt64Ty(), "capturing"}, InternalScalar{ts.getInt64Ty(), "blockLength"},
                                             InternalScalar{ts.getInt64Ty(), "blockRule"}, InternalScalar{ts.getInt64Ty(), "expected"},
                                             InternalScalar{ts.getInt64Ty(), "skip"}};
                           for (unsigned i = 0; i < Text->getNumElements(); i++) {
                               v.push_back(InternalScalar{ts.getInt64Ty(), "block" + std::to_string(i)});
                           }
                           return v;
                       }()),
      mTextLengths(std::move(textLengths)), mStreams(Text->getNumElements()),
      mRuleBits(Marks->getNumElements() - 2 - static_cast<unsigned>(mTextLengths.size())) {
        setStride(ts.getBitBlockWidth());
    }
protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        Type * const i64Ty = b.getInt64Ty();
        Type * const sizeTy = b.getSizeTy();
        Constant * const W0 = b.getInt64(0);
        Constant * const W1 = b.getInt64(1);
        Constant * const W64 = b.getInt64(64);
        Constant * const NONE = b.getInt64(-1);
        const unsigned N = mStreams;
        const unsigned K = mRuleBits;
        auto toI64 = [&](Value * v) {return b.CreateZExtOrTrunc(v, i64Ty);};
        auto toSize = [&](Value * v) {return b.CreateZExtOrTrunc(v, sizeTy);};
        //  The positions below n (0 to 64).
        auto lowMask = [&](Value * n) {
            Value * const full = b.CreateICmpUGE(n, W64);
            return b.CreateSelect(full, b.getInt64(-1), b.CreateSub(b.CreateShl(W1, b.CreateSelect(full, W0, n)), W1));
        };
        auto inWordPtr = [&](const char * name, unsigned i, Value * pos) {
            return b.CreatePointerCast(b.getRawInputPointer(name, b.getInt32(i), toSize(pos)), PointerType::getUnqual(b.getContext()));
        };
        auto outWordPtr = [&](unsigned i, Value * pos) {
            return b.CreatePointerCast(b.getRawOutputPointer("Result", b.getInt32(i), toSize(pos)), PointerType::getUnqual(b.getContext()));
        };
        //  Writes the n (1 to 64) low bits of bits at output position o of stream i.
        auto emit = [&](unsigned i, Value * bits, Value * n, Value * o) {
            Value * const off = b.CreateAnd(o, b.getInt64(63));
            Value * const base = b.CreateSub(o, off);
            Value * const v = b.CreateAnd(bits, lowMask(n));
            Value * const p0 = outWordPtr(i, base);
            Value * const w0 = b.CreateAnd(b.CreateLoad(i64Ty, p0), lowMask(off));
            b.CreateStore(b.CreateOr(w0, b.CreateShl(v, off)), p0);
            //  The bits beyond the word (none if off is 0).
            Value * const spill = b.CreateLShr(v, b.CreateSub(W64, b.CreateSelect(b.CreateICmpEQ(off, W0), W1, off)));
            Value * const hi = b.CreateSelect(b.CreateICmpEQ(off, W0), W0, spill);
            Value * const p1 = outWordPtr(i, b.CreateAdd(base, W64));
            b.CreateStore(hi, p1);
        };

        Value * const processed = toI64(b.getProcessedItemCount("Text"));
        Value * const produced = toI64(b.getProducedItemCount("Result"));
        Value * const accessible = toI64(b.getAccessibleItemCount("Text"));
        Value * const items = b.CreateUMin(b.CreateMul(toI64(numOfStrides), b.getInt64(b.getBitBlockWidth())), accessible);
        Value * const words = b.CreateUDiv(b.CreateAdd(items, b.getInt64(63)), W64);

        //  The state: the output position, the block kept (capturing, its length,
        //  rule and bits), the position at which a text continues the run (or
        //  NONE), the positions of a text still to skip.
        enum {O, CAP, LEN, RULE, EXP, SKIP, BLOCK};
        std::vector<Value *> state0{produced, b.getScalarField("capturing"), b.getScalarField("blockLength"),
                                    b.getScalarField("blockRule"),
                                    b.CreateSub(b.getScalarField("expected"), W1),     // (stored + 1; 0: none)
                                    b.getScalarField("skip")};
        for (unsigned i = 0; i < N; i++) state0.push_back(b.getScalarField("block" + std::to_string(i)));
        auto phis = [&](const std::vector<Value *> & from, BasicBlock * pred, unsigned n) {
            std::vector<PHINode *> v;
            for (Value * x : from) {
                v.push_back(b.CreatePHI(i64Ty, n));
                v.back()->addIncoming(x, pred);
            }
            return v;
        };
        auto addIncoming = [&](std::vector<PHINode *> & v, const std::vector<Value *> & from, BasicBlock * pred) {
            for (size_t k = 0; k < v.size(); k++) v[k]->addIncoming(from[k], pred);
        };
        auto values = [](const std::vector<PHINode *> & v) {return std::vector<Value *>(v.begin(), v.end());};

        BasicBlock * const entry = b.GetInsertBlock();
        BasicBlock * const wordLoop = b.CreateBasicBlock("wordLoop");
        BasicBlock * const wordBody = b.CreateBasicBlock("wordBody");
        BasicBlock * const fastWord = b.CreateBasicBlock("fastWord");
        BasicBlock * const bitLoop = b.CreateBasicBlock("bitLoop");
        BasicBlock * const bitBody = b.CreateBasicBlock("bitBody");
        BasicBlock * const notSkipping = b.CreateBasicBlock("notSkipping");
        BasicBlock * const chained = b.CreateBasicBlock("chained");
        BasicBlock * const passThrough = b.CreateBasicBlock("passThrough");
        BasicBlock * const bitNext = b.CreateBasicBlock("bitNext");
        BasicBlock * const wordNext = b.CreateBasicBlock("wordNext");
        BasicBlock * const done = b.CreateBasicBlock("done");
        b.CreateBr(wordLoop);

        b.SetInsertPoint(wordLoop);
        PHINode * const w = b.CreatePHI(i64Ty, 2);
        w->addIncoming(W0, entry);
        std::vector<PHINode *> ws = phis(state0, entry, 2);
        b.CreateCondBr(b.CreateICmpULT(w, words), wordBody, done);

        b.SetInsertPoint(wordBody);
        Value * const P = b.CreateAdd(processed, b.CreateMul(w, W64));
        Value * const n = b.CreateUMin(W64, b.CreateSub(items, b.CreateMul(w, W64)));
        std::vector<Value *> text, marks;
        for (unsigned i = 0; i < N; i++) text.push_back(b.CreateLoad(i64Ty, inWordPtr("Text", i, P)));
        Value * anyMark = W0;
        const unsigned S = static_cast<unsigned>(mTextLengths.size());
        for (unsigned i = 0; i < 2 + K + S; i++) {
            marks.push_back(b.CreateLoad(i64Ty, inWordPtr("Marks", i, P)));
            if (i < 2 || i >= 2 + K) anyMark = b.CreateOr(anyMark, marks.back());
        }
        anyMark = b.CreateAnd(anyMark, lowMask(n));
        Value * const fast = b.CreateAnd(b.CreateAnd(b.CreateICmpEQ(anyMark, W0), b.CreateICmpEQ(n, W64)),
                                         b.CreateAnd(b.CreateICmpEQ(ws[CAP], W0), b.CreateICmpEQ(ws[SKIP], W0)));
        b.CreateCondBr(fast, fastWord, bitLoop);

        //  A word without marks: copied whole.
        b.SetInsertPoint(fastWord);
        for (unsigned i = 0; i < N; i++) emit(i, text[i], W64, ws[O]);
        std::vector<Value *> fastState = values(ws);
        fastState[O] = b.CreateAdd(ws[O], W64);
        b.CreateBr(wordNext);

        //  Otherwise, position by position.
        b.SetInsertPoint(bitLoop);
        PHINode * const j = b.CreatePHI(i64Ty, 2);
        j->addIncoming(W0, wordBody);
        std::vector<PHINode *> bs = phis(values(ws), wordBody, 2);
        b.CreateCondBr(b.CreateICmpULT(j, n), bitBody, wordNext);

        b.SetInsertPoint(bitBody);
        auto bit = [&](Value * word) {return b.CreateAnd(b.CreateLShr(word, j), W1);};
        Value * const p = b.CreateAdd(P, j);
        std::vector<Value * > skipState = values(bs);
        skipState[SKIP] = b.CreateSub(bs[SKIP], W1);
        BasicBlock * const skipFrom = b.GetInsertBlock();
        b.CreateCondBr(b.CreateICmpNE(bs[SKIP], W0), bitNext, notSkipping);

        b.SetInsertPoint(notSkipping);
        Value * const blockStart = b.CreateICmpNE(bit(marks[0]), W0);
        Value * const inBlock = b.CreateICmpNE(bit(marks[1]), W0);
        Value * blockRule = W0;
        for (unsigned k = 0; k < K; k++) blockRule = b.CreateOr(blockRule, b.CreateShl(bit(marks[2 + k]), b.getInt64(k)));
        //  The end of the block kept: the next text continues the run here.
        Value * const capturing = b.CreateICmpNE(bs[CAP], W0);
        Value * const ended = b.CreateAnd(capturing, b.CreateOr(b.CreateNot(inBlock), blockStart));
        std::vector<Value *> st = values(bs);
        st[CAP] = b.CreateSelect(ended, W0, st[CAP]);
        st[EXP] = b.CreateSelect(ended, p, st[EXP]);
        //  A new block.
        st[CAP] = b.CreateSelect(blockStart, W1, st[CAP]);
        st[LEN] = b.CreateSelect(blockStart, W0, st[LEN]);
        st[RULE] = b.CreateSelect(blockStart, blockRule, st[RULE]);
        st[EXP] = b.CreateSelect(blockStart, NONE, st[EXP]);
        for (unsigned i = 0; i < N; i++) st[BLOCK + i] = b.CreateSelect(blockStart, W0, st[BLOCK + i]);
        //  A text of the rule of the block kept, where the run continues.
        Value * textOfRule = W0;
        for (unsigned r = 0; r < S; r++) {
            textOfRule = b.CreateSelect(b.CreateICmpEQ(st[RULE], b.getInt64(r)), bit(marks[2 + K + r]), textOfRule);
        }
        Value * const continues = b.CreateAnd(b.CreateAnd(b.CreateICmpNE(textOfRule, W0), b.CreateICmpEQ(st[CAP], W0)),
                                              b.CreateICmpEQ(p, st[EXP]));
        b.CreateCondBr(continues, chained, passThrough);

        //  A text continuing the run: a copy of the block in its place.
        b.SetInsertPoint(chained);
        for (unsigned i = 0; i < N; i++) emit(i, st[BLOCK + i], st[LEN], st[O]);
        Value * length = b.getInt64(mTextLengths.back());
        for (int r = static_cast<int>(mTextLengths.size()) - 2; r >= 0; r--) {
            length = b.CreateSelect(b.CreateICmpEQ(st[RULE], b.getInt64(r)), b.getInt64(mTextLengths[r]), length);
        }
        std::vector<Value *> chainState = st;
        chainState[O] = b.CreateAdd(st[O], st[LEN]);
        chainState[SKIP] = b.CreateSub(length, W1);
        chainState[EXP] = b.CreateAdd(p, length);
        b.CreateBr(bitNext);

        //  Any other position: copied (and kept, within a block).
        b.SetInsertPoint(passThrough);
        std::vector<Value *> passState = st;
        Value * const keep = b.CreateICmpNE(st[CAP], W0);
        for (unsigned i = 0; i < N; i++) {
            Value * const v = bit(text[i]);
            emit(i, v, W1, st[O]);
            passState[BLOCK + i] = b.CreateSelect(keep, b.CreateOr(st[BLOCK + i], b.CreateShl(v, st[LEN])), st[BLOCK + i]);
        }
        passState[LEN] = b.CreateSelect(keep, b.CreateAdd(st[LEN], W1), st[LEN]);
        passState[O] = b.CreateAdd(st[O], W1);
        b.CreateBr(bitNext);

        b.SetInsertPoint(bitNext);
        std::vector<PHINode *> nx = phis(skipState, skipFrom, 3);
        addIncoming(nx, chainState, chained);
        addIncoming(nx, passState, passThrough);
        j->addIncoming(b.CreateAdd(j, W1), bitNext);
        addIncoming(bs, values(nx), bitNext);
        b.CreateBr(bitLoop);

        b.SetInsertPoint(wordNext);
        std::vector<PHINode *> wn = phis(fastState, fastWord, 2);
        addIncoming(wn, values(bs), bitLoop);
        w->addIncoming(b.CreateAdd(w, W1), wordNext);
        addIncoming(ws, values(wn), wordNext);
        b.CreateBr(wordLoop);

        b.SetInsertPoint(done);
        b.setScalarField("capturing", ws[CAP]);
        b.setScalarField("blockLength", ws[LEN]);
        b.setScalarField("blockRule", ws[RULE]);
        b.setScalarField("expected", b.CreateAdd(ws[EXP], W1));
        b.setScalarField("skip", ws[SKIP]);
        for (unsigned i = 0; i < N; i++) b.setScalarField("block" + std::to_string(i), ws[BLOCK + i]);
        b.setProducedItemCount("Result", toSize(ws[O]));
    }
private:
    const std::vector<unsigned> mTextLengths;
    const unsigned mStreams;
    const unsigned mRuleBits;
};

//  The blocks written by the self-reproducing rules applied in a round (after
//  the insertions): for each such rule r (srcIndex of the rule, or -1), the
//  positions inserted after its triggers; their first positions, all their
//  positions, and the bits of r at the first.
class DeferredHeadBlocks final : public pablo::PabloKernel {
public:
    DeferredHeadBlocks(LLVMTypeSystemInterface & ts, std::vector<int> srcIndex, unsigned ruleBits,
                       StreamSet * Info, StreamSet * Spread, StreamSet * Blocks)
    : PabloKernel(ts, "DeferredHeadBlocks_" + [&] {
                      std::string sig = std::to_string(ruleBits);
                      for (int r : srcIndex) sig += "," + std::to_string(r);
                      return getStringHash(sig);
                  }(),
                  {Binding{"Info", Info}, Binding{"Spread", Spread}}, {Binding{"Blocks", Blocks}}),
      mSrcIndex(std::move(srcIndex)), mRuleBits(ruleBits) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> info = getInputStreamSet("Info");
        PabloAST * const inserted = pb.createNot(getInputStreamSet("Spread")[0]);
        PabloAST * starts = pb.createZeroes(), * in = pb.createZeroes();
        std::vector<PabloAST *> rule(mRuleBits, pb.createZeroes());
        for (size_t i = 0; i < mSrcIndex.size(); i++) {
            if (mSrcIndex[i] < 0) continue;
            PabloAST * const first = pb.createAnd(pb.createAdvance(info[i], 1), inserted);
            starts = pb.createOr(starts, first);
            in = pb.createOr(in, pb.createAnd(pb.createMatchStar(first, inserted), inserted));
            for (unsigned k = 0; k < mRuleBits; k++) {
                if ((mSrcIndex[i] >> k) & 1) rule[k] = pb.createOr(rule[k], first);
            }
        }
        std::vector<PabloAST *> out{starts, in};
        out.insert(out.end(), rule.begin(), rule.end());
        writeOutputStreamSet("Blocks", out);
    }
private:
    const std::vector<int> mSrcIndex;
    const unsigned mRuleBits;
};

//  Where a self-reproducing rule would apply but for its before context (at the
//  first positions of its texts): its text and after context match there
//  (Matches, at the last positions of the texts, by the index of the match of
//  each deferred rule), and those of no deferred rule before it, within the mask
//  and not in the text written by the deferred rules: a stream for each
//  self-reproducing rule.
class DeferredCandidates final : public pablo::PabloKernel {
public:
    DeferredCandidates(LLVMTypeSystemInterface & ts, std::vector<int> srcIndex, std::vector<unsigned> matchOf,
                       std::vector<unsigned> textLengths, std::vector<std::vector<size_t>> interferers, unsigned ruleBits,
                       const std::vector<StreamSet *> & Matches, StreamSet * Written, StreamSet * Mask, StreamSet * Candidates)
    : PabloKernel(ts, "DeferredCandidates_" + [&] {
                      std::string sig = std::to_string(ruleBits) + (Mask ? "m" : "");
                      for (size_t i = 0; i < srcIndex.size(); i++) {
                          sig += "," + std::to_string(srcIndex[i]) + "/" + std::to_string(matchOf[i]) + "/" + std::to_string(textLengths[i]) + ":";
                          for (size_t j : interferers[i]) sig += std::to_string(j) + ".";
                      }
                      return getStringHash(sig);
                  }(),
                  [&] {
                      Bindings b;
                      for (size_t k = 0; k < Matches.size(); k++) {
                          unsigned lookahead = 1;
                          for (size_t i = 0; i < matchOf.size(); i++) {
                              if (matchOf[i] == k) lookahead = std::max(lookahead, textLengths[i] - 1);
                          }
                          b.emplace_back("M" + std::to_string(k), Matches[k], FixedRate(1), LookAhead(lookahead));
                      }
                      b.emplace_back("Written", Written);
                      if (Mask) b.emplace_back("Mask", Mask);
                      return b;
                  }(),
                  {Binding{"Candidates", Candidates}}),
      mSrcIndex(std::move(srcIndex)), mMatchOf(std::move(matchOf)), mTextLengths(std::move(textLengths)),
      mInterferers(std::move(interferers)), mRuleBits(ruleBits), mMasked(Mask != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * allowed = pb.createNot(getInputStreamSet("Written")[0]);
        if (mMasked) allowed = pb.createAnd(allowed, getInputStreamSet("Mask")[0]);
        std::vector<PabloAST *> out;
        std::vector<PabloAST *> matches;
        for (size_t i = 0; i < mSrcIndex.size(); i++) {
            PabloAST * m = getInputStreamSet("M" + std::to_string(mMatchOf[i]))[0];
            if (mTextLengths[i] > 1) m = pb.createLookahead(m, mTextLengths[i] - 1);
            matches.push_back(pb.createAnd(m, allowed));
        }
        //  (A stream for each self-reproducing rule, in order.)
        for (size_t i = 0; i < mSrcIndex.size(); i++) {
            if (mSrcIndex[i] < 0) continue;
            PabloAST * c = matches[i];
            for (size_t j : mInterferers[i]) c = pb.createAnd(c, pb.createNot(matches[j]));
            out.push_back(c);
        }
        writeOutputStreamSet("Candidates", out);
    }
private:
    const std::vector<int> mSrcIndex;
    const std::vector<unsigned> mMatchOf;
    const std::vector<unsigned> mTextLengths;
    const std::vector<std::vector<size_t>> mInterferers;
    const unsigned mRuleBits;
    const bool mMasked;
};

class RulePipelineBuilder {
public:
    RulePipelineBuilder(PipelineBuilder & P, const ldml::TransformRegistry & registry, TransformAnalyzer & analyzer)
    : mP(P), mRegistry(registry), mAnalyzer(analyzer), mNFD(P) {}
    // Apply the transform of a registry entry to U21 (an implementable transform).
    StreamSet * transform(const ldml::TransformEntry * entry, StreamSet * U21);
    // Apply an already-analyzed plan (e.g. of an ad hoc --rules-file transform,
    // not tied to a registry entry) to U21.
    StreamSet * applyAnalysis(const TransformAnalysis & a, StreamSet * U21);
private:
    //  Filters.  As in ICU, a filter selects the characters of the text that a
    //  transform may change, once, when the transform is applied: the steps of
    //  the transform then apply only at the positions of these characters (and of
    //  the characters they are replaced by), while contexts match the whole text.
    //  The positions are given by masks, one for each filter in effect (the
    //  innermost last, each within the ones before it), which are kept in step
    //  with the text as positions are inserted and deleted.
    //  Two differences from ICU remain: ICU 74 ignores the global filter of a
    //  transform consisting of a single set of conversion rules (applied here),
    //  and a context added by DisambiguateOrder for a string rule (e.g., the
    //  [^d] of b } [^d] → Z for bd → Y ; b → Z ;) still sees a character
    //  outside the filter that keeps the string rule from applying.
    void pushFilter(const UCD::UnicodeSet & filter, StreamSet * U21);
    void popFilter() {mMasks.pop_back();}
    StreamSet * activeMask() const {return mMasks.empty() ? nullptr : mMasks.back();}
    void spreadMasks(StreamSet * SpreadMask);
    void filterMasks(StreamSet * keep);
    //  A context-free, single-character mapping of U21 (no contexts, and no
    //  characters deleted): without an active mask, simply map(U21).  With a
    //  mask, map is applied only to the masked characters, in a stream
    //  shortened by FilterByMask (as the focused pipelines of nfc/nfd), so
    //  that its kernels run only over the masked part of the text, however
    //  sparse the mask; the mapped result is interleaved back with the
    //  unmasked characters.  lengths gives, for each length other than one
    //  that map's replacements may produce, the masked characters it applies
    //  to (e.g. the digit-length ranges of Any-Hex/Unicode, or a conversion
    //  group's characters by the length of their replacement); every other
    //  masked character is assumed to map to exactly one character.
    StreamSet * localCharacterMapping(StreamSet * U21, const std::vector<std::pair<unsigned, UCD::UnicodeSet>> & lengths,
                                      const std::function<StreamSet * (StreamSet *)> & map);
    StreamSet * builtIn(const std::string & name, StreamSet * U21);
    StreamSet * normalization(const std::string & name, StreamSet * U21);
    StreamSet * maskedNormalization(const std::string & name, StreamSet * U21, StreamSet * selection = nullptr);
    StreamSet * barrierNormalization(const std::string & name, StreamSet * U21, StreamSet * mask, bool updateMasks);
    StreamSet * fcc(StreamSet * U21);
    StreamSet * fcd(StreamSet * U21);
    StreamSet * conversionGroup(const ldml::TransformStep & step, StreamSet * U21);
    StreamSet * insertionGroup(const ldml::TransformStep & step, StreamSet * U21);
    StreamSet * deferredRules(const ldml::TransformStep & step, StreamSet * U21);
    StreamSet * insertionsEligible();
    StreamSet * titlecase(StreamSet * U21);
    StreamSet * hexJava(StreamSet * U21);
    StreamSet * hexUnicode(StreamSet * U21);
    StreamSet * hexC(StreamSet * U21);
    StreamSet * hexXML(StreamSet * U21);
    StreamSet * hexXML10(StreamSet * U21);
    StreamSet * hexPerl(StreamSet * U21);
    StreamSet * hexDecode(unsigned forms, StreamSet * U21);
    StreamSet * remove(StreamSet * U21);
    StreamSet * wordBoundaries(StreamSet * U21);
    StreamSet * maskedStringOverrides(const std::vector<std::pair<UCD::property_t, StreamSet *>> & overrides, StreamSet * U21);
    StreamSet * matchPattern(re::RE * engineBefore, re::RE * engineAfter, const std::vector<re::RE *> & text,
                             StreamSet * U21, StreamSet ** spans = nullptr);
    StreamSet * matchStarts(re::RE * engineBefore, re::RE * engineAfter, const std::vector<re::RE *> & text, StreamSet * U21);
    StreamSet * matchPositions(re::RE * engineBefore, re::RE * engineAfter, const std::vector<UCD::UnicodeSet> & text,
                               StreamSet * U21, StreamSet ** spans = nullptr);
    StreamSet * classes(const std::vector<UCD::UnicodeSet> & sets, StreamSet * U21);
    StreamSet * combine(StreamSet * a, StreamSet * b, bool isOr);
    PipelineBuilder & mP;
    const ldml::TransformRegistry & mRegistry;
    TransformAnalyzer & mAnalyzer;
    NFD_PipelineBuilder mNFD;
    std::vector<StreamSet *> mMasks;
    //  Whether the transform applied is a single conversion group with its own
    //  filter (or one where it is invoked), whose insertions the filter does
    //  not restrict (see insertionGroup), and whether the transform about to be
    //  applied is invoked with a filter.
    bool mInsertAnywhere = false;
    bool mInvokedWithFilter = false;
};

//  The Final_Sigma condition of Unicode full lowercasing (as in el-Lower): Σ
//  becomes ς when preceded by a cased letter (with case-ignorable characters
//  between) and not followed by one.
static const ldml::TransformPlan & finalSigmaPlan() {
    static const ldml::TransformPlan plan = [] {
        std::vector<ldml::Rule *> rules = ldml::ExtractForwardRules(ldml::parseTransformRules({
            "Σ } [:caseignorable:]* [:cased:] → σ ;"
            "[:cased:] [:caseignorable:]* { Σ → ς ;"}));
        return ldml::planTransform(ldml::DisambiguateOrder(rules));
    }();
    return plan;
}

void RulePipelineBuilder::pushFilter(const UCD::UnicodeSet & filter, StreamSet * U21) {
    PipelineBuilder & P = mP;
    StreamSet * mask = classes({filter}, U21);
    if (StreamSet * const outer = activeMask()) mask = combine(mask, outer, false);
    SHOW_STREAM(mask);
    mMasks.push_back(mask);
}

//  The masks after positions are inserted (where the spread mask is 0): each
//  inserted position has the mask of the character it follows.
void RulePipelineBuilder::spreadMasks(StreamSet * SpreadMask) {
    for (StreamSet *& mask : mMasks) {
        StreamSet * const spread = mP.CreateStreamSet(1);
        SpreadByMask(mP, SpreadMask, mask, spread);
        StreamSet * const inherited = mP.CreateStreamSet(1);
        mP.CreateKernelCall<InheritMask>(SpreadMask, spread, inherited);
        mask = inherited;
    }
}

//  The masks after the positions not in keep are deleted.
void RulePipelineBuilder::filterMasks(StreamSet * keep) {
    for (StreamSet *& mask : mMasks) {
        StreamSet * const filtered = mP.CreateStreamSet(1);
        FilterByMask(mP, keep, mask, filtered);
        mask = filtered;
    }
}

StreamSet * RulePipelineBuilder::localCharacterMapping(StreamSet * U21, const std::vector<std::pair<unsigned, UCD::UnicodeSet>> & lengths,
                                                       const std::function<StreamSet * (StreamSet *)> & map) {
    StreamSet * const mask = activeMask();
    if (!mask) return map(U21);
    PipelineBuilder & P = mP;
    //  The masked characters, shortened by FilterByMask, and the untouched
    //  rest of the text (also shortened, to merge back with the mapped
    //  result below).
    StreamSet * const notMask = P.CreateStreamSet(1);
    Invert(P, mask, notMask);
    StreamSet * const Selected = P.CreateStreamSet(21);
    FilterByMask(P, mask, U21, Selected);
    StreamSet * const NonSelected = P.CreateStreamSet(21);
    FilterByMask(P, notMask, U21, NonSelected);
    //  map, applied only to the masked characters: its kernels cost only
    //  that of the (possibly much shorter) masked part of the text.
    StreamSet * const Mapped = map(Selected);
    //  The placement of the mapped characters among the untouched ones: as
    //  many positions as each masked character's replacement has (lengths),
    //  or one otherwise, inserted after its position (as for a conversion
    //  group's longer replacements) -- expanding the masks in effect so that
    //  the (now expanded) active mask spans each full replacement, which is
    //  exactly the placement needed.
    std::vector<unsigned> counts;
    std::vector<StreamSet *> positions;
    for (const auto & l : lengths) {
        if (l.first == 1) continue;
        counts.push_back(l.first - 1);
        positions.push_back(combine(classes({l.second}, U21), mask, false));
    }
    StreamSet * placement = mask;
    if (!counts.empty()) {
        unsigned maxCount = 0;
        for (unsigned c : counts) maxCount = std::max(maxCount, c);
        unsigned bits = 0;
        while ((maxCount >> bits) != 0) bits++;
        StreamSet * const InsertBixNum = P.CreateStreamSet(bits);
        P.CreateKernelCall<InsertionCounts>(counts, positions, InsertBixNum);
        SHOW_BIXNUM(InsertBixNum);
        StreamSet * const SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, InsertBixNum, SpreadMask, kernel::InsertPosition::After);
        spreadMasks(SpreadMask);
        placement = activeMask();
    }
    SHOW_STREAM(placement);
    StreamSet * const Merged = P.CreateStreamSet(21);
    MergeByMask(P, placement, Mapped, NonSelected, Merged);
    SHOW_BIXNUM(Merged);
    return Merged;
}

StreamSet * RulePipelineBuilder::builtIn(const std::string & name, StreamSet * U21) {
    PipelineBuilder & P = mP;
    if (name == "Any-Null") return U21;
    if (name == "Any-NFD" || name == "Any-NFKD" || name == "Any-NFC" || name == "Any-NFKC" || name == "Any-FCC") {
        return activeMask() ? maskedNormalization(name, U21) : normalization(name, U21);
    }
    if (name == "Any-FCD") return fcd(U21);
    if (name == "Any-Lower" || name == "Any-Upper") {
        //  Full lowercasing: the Final_Sigma context of Σ, then the lc mapping.
        if (name == "Any-Lower") {
            for (const ldml::TransformStep & step : finalSigmaPlan().steps) {
                U21 = conversionGroup(step, U21);
            }
        }
        const UCD::property_t prop = name == "Any-Lower" ? UCD::lc : UCD::uc;
        if (StreamSet * const mask = activeMask()) return maskedStringOverrides({{prop, mask}}, U21);
        return U21_StringOverridePipeline(P, prop, U21);
    }
    if (name == "Any-Title") return titlecase(U21);
    if (name == "Any-Hex") return hexJava(U21);
    if (name == "Any-Hex/Java") return hexJava(U21);
    if (name == "Any-Hex/Unicode") return hexUnicode(U21);
    if (name == "Any-Hex/C") return hexC(U21);
    if (name == "Any-Hex/XML") return hexXML(U21);
    if (name == "Any-Hex/XML10") return hexXML10(U21);
    if (name == "Any-Hex/Perl") return hexPerl(U21);
    if (const unsigned forms = hexDecodeForms(name)) return hexDecode(forms, U21);
    if (name == "Any-Remove") return remove(U21);
    llvm::report_fatal_error(llvm::StringRef("tconv: built-in transform " + name + " is not implemented"));
}

//  Hex-Any (and its variants): the escapes decoded (within the filter, if any),
//  with the masks kept in step with the positions deleted.
StreamSet * RulePipelineBuilder::hexDecode(unsigned forms, StreamSet * U21) {
    PipelineBuilder & P = mP;
    std::vector<StreamSet *> keeps;
    StreamSet * const decoded = P.CreateStreamSet(21, 1);
    HexAnyPipeline(P, forms, U21, activeMask(), decoded, keeps);
    for (StreamSet * keep : keeps) filterMasks(keep);
    SHOW_BIXNUM(decoded);
    return decoded;
}

//  Any-Remove: the characters within the filter deleted, with the masks kept in
//  step; without a filter, every character (an empty text).
StreamSet * RulePipelineBuilder::remove(StreamSet * U21) {
    PipelineBuilder & P = mP;
    StreamSet * const mask = activeMask();
    if (!mask) return P.CreateEmptyStreamSet(21, 1);
    StreamSet * const keep = P.CreateStreamSet(1);
    Invert(P, mask, keep);
    StreamSet * const kept = P.CreateStreamSet(21, 1);
    FilterByMask(P, keep, U21, kept);
    filterMasks(keep);
    SHOW_BIXNUM(kept);
    return kept;
}

StreamSet * RulePipelineBuilder::normalization(const std::string & name, StreamSet * U21) {
    PipelineBuilder & P = mP;
    if (name == "Any-NFD") return mNFD.NFD_U21_Pipeline(U21);
    if (name == "Any-NFKD") return mNFD.NFKD_U21_Pipeline(U21);
    if (name == "Any-FCC") return fcc(U21);
    if (name == "Any-NFKC") U21 = mNFD.NFKD_U21_Pipeline(U21);
    StreamSet * const basis = UTF8_Of(P, U21);
    StreamSet * const composed = NFC_Bytes(P, BytesOf(P, basis), basis);
    return U21_Of(P, BasisOf(P, composed));
}

//  A normalization within a filter.  As in ICU, each run of the characters of
//  the filter is normalized by itself: the characters outside it are replaced by
//  the barrier codepoint, which is neither reordered nor composed, so that no
//  character moves or composes across them; after the normalization, which keeps
//  the barriers in order, they are replaced by the characters again.  The
//  barrier codepoint itself is normalized to itself and is a barrier anyway: it
//  is treated as a character outside the filter (its masks are restored with
//  it).  The characters of the normalized runs are all within the filter.
//  With a selection, the characters normalized are those of the selection
//  (within the active mask, if any) rather than of the active mask.
StreamSet * RulePipelineBuilder::maskedNormalization(const std::string & name, StreamSet * U21, StreamSet * selection) {
    StreamSet * const selected = selection ? combine(selection, activeMask(), false) : activeMask();
    return barrierNormalization(name, U21, selected, true);
}

//  The normalization of the characters of mask, the others replaced by barriers
//  (as is a barrier codepoint in the text), and restored after it; with
//  updateMasks, the masks of the filters in effect are kept in step.
StreamSet * RulePipelineBuilder::barrierNormalization(const std::string & name, StreamSet * U21, StreamSet * selected, bool updateMasks) {
    PipelineBuilder & P = mP;
    StreamSet * const barrierChars = classes({UCD::UnicodeSet(BarrierMark)}, U21);
    StreamSet * const notBarrierChars = P.CreateStreamSet(1);
    Invert(P, barrierChars, notBarrierChars);
    StreamSet * const mask = combine(selected, notBarrierChars, false);
    StreamSet * const Work = P.CreateStreamSet(21);
    P.CreateKernelCall<ReplaceUnmasked>(U21, mask, Work);
    SHOW_BIXNUM(Work);
    StreamSet * const Normalized = normalization(name, Work);
    StreamSet * const barriers = classes({UCD::UnicodeSet(BarrierMark)}, Normalized);
    StreamSet * const others = P.CreateStreamSet(1);
    Invert(P, barriers, others);
    StreamSet * const unmasked = P.CreateStreamSet(1);
    Invert(P, mask, unmasked);
    //  The characters replaced by barriers, at the positions of the barriers.
    StreamSet * const outside = P.CreateStreamSet(21);
    FilterByMask(P, unmasked, U21, outside);
    StreamSet * const restored = P.CreateStreamSet(21);
    SpreadByMask(P, barriers, outside, restored);
    StreamSet * const kept = P.CreateStreamSet(21);
    ZeroByMask(P, others, Normalized, kept);
    StreamSet * const result = P.CreateStreamSet(21);
    OrCombine(P, kept, restored, result);
    SHOW_BIXNUM(result);
    //  The masks hold at the normalized characters, and keep their values at
    //  the barriers.
    if (updateMasks) for (StreamSet *& m : mMasks) {
        StreamSet * const outer = P.CreateStreamSet(1);
        FilterByMask(P, unmasked, m, outer);
        StreamSet * const spread = P.CreateStreamSet(1);
        SpreadByMask(P, barriers, outer, spread);
        StreamSet * const combined = P.CreateStreamSet(1);
        OrCombine(P, spread, others, combined);
        m = combined;
    }
    return result;
}

//  Any-FCD (see FCD_RegionMask): the regions where the text fails the FCD test
//  are decomposed, as NFD within a mask, the rest is left as it is.  Within a
//  filter, as in ICU, each run of the filtered characters is taken by itself:
//  the regions are found with the other characters replaced by the barrier
//  codepoint (lccc = tccc = 0), and kept within the filter.
StreamSet * RulePipelineBuilder::fcd(StreamSet * U21) {
    PipelineBuilder & P = mP;
    StreamSet * text = U21;
    if (StreamSet * const mask = activeMask()) {
        text = P.CreateStreamSet(21);
        P.CreateKernelCall<ReplaceUnmasked>(U21, mask, text);
    }
    StreamSet * const Regions = P.CreateStreamSet(1);
    FCD_RegionMask(P, text, Regions);
    SHOW_STREAM(Regions);
    return maskedNormalization("Any-NFD", U21, Regions);
}

//  The characters visible in a pass of Any-FCC: the starters (ccc = 0) and the
//  characters immediately after them.
class FCC_Window : public pablo::PabloKernel {
public:
    FCC_Window(LLVMTypeSystemInterface & ts, StreamSet * Starters, StreamSet * Visible)
    : PabloKernel(ts, "FCC_Window", {Binding{"Starters", Starters}}, {Binding{"Visible", Visible}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const starters = getInputStreamSet("Starters")[0];
        PabloAST * const visible = pb.createInFile(pb.createOr(starters, pb.createAdvance(starters, 1)));
        pb.createAssign(pb.createExtract(getOutputStreamVar("Visible"), pb.getInteger(0)), visible);
    }
};

//  The number of passes of Any-FCC: the largest number of nonstarters in the
//  NFD of a primary composite (3, as α 0313 0300 0345 of U+1F82).
static unsigned FCC_Passes() {
    auto * cccObj = llvm::cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    const UCD::UnicodeSet starters = cccObj->GetCodepointSet(UCD::CCC_ns::NR);
    const UCD::UnicodeSet excluded = llvm::cast<UCD::BinaryPropertyObject>(UCD::getPropertyObject(UCD::Comp_Ex))->GetCodepointSet(UCD::Binary_ns::Y);
    auto * dmObj = llvm::cast<UCD::StringPropertyObject>(UCD::getPropertyObject(UCD::dm));
    UCD::NFD_Engine NFD_engine(UCD::DecompositionOptions::NFD);
    unsigned passes = 1;
    for (UCD::codepoint_t cp : dmObj->GetExplicitCps()) {
        if (excluded.contains(cp)) continue;
        std::u32string decomp;
        NFD_engine.NFD_append1(decomp, cp);
        if (decomp.size() < 2) continue;
        unsigned marks = 0;
        for (char32_t c : decomp) marks += starters.contains(c) ? 0 : 1;
        passes = std::max(passes, marks);
    }
    return passes;
}

//  Any-FCC (ICU's FCC, composition of contiguous characters only): NFD, then
//  NFC in passes, each on the starters and the characters immediately after them
//  (the others replaced by barriers): NFC then composes only adjacent characters.
//  Where a character composes with the starter before it, the next one becomes
//  adjacent to the composite, and is tried in the next pass; where it does not,
//  it stays next to the starter, and so does nothing after it compose.  As many
//  passes as nonstarters in a primary composite.
StreamSet * RulePipelineBuilder::fcc(StreamSet * U21) {
    PipelineBuilder & P = mP;
    auto * cccObj = llvm::cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    const UCD::UnicodeSet starterSet = cccObj->GetCodepointSet(UCD::CCC_ns::NR);
    StreamSet * text = mNFD.NFD_U21_Pipeline(U21);
    static const unsigned passes = FCC_Passes();
    for (unsigned i = 0; i < passes; i++) {
        StreamSet * const starters = classes({starterSet}, text);
        StreamSet * const visible = P.CreateStreamSet(1);
        P.CreateKernelCall<FCC_Window>(starters, visible);
        text = barrierNormalization("Any-NFC", text, visible, false);
    }
    return text;
}

//  Unicode titlecasing (see TitlecasePositions), with the word boundaries of
//  Parabix (re::generateWordBoundaryRule).  Lowercasing includes the Final_Sigma
//  condition (as in Any-Lower), which changes only Σ, to σ or ς, both cased and
//  with the titlecase Σ.
StreamSet * RulePipelineBuilder::titlecase(StreamSet * U21) {
    PipelineBuilder & P = mP;
    for (const ldml::TransformStep & step : finalSigmaPlan().steps) {
        U21 = conversionGroup(step, U21);
    }
    StreamSet * const boundaries = wordBoundaries(U21);
    UCD::BinaryPropertyObject * const casedObj = llvm::cast<UCD::BinaryPropertyObject>(UCD::getPropertyObject(UCD::Cased));
    StreamSet * const cased = classes({casedObj->GetCodepointSet("Y")}, U21);
    StreamSet * const title = P.CreateStreamSet(1);
    StreamSet * const lower = P.CreateStreamSet(1);
    P.CreateKernelCall<TitlecasePositions>(boundaries, cased, title, lower);
    SHOW_STREAM(boundaries);
    SHOW_STREAM(title);
    SHOW_STREAM(lower);
    //  Within a filter, the words are those of the whole text.
    if (StreamSet * const mask = activeMask()) {
        return maskedStringOverrides({{UCD::tc, combine(title, mask, false)}, {UCD::lc, combine(lower, mask, false)}}, U21);
    }
    return maskedStringOverrides({{UCD::tc, title}, {UCD::lc, lower}}, U21);
}

//  Any-Hex/Java: each character encoded as UTF-16 code units, then
//  each unit expressed as \u followed by 4 hexadecimal digits (JavaHexPipeline) --
//  a context-free, single-character mapping, so within a mask only the
//  masked characters need be converted (localCharacterMapping).
StreamSet * RulePipelineBuilder::hexJava(StreamSet * U21) {
    static const std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths = {
        {6, UCD::UnicodeSet(0x0000, 0xFFFF)},       // \u and 4 digits
        {12, UCD::UnicodeSet(0x10000, 0x10FFFF)},   // a surrogate pair: 2 \uXXXX
    };
    return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
        PipelineBuilder & P = mP;
        StreamSet * const Hex = P.CreateStreamSet(21, 1);
        JavaHexPipeline(P, Selected, Hex);
        SHOW_BIXNUM(Hex);
        return Hex;
    });
}

//  Any-Hex/Unicode: each character as U+ and its codepoint in uppercase
//  hexadecimal, of 4, 5 or 6 digits by its magnitude (UnicodeHexPipeline) --
//  a context-free, single-character mapping, so within a mask only the
//  masked characters need be converted (localCharacterMapping).
StreamSet * RulePipelineBuilder::hexUnicode(StreamSet * U21) {
    static const std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths = {
        {6, UCD::UnicodeSet(0x0000, 0xFFFF)},       // U+ and 4 digits
        {7, UCD::UnicodeSet(0x10000, 0xFFFFF)},     // U+ and 5 digits
        {8, UCD::UnicodeSet(0x100000, 0x10FFFF)},   // U+ and 6 digits
    };
    return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
        PipelineBuilder & P = mP;
        StreamSet * const Hex = P.CreateStreamSet(21, 1);
        UnicodeHexPipeline(P, Selected, Hex);
        SHOW_BIXNUM(Hex);
        return Hex;
    });
}

//  Any-Hex/C: each basic mulitplane character encoded as \uxxxx,
//  each supplementary plane character as \Uxxxxxxxx (UnicodeHexPipeline) --
//  a context-free, single-character mapping, so within a mask only the
//  masked characters need be converted (localCharacterMapping).
StreamSet * RulePipelineBuilder::hexC(StreamSet * U21) {
    static const std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths = {
        {6, UCD::UnicodeSet(0x0000, 0xFFFF)},       // \u and 4 digits
        {10, UCD::UnicodeSet(0x10000, 0x10FFFF)},   // \U and 8 digits
    };
    return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
        PipelineBuilder & P = mP;
        StreamSet * const Hex = P.CreateStreamSet(21, 1);
        CHexPipeline(P, Selected, Hex);
        SHOW_BIXNUM(Hex);
        return Hex;
    });
}

//  Any-Hex/XML: each basic mulitplane character encoded as &#x<hexdigit>+;
//  where leading zeroes are deleted from <hexdigit>s, except for the last --
//  a context-free, single-character mapping, so within a mask only the
//  masked characters need be converted (localCharacterMapping).
StreamSet * RulePipelineBuilder::hexXML(StreamSet * U21) {
    static const std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths = {
        {5, UCD::UnicodeSet(0x0, 0xF)},            // &#x and 1 digit and ;
        {6, UCD::UnicodeSet(0x10, 0xFF)},          // &#x and 2 digits and ;
        {7, UCD::UnicodeSet(0x100, 0xFFF)},        // &#x and 3 digits and ;
        {8, UCD::UnicodeSet(0x1000, 0xFFFF)},      // &#x and 4 digits and ;
        {9, UCD::UnicodeSet(0x10000, 0xFFFFF)},    // &#x and 5 digits and ;
        {10, UCD::UnicodeSet(0x100000, 0x10FFFF)}, // &#x and 6 digits and ;
    };
    return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
        PipelineBuilder & P = mP;
        StreamSet * const Hex = P.CreateStreamSet(21, 1);
        XMLHexPipeline(P, Selected, Hex);
        SHOW_BIXNUM(Hex);
        return Hex;
    });
}

//  Any-Hex/XML10: each character encoded as &#<digit>+; with the decimal value
//  of its codepoint, without leading zeroes (XMLDecimalPipeline) -- a
//  context-free, single-character mapping, so within a mask only the masked
//  characters need be converted (localCharacterMapping).
StreamSet * RulePipelineBuilder::hexXML10(StreamSet * U21) {
    static const std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths = {
        {4, UCD::UnicodeSet(0, 9)},                 // &# and 1 digit and ;
        {5, UCD::UnicodeSet(10, 99)},               // &# and 2 digits and ;
        {6, UCD::UnicodeSet(100, 999)},             // &# and 3 digits and ;
        {7, UCD::UnicodeSet(1000, 9999)},           // &# and 4 digits and ;
        {8, UCD::UnicodeSet(10000, 99999)},         // &# and 5 digits and ;
        {9, UCD::UnicodeSet(100000, 999999)},       // &# and 6 digits and ;
        {10, UCD::UnicodeSet(1000000, 0x10FFFF)},   // &# and 7 digits and ;
    };
    return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
        PipelineBuilder & P = mP;
        StreamSet * const Dec = P.CreateStreamSet(21, 1);
        XMLDecimalPipeline(P, Selected, Dec);
        SHOW_BIXNUM(Dec);
        return Dec;
    });
}

//  Any-Hex/Perl: each basic mulitplane character encoded as &#x<hexdigit>+;
//  where leading zeroes are deleted from <hexdigit>s, except for the last --
//  a context-free, single-character mapping, so within a mask only the
//  masked characters need be converted (localCharacterMapping).
StreamSet * RulePipelineBuilder::hexPerl(StreamSet * U21) {
    static const std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths = {
        {5, UCD::UnicodeSet(0x0, 0xF)},            // \x{ and 1 digit and }
        {6, UCD::UnicodeSet(0x10, 0xFF)},          // \x{ and 2 digits and }
        {7, UCD::UnicodeSet(0x100, 0xFFF)},        // \x{ and 3 digits and }
        {8, UCD::UnicodeSet(0x1000, 0xFFFF)},      // \x{ and 4 digits and }
        {9, UCD::UnicodeSet(0x10000, 0xFFFFF)},    // \x{ and 5 digits and }
        {10, UCD::UnicodeSet(0x100000, 0x10FFFF)}, // \x{ and 6 digits and }
    };
    return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
        PipelineBuilder & P = mP;
        StreamSet * const Hex = P.CreateStreamSet(21, 1);
        PerlHexPipeline(P, Selected, Hex);
        SHOW_BIXNUM(Hex);
        return Hex;
    });
}

//  The word boundaries of a text (in Unicode indexing, one position per
//  character), marked at the position after each.
StreamSet * RulePipelineBuilder::wordBoundaries(StreamSet * U21) {
    const PreparedRE prepared = prepareRE(re::generateWordBoundaryRule());
    StreamSet * const boundaries = mP.CreateStreamSet(1);
    RE_PipelineBuilder engine(mP, RE_context{&cc::Unicode, U21});
    engine.matchSearchPipeline(prepared, boundaries);
    return boundaries;
}

//  String override properties (e.g. full case mappings) applied to the
//  characters at the positions of their masks (which are disjoint), as by
//  U21_StringOverridePipeline: positions are inserted after the characters
//  mapped to longer strings, and the bits of each position of the replacements
//  are changed (the first) or set (the further ones).
StreamSet * RulePipelineBuilder::maskedStringOverrides(const std::vector<std::pair<UCD::property_t, StreamSet *>> & overrides,
                                                       StreamSet * U21) {
    PipelineBuilder & P = mP;
    struct Override {
        std::vector<UCD::UnicodeSet> insertion;             // the bixnum of the number of positions to insert
        std::vector<std::vector<UCD::UnicodeSet>> xfrms;    // the bit sets of each position of the replacements
        StreamSet * mask;
    };
    std::vector<Override> all;
    unsigned insertionBits = 0;
    size_t maxLength = 0;
    for (const auto & o : overrides) {
        Override v;
        v.mask = o.second;
        UCD::PropertyObject * const propObj = UCD::getPropertyObject(o.first);
        if (auto * p = llvm::dyn_cast<UCD::CodePointPropertyObject>(propObj)) {
            v.xfrms.push_back(p->GetBitTransformSets());
        } else if (auto * p = llvm::dyn_cast<UCD::StringOverridePropertyObject>(propObj)) {
            for (unsigned i = 0; i < p->MaxUnicodeInsertLength(); i++) v.xfrms.push_back(p->GetBitTransformSets(i));
            v.insertion = p->GetUnicodeInsertLengthBixNumSets();
        } else {
            llvm::report_fatal_error("tconv: not a codepoint or string override property");
        }
        insertionBits = std::max<unsigned>(insertionBits, v.insertion.size());
        maxLength = std::max(maxLength, v.xfrms.size());
        all.push_back(std::move(v));
    }
    //  Each set of streams restricted to the positions of a mask.
    auto masked = [&](StreamSet * mask, StreamSet * streams) {
        StreamSet * const result = P.CreateStreamSet(streams->getNumElements());
        ZeroByMask(P, mask, streams, result);
        return result;
    };
    if (insertionBits > 0) {
        StreamSet * InsertBixNum = nullptr;
        for (const Override & v : all) {
            std::vector<UCD::UnicodeSet> sets = v.insertion;
            sets.resize(insertionBits);
            InsertBixNum = combine(InsertBixNum, masked(v.mask, classes(sets, U21)), true);
        }
        SHOW_BIXNUM(InsertBixNum);
        StreamSet * const SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, InsertBixNum, SpreadMask, kernel::InsertPosition::After);
        StreamSet * const Expanded = P.CreateStreamSet(21, 1);
        SpreadByMask(P, SpreadMask, U21, Expanded);
        U21 = Expanded;
        spreadMasks(SpreadMask);
        for (Override & v : all) {
            StreamSet * const spread = P.CreateStreamSet(1);
            SpreadByMask(P, SpreadMask, v.mask, spread);
            v.mask = spread;
        }
    }
    StreamSet * result = U21;
    for (size_t i = 0; i < maxLength; i++) {
        StreamSet * bits = nullptr;
        for (const Override & v : all) {
            if (i >= v.xfrms.size()) continue;
            std::vector<UCD::UnicodeSet> sets = v.xfrms[i];
            sets.resize(21);
            bits = combine(bits, masked(v.mask, classes(sets, U21)), true);
        }
        StreamSet * const next = P.CreateStreamSet(21, 1);
        if (i == 0) {
            XorCombine(P, result, bits, next);
        } else {
            StreamSet * const shifted = P.CreateStreamSet(21);
            P.CreateKernelCall<ShiftForward>(bits, shifted, static_cast<unsigned>(i));
            OrCombine(P, result, shifted, next);
        }
        result = next;
    }
    return result;
}

//  The positions of the last characters of the matches of a text (a sequence of
//  sets) with contexts.  With spans, also the positions of the characters of the
//  texts of the matches, from the first to the last (the match spans of the
//  regular expression engine, without the contexts).
StreamSet * RulePipelineBuilder::matchPositions(re::RE * engineBefore, re::RE * engineAfter, const std::vector<UCD::UnicodeSet> & text,
                                                StreamSet * U21, StreamSet ** spans) {
    std::vector<re::RE *> items;
    for (const UCD::UnicodeSet & chars : text) items.push_back(re::makeCC(chars, &cc::Unicode));
    return matchPattern(engineBefore, engineAfter, items, U21, spans);
}

//  The positions of the last characters of the matches of a pattern (a
//  sequence of items) with contexts, and optionally the spans of the matches.
StreamSet * RulePipelineBuilder::matchPattern(re::RE * engineBefore, re::RE * engineAfter, const std::vector<re::RE *> & text,
                                              StreamSet * U21, StreamSet ** spans) {
    std::vector<re::RE *> items;
    //  ICU matches contexts possessively (see ldml::possessiveContext).  (The
    //  contexts are resolved to sets of characters by ldml::expandedContext,
    //  without inserted positions.)
    const Insertions none;
    if (engineBefore) {
        items.push_back(re::makeLookBehindAssertion(
            ldml::possessiveContext(ldml::expandedContext(engineBefore, none, DeletionMark), false)));
    }
    for (re::RE * item : text) items.push_back(item);
    if (engineAfter) {
        items.push_back(re::makeLookAheadAssertion(
            ldml::possessiveContext(ldml::expandedContext(engineAfter, none, DeletionMark), true)));
    }
    re::RE * const pattern = re::makeSeq(items.begin(), items.end());
    //  The engine marks a match at an offset after its last character (e.g. 1
    //  for a pattern ending with a zero-width lookahead): shift the marks back.
    const PreparedRE prepared = prepareRE(pattern);
    StreamSet * matches = mP.CreateStreamSet(1);
    RE_PipelineBuilder engine(mP, RE_context{&cc::Unicode, U21});
    if (spans) {
        *spans = mP.CreateStreamSet(1);
        engine.matchSpanPipeline(prepared, matches, *spans);
    } else {
        engine.matchSearchPipeline(prepared, matches);
    }
    if (const unsigned offset = prepared.grepOffset()) {
        StreamSet * const shifted = mP.CreateStreamSet(1);
        mP.CreateKernelCall<ShiftBack>(matches, shifted, offset);
        matches = shifted;
    }
    return matches;
}

//  The positions at which a pattern (a sequence of items) matches with its
//  contexts: the pattern and its after context as a lookahead (which the engine
//  marks at the position it begins), after a lookbehind of the before context.
StreamSet * RulePipelineBuilder::matchStarts(re::RE * engineBefore, re::RE * engineAfter, const std::vector<re::RE *> & text,
                                             StreamSet * U21) {
    const Insertions none;
    std::vector<re::RE *> items;
    if (engineBefore) {
        items.push_back(re::makeLookBehindAssertion(
            ldml::possessiveContext(ldml::expandedContext(engineBefore, none, DeletionMark), false)));
    }
    std::vector<re::RE *> ahead(text.begin(), text.end());
    if (engineAfter) ahead.push_back(ldml::possessiveContext(ldml::expandedContext(engineAfter, none, DeletionMark), true));
    items.push_back(re::makeLookAheadAssertion(re::makeSeq(ahead.begin(), ahead.end())));
    const PreparedRE prepared = prepareRE(re::makeSeq(items.begin(), items.end()));
    StreamSet * const matches = mP.CreateStreamSet(1);
    RE_PipelineBuilder engine(mP, RE_context{&cc::Unicode, U21});
    engine.matchSearchPipeline(prepared, matches);
    return matches;
}

//  The stream (or stream set) of the CCs for a vector of sets, at each position of U21.
StreamSet * RulePipelineBuilder::classes(const std::vector<UCD::UnicodeSet> & sets, StreamSet * U21) {
    std::vector<re::CC *> ccs;
    for (const UCD::UnicodeSet & s : sets) ccs.push_back(re::makeCC(s, &cc::Unicode));
    StreamSet * const result = mP.CreateStreamSet(ccs.size());
    mP.CreateKernelCall<CharClassesKernel>(ccs, U21, result);
    return result;
}

//  (nullptr stands for no streams: OR or AND of the other.)
StreamSet * RulePipelineBuilder::combine(StreamSet * a, StreamSet * b, bool isOr) {
    if (a == nullptr) return b;
    if (b == nullptr) return a;
    StreamSet * const result = mP.CreateStreamSet(a->getNumElements());
    if (isOr) OrCombine(mP, a, b, result); else AndCombine(mP, a, b, result);
    return result;
}

//  Whether a conversion group is a context-free, single-character mapping:
//  no string rules (whose multi-character matches could spuriously span the
//  seam between unrelated masked spans once concatenated by
//  localCharacterMapping), no subgroup with a context (which could need to
//  see a character outside the mask), and no character deleted (a
//  replacement length of zero, which localCharacterMapping's masked fast
//  path does not support).  Only for such a group is shortening the text to
//  the masked characters both correct and complete.
static bool isLocalCharacterMapping(const ldml::TransformStep & step) {
    if (!step.stringRules.empty() || !step.captureReplacements.empty() || !step.insertions.empty() || !step.deferred.empty()) return false;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        if (s.engineBefore || s.engineAfter) return false;
        for (const ldml::CharMapping & m : s.mappings) {
            if (m.replacement.empty()) return false;
        }
    }
    return true;
}

//  A conversion group, in three phases.
//
//  1. The rules are matched on the text as it is (with their contexts): the
//     subgroups of single-character rules, and the string rules, of whose
//     matches those that apply are selected where string rules may match within
//     the texts of others (see SelectStringMatches).  The single-character rules
//     apply only where not covered by the applied matches of the string rules
//     (see ldml::TransformStep::coverStringRules) and, within a filter, at the
//     positions of the mask.
//  2. The changes are computed at the positions of the characters, for each
//     position i of the replacements: the bits to change at the character (for
//     i = 0, to change it to the first character of its replacement, or to the
//     deletion mark), or at the i-th position to be inserted after it (for i > 0,
//     to change the deletion mark there to the i-th character of the
//     replacement).  A string rule, of a text of n characters replaced by m,
//     changes the characters of the text to those of the replacement (the
//     characters beyond m to the deletion mark), and the further characters of
//     the replacement (if m > n) are written at the positions to be inserted after
//     its last character.
//  3. Positions holding the deletion mark are inserted after the characters where
//     applied rules have longer replacements, as many as needed after each; the
//     changes are spread with the text (those of position i > 0 then moved i
//     positions forward), and applied by XOR (as no two rules apply at the same
//     position, the changes of all the rules are combined by OR).  The positions
//     holding the deletion mark are then deleted.
//
//  As positions are inserted only where rules apply, after the rules have been
//  matched, the matches are those of the text itself, and the characters of the
//  text of a string rule are consecutive.
//
//  Phases 1 and 2 are computed by a few fused kernels (FusedRules), each for a
//  chunk of the string rules (by their lengths, see fusedStringChunks) or of
//  the subgroups, which also match the rules whose contexts are fixed sequences
//  of sets; the matches of the other rules are given to them by the regular
//  expression engine.
//  The positions after which the insertion rules of a group may insert, within
//  filters (see insertionGroup), or nullptr (everywhere).
StreamSet * RulePipelineBuilder::insertionsEligible() {
    const size_t restricting = mMasks.size() - (mInsertAnywhere && !mMasks.empty() ? 1 : 0);
    if (restricting == 0) return nullptr;
    StreamSet * const eligible = mP.CreateStreamSet(1);
    mP.CreateKernelCall<ShiftBack>(mMasks[restricting - 1], eligible, 1);
    return eligible;
}

//  A group of insertion rules (see ldml::InsertionRule).  Each rule applies at
//  the positions of the characters after which it inserts its text (its before
//  character, followed by its after context), where no earlier rule applies,
//  and, within a filter, where the next character is in the mask (see below).
//  The positions are inserted after them (InsertionSpreadMask), and the texts
//  written there.
StreamSet * RulePipelineBuilder::insertionGroup(const ldml::TransformStep & step, StreamSet * U21) {
    PipelineBuilder & P = mP;
    //  Within a filter, ICU applies a compound transform (of several groups) to
    //  each run of the characters of the mask, inserting where the cursor stops
    //  in the run: before a character of the mask (the contexts see the text
    //  outside the run).  The text inserted is in the run.  A transform of this
    //  group alone, with its own filter (or one given where it is invoked), checks
    //  the filter only for the characters it replaces, of which an insertion has
    //  none: it inserts anywhere (within the runs of an enclosing filter).
    StreamSet * eligible = insertionsEligible();
    std::vector<StreamSet *> positions;
    std::vector<unsigned> counts;
    std::vector<std::u32string> texts;
    unsigned maxCount = 0;
    for (const ldml::InsertionRule & ins : step.insertions) {
        //  (The engine's matches may extend a position beyond the text: they are
        //  combined with a stream of the text's length, the class of the character.)
        StreamSet * at = combine(classes({ins.before}, U21), matchPositions(nullptr, ins.engineAfter, {ins.before}, U21), false);
        at = combine(at, eligible, false);
        SHOW_STREAM(at);
        StreamSet * const notAt = P.CreateStreamSet(1);
        Invert(P, at, notAt);
        eligible = combine(eligible, notAt, false);
        positions.push_back(at);
        counts.push_back(static_cast<unsigned>(ins.replacement.size()));
        texts.push_back(ins.replacement);
        maxCount = std::max(maxCount, counts.back());
    }
    unsigned bits = 0;
    while ((maxCount >> bits) != 0) bits++;
    StreamSet * const InsertBixNum = P.CreateStreamSet(bits);
    P.CreateKernelCall<InsertionCounts>(counts, positions, InsertBixNum);
    SHOW_BIXNUM(InsertBixNum);
    StreamSet * const SpreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, InsertBixNum, SpreadMask, kernel::InsertPosition::After);
    SHOW_STREAM(SpreadMask);
    StreamSet * const Expanded = P.CreateStreamSet(21, 1);
    SpreadByMask(P, SpreadMask, U21, Expanded);
    spreadMasks(SpreadMask);
    if (!mMasks.empty()) {
        StreamSet * const inserted = P.CreateStreamSet(1);
        Invert(P, SpreadMask, inserted);
        for (StreamSet *& mask : mMasks) mask = combine(mask, inserted, true);
    }
    //  The j-th position inserted after a position where rule i applies.
    StreamSet * const At = P.CreateStreamSet(positions.size());
    P.CreateKernelCall<CollectStreams>(positions, At);
    StreamSet * const SpreadAt = P.CreateStreamSet(positions.size());
    SpreadByMask(P, SpreadMask, At, SpreadAt);
    std::vector<StreamSet *> shifted;
    for (unsigned j = 1; j <= maxCount; j++) {
        StreamSet * const s = P.CreateStreamSet(positions.size());
        P.CreateKernelCall<ShiftForward>(SpreadAt, s, j);
        shifted.push_back(s);
    }
    StreamSet * const Result = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<InsertedTexts>(texts, shifted, Expanded, Result);
    SHOW_BIXNUM(Result);
    return Result;
}

StreamSet * RulePipelineBuilder::conversionGroup(const ldml::TransformStep & step, StreamSet * U21) {
    if (insertionsOnly(step)) return insertionGroup(step, U21);
    //  Within a mask, a context-free, single-character mapping is applied
    //  only to the masked characters (localCharacterMapping): map runs this
    //  same function again, but on the (unmasked) shortened text, where the
    //  group's rules apply to every character directly, as without a filter.
    if (activeMask() && isLocalCharacterMapping(step)) {
        std::vector<std::pair<unsigned, UCD::UnicodeSet>> lengths;
        for (const ldml::CharMapSubgroup & s : step.subgroups) {
            for (const ldml::CharMapping & m : s.mappings) {
                if (m.replacement.size() != 1) lengths.push_back({static_cast<unsigned>(m.replacement.size()), m.chars});
            }
        }
        return localCharacterMapping(U21, lengths, [&](StreamSet * Selected) {
            std::vector<StreamSet *> outerMasks = std::move(mMasks);
            mMasks.clear();
            StreamSet * const Mapped = conversionGroup(step, Selected);
            mMasks = std::move(outerMasks);
            return Mapped;
        });
    }
    PipelineBuilder & P = mP;
    //  The characters of a set that the group may change: those other than the
    //  deletion mark.
    auto restricted = [&](const UCD::UnicodeSet & chars) {
        return chars - UCD::UnicodeSet(DeletionMark);
    };
    //  The mappings of each subgroup, and the texts of the string rules,
    //  restricted to the characters that may change.
    std::vector<std::vector<ldml::CharMapping>> maps;
    bool needDeletion = false;
    for (const ldml::CharMapSubgroup & subgroup : step.subgroups) {
        maps.emplace_back();
        for (const ldml::CharMapping & m : subgroup.mappings) {
            //  A character replaced by itself does not change (and a subgroup in
            //  which no character changes needs no matches, whose kernels would
            //  have no use).
            UCD::UnicodeSet chars = restricted(m.chars);
            if (m.replacement.size() == 1) chars = chars - UCD::UnicodeSet(m.replacement[0]);
            if (chars.empty()) continue;
            maps.back().push_back(ldml::CharMapping{chars, m.replacement});
            if (m.replacement.empty()) needDeletion = true;
        }
    }
    std::vector<std::vector<UCD::UnicodeSet>> texts;
    for (const ldml::StringRule & rule : step.stringRules) {
        texts.emplace_back();
        for (const UCD::UnicodeSet & chars : rule.text) {
            const UCD::UnicodeSet r = restricted(chars);
            if (r.empty()) {
                texts.back().clear();
                break;
            }
            texts.back().push_back(r);
        }
        if (!texts.back().empty() && (rule.replacement.size() < rule.text.size() || !rule.run.empty())) needDeletion = true;
    }
    for (const ldml::MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
        for (const ldml::BoundaryReplacement & br : set.replacements) {
            if (br.kind == ldml::BoundaryReplacement::Kind::Replace && br.replacement.size() < br.text.size()) needDeletion = true;
        }
        for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) {
            if (r.firstReplacement.size() < r.first.size()) needDeletion = true;
        }
    }
    StreamSet * const mask = activeMask();

    //  Phase 1.  The matches of the string rules, at their first positions: of a
    //  rule of tier A or B (see FusedRules), computed by the fused kernels; of another,
    //  by the regular expression engine (at its last character, shifted back).
    //  Within a filter, the n characters of a match must all be within the mask.
    const size_t numStringRules = step.stringRules.size();
    std::vector<bool> fusedMatch(numStringRules, false);
    std::vector<FusedContexts> befores(numStringRules), afters(numStringRules);
    std::vector<std::pair<size_t, size_t>> lengths(numStringRules);
    for (size_t r = 0; r < numStringRules; r++) {
        const ldml::StringRule & rule = step.stringRules[r];
        lengths[r] = {texts[r].size(), rule.replacement.size()};
        //  (The after context first: it is also needed where the before context is
        //  matched against the converted text, with repetitions.)
        fusedMatch[r] = !texts[r].empty() && fusedContexts(rule.engineAfter, false, afters[r])
                   && fusedContexts(rule.engineBefore, true, befores[r]);
    }
    //  The rules whose before contexts the conversions of the group may make match
    //  differently in the converted text: matched against it by a correction
    //  kernel (FusedRules with rounds), after the other rules; or, if such string
    //  rules must be selected or other rules need their coverage, together with
    //  all the rules of the group (see wholeGroup below).
    const std::vector<ConversionOutcome> outcomes = conversionOutcomes(step);
    const bool needCoverage = (step.coverStringRules || hasConvertedSubgroup(step, outcomes)) &&
        std::any_of(maps.begin(), maps.end(), [](const std::vector<ldml::CharMapping> & m) {return !m.empty();});
    const bool correct = ConvertedContextRounds > 0;
    //  The string rules whose before contexts (possibly with repetitions) are
    //  matched against the converted text, and their contexts.
    std::vector<bool> convertible(numStringRules, false);
    std::vector<FusedContexts> convertedBefores(numStringRules);
    for (size_t r = 0; r < numStringRules; r++) {
        const ldml::StringRule & rule = step.stringRules[r];
        FusedContexts after;
        convertible[r] = correct && !texts[r].empty() && fusedContexts(rule.engineAfter, false, after)
                         && fusedContexts(rule.engineBefore, true, convertedBefores[r], true)
                         && convertedContextMatters(convertedBefores[r], outcomes);
    }
    std::vector<bool> correctedString(numStringRules, false);
    for (size_t r = 0; r < numStringRules; r++) {
        correctedString[r] = convertible[r] && step.stringOrder.empty() && !needCoverage;
    }
    auto engineFirsts = [&](size_t r) {
        const ldml::StringRule & rule = step.stringRules[r];
        const size_t n = texts[r].size();
        StreamSet * matches = matchPositions(rule.engineBefore, rule.engineAfter, texts[r], U21);
        if (mask) {
            for (size_t j = 0; j < n; j++) {
                StreamSet * within = mask;
                if (j > 0) {
                    within = P.CreateStreamSet(1);
                    P.CreateKernelCall<ShiftForward>(mask, within, static_cast<unsigned>(j));
                }
                matches = combine(matches, within, false);
            }
        }
        if (n == 1) return matches;
        StreamSet * const first = P.CreateStreamSet(1);
        P.CreateKernelCall<ShiftBack>(matches, first, static_cast<unsigned>(n - 1));
        return first;
    };
    auto fusedRule = [&](size_t r) {
        FusedStringRule fr;
        fr.text = texts[r];
        fr.run = step.stringRules[r].run;
        fr.replacement = step.stringRules[r].replacement;
        fr.before = befores[r];
        fr.after = afters[r];
        return fr;
    };
    StreamSet * const inText = P.CreateStreamSet(1);
    P.CreateKernelCall<TextPositions>(U21, inText);
    //  The classes of the sets that a fused kernel uses.
    auto fusedClasses = [&](const FusedRulesConfig & config) -> StreamSet * {
        const std::vector<UCD::UnicodeSet> sets = fusedClassSets(config);
        if (sets.empty()) return nullptr;
        //  The classes of the multiplexed mapping numbers in kernels of their own
        //  (see MuxClassKernels), the others in one.
        std::set<std::string> muxKeys;
        if (MuxClassKernels > 0) {
            for (const FusedSubgroup & g : config.subgroups) {
                const FusedSubgroupPlan plan(g);
                for (const UCD::UnicodeSet & set : plan.mux) muxKeys.insert(setKey(set));
            }
        }
        std::vector<size_t> rest, mux;
        for (size_t i = 0; i < sets.size(); i++) (muxKeys.count(setKey(sets[i])) ? mux : rest).push_back(i);
        auto kernelOf = [&](const std::vector<size_t> & indices) {
            std::vector<re::CC *> ccs;
            for (size_t i : indices) ccs.push_back(re::makeCC(sets[i], &cc::Unicode));
            StreamSet * const classes = P.CreateStreamSet(ccs.size());
            P.CreateKernelCall<CharClassesKernel>(ccs, U21, classes);
            return classes;
        };
        if (mux.empty()) return kernelOf(rest);
        std::vector<std::vector<size_t>> parts;
        if (!rest.empty()) parts.push_back(rest);
        const size_t n = std::min<size_t>(MuxClassKernels, mux.size());
        for (size_t k = 0; k < n; k++) {
            std::vector<size_t> part;
            for (size_t j = k * mux.size() / n; j < (k + 1) * mux.size() / n; j++) part.push_back(mux[j]);
            parts.push_back(part);
        }
        std::vector<StreamSet *> in;
        std::vector<std::pair<unsigned, unsigned>> where(sets.size());
        for (size_t p = 0; p < parts.size(); p++) {
            in.push_back(kernelOf(parts[p]));
            for (size_t j = 0; j < parts[p].size(); j++) where[parts[p][j]] = {static_cast<unsigned>(p), static_cast<unsigned>(j)};
        }
        StreamSet * const classes = P.CreateStreamSet(sets.size());
        P.CreateKernelCall<MergeStreamSets>(in, where, classes);
        return classes;
    };
    //  For the selection of the matches by class (LeftmostMatches): the lengths of
    //  the classes, and their runs, as streams of the characters of their sets
    //  (within the mask).
    struct SelectionClasses {
        std::vector<unsigned> lengths;
        std::vector<int> runOf;
        StreamSet * runs = nullptr;
    };
    auto selectionClasses = [&](const std::vector<MatchClass> & classes) {
        SelectionClasses c;
        std::vector<UCD::UnicodeSet> runSets;
        for (const MatchClass & k : classes) {
            c.lengths.push_back(k.length);
            if (k.run.empty()) {
                c.runOf.push_back(-1);
                continue;
            }
            auto f = std::find(runSets.begin(), runSets.end(), k.run);
            if (f == runSets.end()) {
                runSets.push_back(k.run);
                f = runSets.end() - 1;
            }
            c.runOf.push_back(static_cast<int>(f - runSets.begin()));
        }
        if (!runSets.empty()) {
            std::vector<re::CC *> ccs;
            for (const UCD::UnicodeSet & set : runSets) ccs.push_back(re::makeCC(set, &cc::Unicode));
            StreamSet * runs = P.CreateStreamSet(ccs.size());
            P.CreateKernelCall<CharClassesKernel>(ccs, U21, runs);
            if (mask) {
                StreamSet * const masked = P.CreateStreamSet(ccs.size());
                P.CreateKernelCall<MaskStreams>(runs, mask, masked);
                runs = masked;
            }
            c.runs = runs;
        }
        return c;
    };
    //  The results of the rules, accumulated by the fused kernels (each ORing its
    //  results into those of the kernels before it).
    FusedState state;
    //  The regions of the matches of the rules with captures referred to in
    //  reverse order (see ldml::MultipleCaptureMultipleReplacementSet::reversed):
    //  the first characters of B1, kept in step with the text as positions are
    //  inserted and deleted, from which the regions begin at an offset.
    struct Swap {
        StreamSet * marks;
        int offset;
        unsigned length;
        unsigned distance;
    };
    std::vector<Swap> swaps;
    //  The regions moved (see ldml::MultipleCaptureMultipleReplacementSet::moved):
    //  the first characters of capture i (moved by offset to the start of the
    //  region) and of capture j, and the last of capture j, kept in step with
    //  the text likewise.
    struct Move {
        StreamSet * startI;
        int offset;
        StreamSet * startJ;
        StreamSet * endJ;
    };
    std::vector<Move> moves;
    auto applyFused = [&](FusedRulesConfig && config, StreamSet * allowedAt) {
        const FusedLayout layout(config, state);
        FusedState out;
        for (unsigned i = 0; i < layout.changePositions; i++) out.changes.push_back(P.CreateStreamSet(21));
        for (unsigned k : layout.insertCounts) out.insertions[k] = P.CreateStreamSet(1);
        if (config.coverage) {
            out.spans = P.CreateStreamSet(1);
            out.starts = P.CreateStreamSet(1);
        } else {
            out.spans = state.spans;
            out.starts = state.starts;
        }
        StreamSet * const classes = fusedClasses(config);
        P.CreateKernelCall<FusedRules>(std::move(config), U21, classes, inText, mask, allowedAt, state, out, nullptr);
        state = out;
    };
    //  The matches of the MultipleCaptureMultipleReplacement rules (see below),
    //  on the text as it is: before the selection of the string rules, if the
    //  matches of both are selected together (jointSelection).
    struct CaptureMatches {
        StreamSet * spans = nullptr;
        StreamSet * ends = nullptr;
        StreamSet * starts = nullptr;
        StreamSet * firstStarts = nullptr;  // (with exactStarts: the first start of each span)
    };
    std::vector<CaptureMatches> captureMatches;
    bool capturesMatched = false;
    //  With jointSelection: the matches selected (of the string rules and the
    //  sets), and the runs of the sets.
    StreamSet * jointSelected = nullptr;
    std::vector<StreamSet *> jointRuns;
    auto matchCaptures = [&]() {
        capturesMatched = true;
        for (const ldml::MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
            const size_t n0 = set.rules.front().first.size();
            std::vector<re::RE *> items;
            for (size_t j = 0; j < n0; j++) {
                UCD::UnicodeSet chars;
                for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) chars = chars + r.first[j];
                items.push_back(re::makeCC(chars, &cc::Unicode));
            }
            for (size_t i = 0; i < set.captures.size(); i++) {
                //  (A capture matched possessively, as ICU matches it, with its sets resolved.)
                items.push_back(ldml::possessiveContext(ldml::expandedContext(set.captures[i], Insertions(), DeletionMark), true));
                for (const UCD::UnicodeSet & chars : set.texts[i]) items.push_back(re::makeCC(chars, &cc::Unicode));
            }
            //  Only the streams used are made (the pipeline compiler fails on kernels
            //  whose outputs are unused): the starts, if replacements are made from
            //  them, the spans are covered or selected, and the spans, for the starts.
            bool needStarts = n0 > 0 || needCoverage || step.captureSelection || set.reversed || set.moved;
            for (const ldml::BoundaryReplacement & br : set.replacements) needStarts |= !br.fromEnd;
            CaptureMatches m;
            m.ends = matchPattern(set.engineBefore, set.engineAfter, items, U21,
                                  (needStarts && !set.singleCharacter) ? &m.spans : nullptr);
            if (!needStarts) {
                //  (Only the ends are used.)
            } else if (set.singleCharacter) {
                m.starts = m.ends;
                m.spans = m.ends;
            } else if (set.startsByFirst) {
                UCD::UnicodeSet firstChars = set.startChars;
                for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) {
                    if (n0 > 0) firstChars = firstChars + r.first.front();
                }
                //  (The ends, within the spans, are included so that they are used.)
                m.starts = combine(combine(m.spans, m.ends, true), classes({firstChars}, U21), false);
            } else {
                //  The positions of the spans after a position outside them, or after
                //  the end of a match (no match ends within another).
                m.starts = P.CreateStreamSet(1);
                P.CreateKernelCall<MatchStartsFromEnds>(m.spans, m.ends, m.starts);
                //  Where the matches of the sets are selected: every start, as matches
                //  beginning within another of the set apply where that one does not
                //  (the rest of the span, after the first starts, still covered).
                if (set.exactStarts) {
                    m.firstStarts = m.starts;
                    std::vector<re::RE *> lookahead = items;
                    if (set.greedyStarts) {
                        //  (The captures matched greedily: the same matches, see greedyStarts.)
                        lookahead.clear();
                        for (size_t j = 0; j < n0; j++) lookahead.push_back(items[j]);
                        for (size_t i = 0; i < set.captures.size(); i++) {
                            lookahead.push_back(ldml::expandedContext(set.captures[i], Insertions(), DeletionMark));
                            for (const UCD::UnicodeSet & chars : set.texts[i]) lookahead.push_back(re::makeCC(chars, &cc::Unicode));
                        }
                    }
                    m.starts = matchStarts(set.engineBefore, set.engineAfter, lookahead, U21);
                }
            }
            if (m.starts && mask) m.starts = combine(m.starts, mask, false);
            captureMatches.push_back(m);
        }
    };
    //  A group with string rules whose before contexts are matched against the
    //  converted text, and which must be selected (or whose coverage other rules
    //  need): all its rules in one correction kernel, as which matches apply, and
    //  where the single-character rules may apply, then depend on the converted
    //  text too.
    bool wholeGroup = false;
    if (correct && (!step.stringOrder.empty() || needCoverage)) {
        for (size_t r = 0; r < numStringRules; r++) wholeGroup |= convertible[r];
        if (numStringRules > FusedMaxWholeGroupRules || step.subgroups.size() > FusedMaxWholeGroupRules) wholeGroup = false;
        //  (The coverage of the MultipleCaptureMultipleReplacement rules is not known to the correction kernel.)
        if (!step.captureReplacements.empty()) wholeGroup = false;
    }
    if (wholeGroup) {
        FusedRulesConfig config;
        std::vector<size_t> order;
        if (!step.stringOrder.empty()) {
            for (size_t r : step.stringOrder) {
                if (!texts[r].empty()) order.push_back(r);
            }
        } else {
            for (size_t r = 0; r < numStringRules; r++) {
                if (!texts[r].empty()) order.push_back(r);
            }
        }
        std::vector<size_t> index(numStringRules, SIZE_MAX);
        for (size_t i = 0; i < order.size(); i++) index[order[i]] = i;
        for (size_t r : order) {
            FusedStringRule fr = fusedRule(r);
            if (convertible[r]) {
                fr.before = convertedBefores[r];
                fr.converted = true;
            } else if (fusedMatch[r]) {
                fr.converted = false;
            } else {
                fr.before = {FusedContext()};
                fr.after = {FusedContext()};
                fr.match = static_cast<int>(config.sources.size());
                config.sources.push_back(FusedMatchSource{engineFirsts(r), 0});
            }
            config.strings.push_back(std::move(fr));
            if (!step.stringOrder.empty()) {
                FusedRulesConfig::Selection sel;
                sel.length = static_cast<unsigned>(texts[r].size());
                sel.selfOverlapping = step.stringRules[r].selfOverlapping;
                for (size_t a : step.stringPredecessors[r]) {
                    if (index[a] != SIZE_MAX) sel.predecessors.push_back(index[a]);
                }
                config.selection.push_back(std::move(sel));
            }
        }
        config.groupCoverage = needCoverage;
        for (size_t g = 0; g < step.subgroups.size(); g++) {
            const ldml::CharMapSubgroup & subgroup = step.subgroups[g];
            if (maps[g].empty()) continue;
            FusedSubgroup fg;
            fg.mappings = maps[g];
            FusedContexts starBefore;
            if (fusedContexts(subgroup.engineAfter, false, fg.after) && fusedContexts(subgroup.engineBefore, true, starBefore, true)
                    && convertedContextMatters(starBefore, outcomes)) {
                fg.before = starBefore;
                fg.converted = true;
            } else if (fusedContexts(subgroup.engineBefore, true, fg.before) && fusedContexts(subgroup.engineAfter, false, fg.after)) {
                fg.converted = false;
            } else {
                fg.before = {FusedContext()};
                fg.after = {FusedContext()};
                UCD::UnicodeSet all;
                for (const ldml::CharMapping & m : maps[g]) all = all + m.chars;
                fg.match = static_cast<int>(config.sources.size());
                config.sources.push_back(FusedMatchSource{matchPositions(subgroup.engineBefore, subgroup.engineAfter, {all}, U21), 0});
            }
            config.subgroups.push_back(std::move(fg));
        }
        config.rounds = ConvertedContextRounds;
        for (const ConversionOutcome & o : outcomes) {
            config.replacementChars = config.replacementChars + o.resultChars();
        }
        if (step.sequentialSelection) {
            //  The string matches are selected sequentially, between the rounds:
            //  each round is a kernel matching the before contexts against the
            //  converted text of the previous round, the selection of the matches
            //  (LeftmostMatches), and a kernel applying the rules where selected.
            std::vector<MatchClass> byLengthOf;
            for (const FusedStringRule & r : config.strings) {
                const MatchClass k{static_cast<unsigned>(r.text.size()), r.run};
                if (std::find(byLengthOf.begin(), byLengthOf.end(), k) == byLengthOf.end()) byLengthOf.push_back(k);
            }
            const SelectionClasses selection = selectionClasses(byLengthOf);
            config.staged = true;
            config.selection.clear();
            StreamSet * const classes = fusedClasses(config);
            FusedState previous;
            for (unsigned round = 0; round <= ConvertedContextRounds; round++) {
                FusedRulesConfig matching = config;
                matching.matchOnly = true;
                matching.subgroups.clear();
                matching.startsByLength = byLengthOf;
                StreamSet * const starts = P.CreateStreamSet(byLengthOf.size());
                const FusedState none;
                P.CreateKernelCall<FusedRules>(std::move(matching), U21, classes, inText, mask, nullptr, previous, none, starts);
                StreamSet * const selected = P.CreateStreamSet(1);
                P.CreateKernelCall<LeftmostMatches>(selection.lengths, selection.runOf, starts, selection.runs, selected);
                FusedRulesConfig applying = config;
                applying.selected = static_cast<int>(applying.sources.size());
                applying.sources.push_back(FusedMatchSource{selected, 0});
                const FusedLayout layout(applying, previous);
                FusedState out;
                for (unsigned i = 0; i < layout.changePositions; i++) out.changes.push_back(P.CreateStreamSet(21));
                for (unsigned k : layout.insertCounts) out.insertions[k] = P.CreateStreamSet(1);
                P.CreateKernelCall<FusedRules>(std::move(applying), U21, classes, inText, mask, nullptr, previous, out, nullptr);
                previous = out;
            }
            state = previous;
        } else {
            applyFused(std::move(config), nullptr);
        }
    } else {
        std::vector<size_t> applying;               // the string rules, by their matches
        std::vector<FusedMatchSource> applied;      // and the source of their matches that apply
        StreamSet * selected = nullptr;             // the first positions of the matches that apply
        if (step.sequentialSelection) {
            //  The matches that apply are selected sequentially, from the left (see
            //  LeftmostMatches), given the matches of the rules by the lengths of
            //  their texts.  The rules are then applied where selected.
            auto classOf = [&](size_t r) {return MatchClass{static_cast<unsigned>(texts[r].size()), step.stringRules[r].run};};
            std::vector<MatchClass> byLengthOf;
            for (size_t r = 0; r < numStringRules; r++) {
                const MatchClass k = classOf(r);
                if (k.length > 0 && std::find(byLengthOf.begin(), byLengthOf.end(), k) == byLengthOf.end()) byLengthOf.push_back(k);
            }
            auto lengthIndex = [&](const MatchClass & k) {
                return std::find(byLengthOf.begin(), byLengthOf.end(), k) - byLengthOf.begin();
            };
            std::vector<std::vector<SelectStringMatches::Source>> byLength(byLengthOf.size());
            std::vector<size_t> fusedRules;
            for (size_t r = 0; r < numStringRules; r++) {
                if (texts[r].empty()) continue;
                applying.push_back(r);
                if (fusedMatch[r]) {
                    fusedRules.push_back(r);
                    applied.push_back(FusedMatchSource{});
                } else {
                    StreamSet * const firsts = engineFirsts(r);
                    byLength[lengthIndex(classOf(r))].push_back(SelectStringMatches::Source{firsts, 0});
                    applied.push_back(FusedMatchSource{firsts, 0});
                }
            }
            for (const std::vector<size_t> & chunk : fusedStringChunks(lengths, fusedRules)) {
                FusedRulesConfig config;
                config.matchOnly = true;
                for (size_t r : chunk) {
                    config.strings.push_back(fusedRule(r));
                    const MatchClass k = classOf(r);
                    if (std::find(config.startsByLength.begin(), config.startsByLength.end(), k) == config.startsByLength.end()) {
                        config.startsByLength.push_back(k);
                    }
                }
                const std::vector<MatchClass> chunkLengths = config.startsByLength;
                StreamSet * const matches = P.CreateStreamSet(chunkLengths.size());
                const FusedState none;
                StreamSet * const classes = fusedClasses(config);
                P.CreateKernelCall<FusedRules>(std::move(config), U21, classes, inText, mask, nullptr, none, none, matches);
                for (size_t k = 0; k < chunkLengths.size(); k++) {
                    byLength[lengthIndex(chunkLengths[k])].push_back(SelectStringMatches::Source{matches, static_cast<unsigned>(k)});
                }
            }
            StreamSet * starts = P.CreateStreamSet(byLengthOf.size());
            P.CreateKernelCall<MatchStartsByLength>(byLength, starts);
            selected = P.CreateStreamSet(1);
            SelectionClasses selection = selectionClasses(byLengthOf);
            if (step.jointSelection) {
                //  With the matches of the MultipleCaptureMultipleReplacement sets
                //  (after the string rules, which take precedence at a position): a
                //  class for each set, of its starts, followed by a run of the rest of
                //  its span.
                matchCaptures();
                std::vector<StreamSet *> startSets{starts}, runSets;
                std::vector<std::pair<unsigned, unsigned>> startOrder, runOrder;
                for (unsigned k = 0; k < byLengthOf.size(); k++) startOrder.emplace_back(0, k);
                const unsigned firstRun = selection.runs ? selection.runs->getNumElements() : 0;
                if (selection.runs) {
                    runSets.push_back(selection.runs);
                    for (unsigned k = 0; k < firstRun; k++) runOrder.emplace_back(0, k);
                }
                for (size_t i = 0; i < captureMatches.size(); i++) {
                    CaptureMatches & m = captureMatches[i];
                    StreamSet * const run = P.CreateStreamSet(1);
                    P.CreateKernelCall<SpanContinuation>(m.spans, m.firstStarts ? m.firstStarts : m.starts, run);
                    jointRuns.push_back(run);
                    startOrder.emplace_back(startSets.size(), 0);
                    startSets.push_back(m.starts);
                    runOrder.emplace_back(runSets.size(), 0);
                    runSets.push_back(run);
                    selection.lengths.push_back(1);
                    selection.runOf.push_back(static_cast<int>(firstRun + i));
                }
                StreamSet * const allStarts = P.CreateStreamSet(startOrder.size());
                P.CreateKernelCall<MergeStreamSets>(startSets, startOrder, allStarts);
                StreamSet * const allRuns = P.CreateStreamSet(runOrder.size());
                P.CreateKernelCall<MergeStreamSets>(runSets, runOrder, allRuns);
                starts = allStarts;
                selection.runs = allRuns;
            }
            P.CreateKernelCall<LeftmostMatches>(selection.lengths, selection.runOf, starts, selection.runs, selected);
            SHOW_STREAM(selected);
            jointSelected = selected;
        } else if (!step.stringOrder.empty()) {
            //  String rules may match within the texts of others: the matches of the
            //  rules, in the order to apply them, from which those that apply are
            //  selected (see ldml::TransformStep::stringOrder).
            std::vector<size_t> rules;
            std::vector<size_t> index(numStringRules, SIZE_MAX);
            for (size_t r : step.stringOrder) {
                if (texts[r].empty()) continue;
                index[r] = rules.size();
                rules.push_back(r);
            }
            std::vector<SelectStringMatches::Source> firsts(rules.size());
            std::vector<size_t> fusedRules;
            for (size_t i = 0; i < rules.size(); i++) {
                if (fusedMatch[rules[i]]) {
                    fusedRules.push_back(rules[i]);
                } else {
                    firsts[i] = SelectStringMatches::Source{engineFirsts(rules[i]), 0};
                }
            }
            for (const std::vector<size_t> & chunk : fusedStringChunks(lengths, fusedRules)) {
                FusedRulesConfig config;
                config.matchOnly = true;
                for (size_t r : chunk) config.strings.push_back(fusedRule(r));
                StreamSet * const matches = P.CreateStreamSet(chunk.size());
                const FusedState none;
                StreamSet * const classes = fusedClasses(config);
                P.CreateKernelCall<FusedRules>(std::move(config), U21, classes, inText, mask, nullptr, none, none, matches);
                for (size_t k = 0; k < chunk.size(); k++) firsts[index[chunk[k]]] = SelectStringMatches::Source{matches, static_cast<unsigned>(k)};
            }
            std::vector<SelectStringMatches::Rule> select;
            for (size_t r : rules) {
                SelectStringMatches::Rule sr;
                sr.length = static_cast<unsigned>(texts[r].size());
                sr.selfOverlapping = step.stringRules[r].selfOverlapping;
                for (size_t a : step.stringPredecessors[r]) {
                    if (index[a] != SIZE_MAX) sr.predecessors.push_back(index[a]);
                }
                select.push_back(std::move(sr));
            }
            StreamSet * const Applied = P.CreateStreamSet(rules.size());
            StreamSet * const inside = needCoverage ? P.CreateStreamSet(1) : nullptr;
            StreamSet * const first = needCoverage ? P.CreateStreamSet(1) : nullptr;
            P.CreateKernelCall<SelectStringMatches>(select, firsts, Applied, inside, first);
            SHOW_BIXNUM(Applied);
            for (size_t i = 0; i < rules.size(); i++) {
                applying.push_back(rules[i]);
                applied.push_back(FusedMatchSource{Applied, static_cast<unsigned>(i)});
            }
            //  The spans of the matches that apply.
            if (needCoverage) {
                state.spans = combine(inside, first, true);
                state.starts = first;
            }
        } else {
            for (size_t r = 0; r < numStringRules; r++) {
                if (texts[r].empty() || correctedString[r]) continue;
                applying.push_back(r);
                applied.push_back(fusedMatch[r] ? FusedMatchSource{} : FusedMatchSource{engineFirsts(r), 0});
            }
        }

        //  Phase 2.  The changes at the positions of the characters, for each
        //  position of the replacements, and the positions after which positions are
        //  to be inserted, by their number: of the string rules, in chunks by their
        //  lengths, ...
        {
            std::vector<size_t> order(applying.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::vector<std::pair<size_t, size_t>> applyingLengths;
            for (size_t r : applying) applyingLengths.push_back(lengths[r]);
            for (const std::vector<size_t> & chunk : fusedStringChunks(applyingLengths, order)) {
                FusedRulesConfig config;
                config.coverage = needCoverage && (step.stringOrder.empty() || selected);
                if (selected) {
                    config.selected = static_cast<int>(config.sources.size());
                    config.sources.push_back(FusedMatchSource{selected, 0});
                }
                for (size_t i : chunk) {
                    FusedStringRule fr = fusedRule(applying[i]);
                    if (applied[i].streamSet) {
                        fr.match = static_cast<int>(config.sources.size());
                        config.sources.push_back(applied[i]);
                    }
                    config.strings.push_back(std::move(fr));
                }
                applyFused(std::move(config), nullptr);
            }
        }
        //  ... of the MultipleCaptureMultipleReplacement rules: for each set, the
        //  spans of its matches, found by the regular expression engine, and their
        //  ends.  The captures stay in place: each Ti is replaced by Ri (T0 by R0
        //  at the start of each match, as string rules applied there), or Ri is
        //  inserted around a kept character of a capture (without a second spread
        //  with InsertionSpreadMask's InsertPosition::Before, which an insertion
        //  before the start of a match would otherwise need).  The spans are covered.
        if (!capturesMatched) matchCaptures();
        //  Where the matches of different sets may overlap, those that apply are
        //  selected from the left (see LeftmostMatches), the earlier set first at the
        //  same position: a class for each set, of its starts, followed by a run of
        //  the rest of its span.  The spans and ends are then those of the matches
        //  selected.
        if (step.captureSelection) {
            std::vector<StreamSet *> runStreams = jointRuns;
            StreamSet * selected = jointSelected;
            if (!step.jointSelection) {
                std::vector<StreamSet *> startStreams;
                for (CaptureMatches & m : captureMatches) {
                    StreamSet * const run = P.CreateStreamSet(1);
                    P.CreateKernelCall<SpanContinuation>(m.spans, m.firstStarts ? m.firstStarts : m.starts, run);
                    startStreams.push_back(m.starts);
                    runStreams.push_back(run);
                }
                StreamSet * const starts = P.CreateStreamSet(startStreams.size());
                P.CreateKernelCall<CollectStreams>(startStreams, starts);
                StreamSet * const runs = P.CreateStreamSet(runStreams.size());
                P.CreateKernelCall<CollectStreams>(runStreams, runs);
                std::vector<unsigned> lengths(captureMatches.size(), 1);
                std::vector<int> runOf;
                for (size_t i = 0; i < captureMatches.size(); i++) runOf.push_back(static_cast<int>(i));
                selected = P.CreateStreamSet(1);
                P.CreateKernelCall<LeftmostMatches>(lengths, runOf, starts, runs, selected);
                SHOW_STREAM(selected);
            }
            for (size_t i = 0; i < captureMatches.size(); i++) {
                const ldml::MultipleCaptureMultipleReplacementSet & set = step.captureReplacements[i];
                CaptureMatches & m = captureMatches[i];
                StreamSet * const chosen = P.CreateStreamSet(3);
                P.CreateKernelCall<SelectedSpans>(m.starts, runStreams[i], m.ends, selected, chosen);
                //  (Only the streams used are selected.)
                bool usesStarts = !set.rules.front().first.empty() || needCoverage || set.reversed || set.moved, usesEnds = false;
                for (const ldml::BoundaryReplacement & br : set.replacements) {
                    usesStarts |= !br.fromEnd;
                    usesEnds |= br.fromEnd;
                }
                auto select = [&](unsigned k) {
                    StreamSet * const out = P.CreateStreamSet(1);
                    P.CreateKernelCall<StreamSelect>(out, Select(chosen, {k}));
                    return out;
                };
                m.starts = usesStarts ? select(0) : nullptr;
                m.spans = needCoverage ? select(1) : nullptr;
                m.ends = usesEnds ? select(2) : nullptr;
            }
        }
        for (size_t setIndex = 0; setIndex < step.captureReplacements.size(); setIndex++) {
            const ldml::MultipleCaptureMultipleReplacementSet & set = step.captureReplacements[setIndex];
            const size_t n0 = set.rules.front().first.size();
            StreamSet * const spans = captureMatches[setIndex].spans;
            StreamSet * const ends = captureMatches[setIndex].ends;
            StreamSet * const starts = captureMatches[setIndex].starts;
            if (spans) SHOW_STREAM(spans);
            SHOW_STREAM(ends);
            if (starts) SHOW_STREAM(starts);
            //  Captures referred to in reverse order: the first character of B1,
            //  which stays in place, marks the region of the match to reverse.
            if (set.reversed) {
                StreamSet * b1 = starts;
                if (n0 > 0) {
                    b1 = P.CreateStreamSet(1);
                    P.CreateKernelCall<ShiftForward>(starts, b1, static_cast<unsigned>(n0));
                }
                //  With T0 empty, B1 is kept after the text R0 R1 inserted before it.
                const int offset = (n0 == 0 ? static_cast<int>(set.rules.front().firstReplacement.size()) : 0)
                                   - static_cast<int>(set.swapBefore);
                swaps.push_back(Swap{b1, offset, set.swapLength, set.swapDistance});
            }
            if (set.moved) {
                //  The captures found by their possessive walk from the starts.
                CaptureWalkConfig walk;
                walk.textLengths.push_back(static_cast<unsigned>(n0));
                for (size_t c = 0; c < set.captures.size(); c++) {
                    walk.captures.emplace_back();
                    walkItems(ldml::expandedContext(set.captures[c], Insertions(), DeletionMark), walk.captures.back());
                    walk.textLengths.push_back(static_cast<unsigned>(set.texts[c].size()));
                }
                walk.first = set.moveFirst;
                walk.moved = set.moveCapture;
                StreamSet * const marks = P.CreateStreamSet(3);
                P.CreateKernelCall<CaptureWalk>(std::move(walk), U21, starts, marks);
                auto select = [&](unsigned k) {
                    StreamSet * const out = P.CreateStreamSet(1);
                    P.CreateKernelCall<StreamSelect>(out, Select(marks, {k}));
                    return out;
                };
                StreamSet * const endJ = P.CreateStreamSet(1);
                P.CreateKernelCall<ShiftBack>(select(2), endJ, 1);
                //  Capture i is kept after the text inserted before it (if any).
                int keptAt = 0;
                for (const ldml::BoundaryReplacement & br : set.replacements) {
                    if (br.kind == ldml::BoundaryReplacement::Kind::Keep && br.capture == set.moveFirst + 1 && br.keepAt > 0) {
                        keptAt = static_cast<int>(br.keepAt);
                    }
                }
                moves.push_back(Move{select(0), keptAt - static_cast<int>(set.swapBefore), select(1), endJ});
            }
            //  T0 by R0, the first rule whose T0 matches at the start applying.
            if (n0 > 0) {
                FusedRulesConfig config;
                config.selected = 0;
                config.sources.push_back(FusedMatchSource{starts, 0});
                UCD::UnicodeSet earlier;
                for (const ldml::MultipleCaptureMultipleReplacement & r : set.rules) {
                    FusedStringRule fr;
                    fr.text = r.first;
                    if (n0 == 1) fr.text.front() = fr.text.front() - earlier;
                    earlier = earlier + r.first.front();
                    if (fr.text.front().empty()) continue;
                    fr.replacement = r.firstReplacement;
                    config.strings.push_back(std::move(fr));
                }
                applyFused(std::move(config), nullptr);
            }
            //  The other replacements, each at its offset from the start or the end.
            std::vector<StreamSet *> positions;
            for (const ldml::BoundaryReplacement & br : set.replacements) {
                StreamSet * at = br.fromEnd ? ends : starts;
                if (br.offset > 0) {
                    StreamSet * const shifted = P.CreateStreamSet(1);
                    if (br.fromEnd) {
                        P.CreateKernelCall<ShiftBack>(ends, shifted, br.offset);
                    } else {
                        P.CreateKernelCall<ShiftForward>(starts, shifted, br.offset);
                    }
                    at = shifted;
                }
                positions.push_back(at);
            }
            auto apply = [&](const std::vector<UCD::UnicodeSet> & text, const std::u32string & replacement, int keep,
                             FusedMatchSource at) {
                FusedRulesConfig config;
                config.selected = 0;
                config.sources.push_back(at);
                FusedStringRule fr;
                fr.text = text;
                fr.replacement = replacement;
                fr.keep = keep;
                config.strings.push_back(std::move(fr));
                applyFused(std::move(config), nullptr);
            };
            std::vector<bool> done(set.replacements.size(), false);
            //  An insertion before a capture and one after it coincide where it
            //  has one character: there, both apply around the character.
            for (size_t a = 0; a < set.replacements.size(); a++) {
                const ldml::BoundaryReplacement & before = set.replacements[a];
                if (before.kind != ldml::BoundaryReplacement::Kind::Keep || before.keepAt != before.replacement.size()) continue;
                for (size_t b = 0; b < set.replacements.size(); b++) {
                    const ldml::BoundaryReplacement & after = set.replacements[b];
                    if (b == a || done[b] || after.kind != ldml::BoundaryReplacement::Kind::Keep || after.keepAt != 0
                            || after.capture != before.capture) continue;
                    StreamSet * const split = P.CreateStreamSet(3);
                    P.CreateKernelCall<CoincidentPositions>(positions[a], positions[b], split);
                    apply(before.text, before.replacement, static_cast<int>(before.keepAt), FusedMatchSource{split, 0});
                    apply(after.text, after.replacement, 0, FusedMatchSource{split, 1});
                    apply({before.text.front() & after.text.front()}, before.replacement + after.replacement,
                          static_cast<int>(before.keepAt), FusedMatchSource{split, 2});
                    done[a] = done[b] = true;
                    break;
                }
            }
            for (size_t i = 0; i < set.replacements.size(); i++) {
                if (done[i]) continue;
                const ldml::BoundaryReplacement & br = set.replacements[i];
                apply(br.text, br.replacement, br.kind == ldml::BoundaryReplacement::Kind::Keep ? static_cast<int>(br.keepAt) : -1,
                      FusedMatchSource{positions[i], 0});
            }
            if (needCoverage) {
                state.spans = combine(state.spans, spans, true);
                state.starts = combine(state.starts, starts, true);
            }
        }
        //  The positions where the single-character rules may apply: not within the
        //  applied matches of string rules (other than at their first positions), as
        //  in ICU, which does not apply rules within the text it has replaced, and
        //  within the mask.
        StreamSet * allowed = mask;
        if (needCoverage && state.spans) {
            SHOW_STREAM(state.spans);
            SHOW_STREAM(state.starts);
            StreamSet * const uncovered = P.CreateStreamSet(1);
            P.CreateKernelCall<UncoveredPositions>(state.spans, state.starts, uncovered);
            SHOW_STREAM(uncovered);
            allowed = combine(allowed, uncovered, false);
        }
        //  ... of the subgroups of single-character rules, in chunks, ...
        FusedRulesConfig correction;
        {
            FusedRulesConfig config;
            size_t mappings = 0;
            auto flush = [&]() {
                if (config.subgroups.empty()) return;
                applyFused(std::move(config), allowed);
                config = FusedRulesConfig();
                mappings = 0;
            };
            for (size_t g = 0; g < step.subgroups.size(); g++) {
                const ldml::CharMapSubgroup & subgroup = step.subgroups[g];
                if (maps[g].empty()) continue;
                FusedSubgroup fg;
                fg.mappings = maps[g];
                {
                    //  (With insertion rules, a before context may also see the texts
                    //  inserted.)
                    FusedContexts starBefore, after;
                    if (correct && subgroup.engineBefore && fusedContexts(subgroup.engineAfter, false, after)
                            && fusedContexts(subgroup.engineBefore, true, starBefore, true)
                            && (!step.insertions.empty() || convertedContextMatters(starBefore, outcomes))) {
                        fg.before = starBefore;
                        fg.after = after;
                        fg.converted = true;
                        correction.subgroups.push_back(std::move(fg));
                        continue;
                    }
                }
                if (!fusedContexts(subgroup.engineBefore, true, fg.before) || !fusedContexts(subgroup.engineAfter, false, fg.after)) {
                    fg.before = {FusedContext()};
                    fg.after = {FusedContext()};
                    UCD::UnicodeSet all;
                    for (const ldml::CharMapping & m : maps[g]) all = all + m.chars;
                    fg.match = static_cast<int>(config.sources.size());
                    config.sources.push_back(FusedMatchSource{matchPositions(subgroup.engineBefore, subgroup.engineAfter, {all}, U21), 0});
                }
                mappings += fg.mappings.size();
                config.subgroups.push_back(std::move(fg));
                if (config.subgroups.size() >= FusedMaxSubgroups || mappings >= FusedMaxRules) flush();
            }
            flush();
        }
        //  ... and, last, of the rules whose before contexts are matched against the
        //  converted text (given the results of all the others).
        for (size_t r = 0; r < numStringRules; r++) {
            if (correctedString[r]) {
                FusedStringRule fr = fusedRule(r);
                fr.before = convertedBefores[r];
                fr.converted = true;
                correction.strings.push_back(std::move(fr));
            }
        }
        //  ... with the insertion rules (see FusedInsertion).
        if (!step.insertions.empty()) {
            StreamSet * const eligible = insertionsEligible();
            int eligibleSource = -1;
            if (eligible) {
                eligibleSource = static_cast<int>(correction.sources.size());
                correction.sources.push_back(FusedMatchSource{eligible, 0});
            }
            for (const ldml::InsertionRule & ins : step.insertions) {
                FusedInsertion fi;
                fusedContexts(ins.engineBefore, true, fi.before, true);
                if (!fusedContexts(ins.engineAfter, false, fi.after)) {
                    //  The positions before those where the after context matches.
                    fi.after = {FusedContext()};
                    fi.match = static_cast<int>(correction.sources.size());
                    correction.sources.push_back(FusedMatchSource{matchPositions(nullptr, ins.engineAfter,
                                                                                 {UCD::UnicodeSet(0, 0x10FFFF)}, U21), 0});
                }
                fi.eligible = eligibleSource;
                fi.text = ins.replacement;
                for (char32_t ch : ins.replacement) correction.replacementChars.insert(ch);
                correction.insertions.push_back(std::move(fi));
            }
        }
        if (!correction.strings.empty() || !correction.subgroups.empty() || !correction.insertions.empty()) {
            correction.rounds = ConvertedContextRounds;
            for (const ConversionOutcome & o : outcomes) {
                correction.replacementChars = correction.replacementChars + o.resultChars();
            }
            applyFused(std::move(correction), allowed);
        }
    }
    std::vector<StreamSet *> changesAt = state.changes;
    std::map<unsigned, StreamSet *> inserting = state.insertions;

    //  Phase 3.  The positions inserted, and the changes applied.
    StreamSet * result = U21;
    StreamSet * changes = changesAt.empty() ? nullptr : changesAt[0];
    if (!inserting.empty()) {
        std::vector<unsigned> counts;
        std::vector<StreamSet *> positions;
        unsigned maxCount = 0;
        for (const auto & k : inserting) {
            counts.push_back(k.first);
            positions.push_back(k.second);
            maxCount = std::max(maxCount, k.first);
        }
        unsigned bits = 0;
        while ((maxCount >> bits) != 0) bits++;
        StreamSet * const InsertBixNum = P.CreateStreamSet(bits);
        P.CreateKernelCall<InsertionCounts>(counts, positions, InsertBixNum);
        SHOW_BIXNUM(InsertBixNum);
        StreamSet * const SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, InsertBixNum, SpreadMask, kernel::InsertPosition::After);
        SHOW_STREAM(SpreadMask);
        StreamSet * const Expanded = P.CreateStreamSet(21, 1);
        SpreadByMask(P, SpreadMask, U21, Expanded);
        StreamSet * const Marked = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<FillInsertedPositions>(SpreadMask, Expanded, Marked);
        SHOW_BIXNUM(Marked);
        result = Marked;
        spreadMasks(SpreadMask);
        for (Swap & sw : swaps) {
            StreamSet * const spread = P.CreateStreamSet(1);
            SpreadByMask(P, SpreadMask, sw.marks, spread);
            sw.marks = spread;
        }
        for (Move & mv : moves) {
            for (StreamSet ** m : {&mv.startI, &mv.startJ, &mv.endJ}) {
                StreamSet * const spread = P.CreateStreamSet(1);
                SpreadByMask(P, SpreadMask, *m, spread);
                *m = spread;
            }
        }
        //  The changes of each position i of the replacements, spread with the
        //  text, and moved to the i-th position inserted after their characters.
        changes = nullptr;
        for (size_t i = 0; i < changesAt.size(); i++) {
            if (changesAt[i] == nullptr) continue;
            StreamSet * spread = P.CreateStreamSet(21);
            SpreadByMask(P, SpreadMask, changesAt[i], spread);
            if (i > 0) {
                StreamSet * const shifted = P.CreateStreamSet(21);
                P.CreateKernelCall<ShiftForward>(spread, shifted, static_cast<unsigned>(i));
                spread = shifted;
            }
            changes = combine(changes, spread, true);
        }
    }
    if (changes) {
        StreamSet * const changed = P.CreateStreamSet(21);
        XorCombine(P, result, changes, changed);
        SHOW_BIXNUM(changed);
        result = changed;
    }
    if (needDeletion) {
        StreamSet * const marks = classes({UCD::UnicodeSet(DeletionMark)}, result);
        StreamSet * const keep = P.CreateStreamSet(1);
        Invert(P, marks, keep);
        SHOW_STREAM(keep);
        StreamSet * const filtered = P.CreateStreamSet(21);
        FilterByMask(P, keep, result, filtered);
        result = filtered;
        filterMasks(keep);
        for (Swap & sw : swaps) {
            StreamSet * const marks = P.CreateStreamSet(1);
            FilterByMask(P, keep, sw.marks, marks);
            sw.marks = marks;
        }
        for (Move & mv : moves) {
            for (StreamSet ** m : {&mv.startI, &mv.startJ, &mv.endJ}) {
                StreamSet * const filtered = P.CreateStreamSet(1);
                FilterByMask(P, keep, *m, filtered);
                *m = filtered;
            }
        }
    }
    //  The regions reversed (their last characters moved before the others).
    for (const Swap & sw : swaps) {
        StreamSet * marks = sw.marks;
        if (sw.offset != 0) {
            StreamSet * const shifted = P.CreateStreamSet(1);
            if (sw.offset > 0) {
                P.CreateKernelCall<ShiftForward>(marks, shifted, static_cast<unsigned>(sw.offset));
            } else {
                P.CreateKernelCall<ShiftBack>(marks, shifted, static_cast<unsigned>(-sw.offset));
            }
            marks = shifted;
        }
        SHOW_STREAM(marks);
        StreamSet * const swapped = P.CreateStreamSet(21);
        P.CreateKernelCall<SwapBack>(marks, result, swapped, sw.length, sw.distance);
        result = swapped;
    }
    //  The regions moved after capture j: their characters taken out, in order
    //  (FilterByMask), and put into as many positions inserted after capture j
    //  (InsertionSpreadMask, MergeByMask).  The marks of the regions moved later,
    //  and the masks, are kept in step.
    for (size_t m = 0; m < moves.size(); m++) {
        const Move & mv = moves[m];
        StreamSet * xStart = mv.startI;
        if (mv.offset != 0) {
            xStart = P.CreateStreamSet(1);
            if (mv.offset > 0) {
                P.CreateKernelCall<ShiftForward>(mv.startI, xStart, static_cast<unsigned>(mv.offset));
            } else {
                P.CreateKernelCall<ShiftBack>(mv.startI, xStart, static_cast<unsigned>(-mv.offset));
            }
        }
        StreamSet * const x = P.CreateStreamSet(1);
        StreamSet * const j = P.CreateStreamSet(1);
        P.CreateKernelCall<MoveRegions>(xStart, mv.startJ, mv.endJ, x, j);
        SHOW_STREAM(x);
        StreamSet * const index = P.CreateStreamSet(6);
        P.CreateKernelCall<RunIndex>(x, index);
        StreamSet * const counts = P.CreateStreamSet(7);
        P.CreateKernelCall<MoveCounts>(x, index, mv.startJ, j, mv.endJ, counts);
        StreamSet * const keepRest = P.CreateStreamSet(1);
        Invert(P, x, keepRest);
        StreamSet * const extracted = P.CreateStreamSet(21);
        FilterByMask(P, x, result, extracted);
        StreamSet * const rest = P.CreateStreamSet(21);
        FilterByMask(P, keepRest, result, rest);
        StreamSet * const restCounts = P.CreateStreamSet(7);
        FilterByMask(P, keepRest, counts, restCounts);
        StreamSet * const SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, restCounts, SpreadMask, kernel::InsertPosition::After);
        StreamSet * const inserted = P.CreateStreamSet(1);
        Invert(P, SpreadMask, inserted);
        StreamSet * const merged = P.CreateStreamSet(21);
        MergeByMask(P, inserted, extracted, rest, merged);
        result = merged;
        filterMasks(keepRest);
        spreadMasks(SpreadMask);
        for (size_t later = m + 1; later < moves.size(); later++) {
            for (StreamSet ** mk : {&moves[later].startI, &moves[later].startJ, &moves[later].endJ}) {
                StreamSet * const filtered = P.CreateStreamSet(1);
                FilterByMask(P, keepRest, *mk, filtered);
                StreamSet * const spread = P.CreateStreamSet(1);
                SpreadByMask(P, SpreadMask, filtered, spread);
                *mk = spread;
            }
        }
    }
    return result;
}

StreamSet * RulePipelineBuilder::transform(const ldml::TransformEntry * entry, StreamSet * U21) {
    if (entry->isBuiltIn()) return builtIn(entry->canonicalName, U21);
    const TransformAnalysis & a = mAnalyzer.analyze(entry);
    assert (a.implementable);
    return applyAnalysis(a, U21);
}

//  The deferred rules of a group (see ldml::DeferredRule and DeferredPlan).
StreamSet * RulePipelineBuilder::deferredRules(const ldml::TransformStep & step, StreamSet * U21) {
    PipelineBuilder & P = mP;
    //  (The bounds of the buffers grow exponentially with the rounds, see DeferredRunBits.)
    if (DeferredRounds > 6) llvm::report_fatal_error("tconv: --deferred-rule-rounds is at most 6");
    DeferredConfig config;
    for (const ldml::DeferredRule & d : step.deferred) {
        config.textLengths.push_back(static_cast<unsigned>(d.text.size()));
        config.replacements.push_back(d.replacement);
        config.captures.push_back(d.capture);
    }
    config.numTails = static_cast<unsigned>(step.deferredTails.size());
    std::vector<UCD::UnicodeSet> classSets{step.deferredTriggers};
    for (const UCD::UnicodeSet & t : step.deferredTails) classSets.push_back(t);
    classSets.push_back(step.deferredReach);
    for (const ldml::CopyCapture & c : step.copyCaptures) {
        config.prefix.push_back(static_cast<unsigned>(c.prefix.size()));
        config.suffix.push_back(static_cast<unsigned>(c.suffix.size()));
        config.starClass.push_back(c.star ? static_cast<int>(classSets.size()) : -1);
        if (c.star) classSets.push_back(c.starSet);
    }
    //  The self-reproducing rules (their indexes, the bits of an index, the lengths
    //  of their texts), the distinct texts and after contexts of the deferred
    //  rules (the match of each), and the longest block written.
    std::vector<int> srcIndex;
    std::vector<unsigned> srcTextLengths;
    for (const ldml::DeferredRule & d : step.deferred) {
        srcIndex.push_back(d.selfReproducing ? static_cast<int>(srcTextLengths.size()) : -1);
        if (d.selfReproducing) srcTextLengths.push_back(static_cast<unsigned>(d.text.size()));
    }
    const unsigned srcCount = static_cast<unsigned>(srcTextLengths.size());
    unsigned srcBits = 1;
    while ((srcCount >> srcBits) != 0) srcBits++;
    auto matchKey = [](const ldml::DeferredRule & d) {
        re::RE * const after = d.rule->getSourceSide(ldml::Direction::Forward)->getAfterContext();
        std::string key = after ? ldml::printPattern(after) : "";
        for (const UCD::UnicodeSet & chars : d.text) {
            key += "|";
            for (const auto & range : chars) key += std::to_string(range.first) + "-" + std::to_string(range.second) + ",";
        }
        return key;
    };
    std::vector<size_t> matchKeys;
    std::vector<unsigned> matchOf;
    for (size_t i = 0; i < step.deferred.size(); i++) {
        size_t k = 0;
        while (k < matchKeys.size() && matchKey(step.deferred[matchKeys[k]]) != matchKey(step.deferred[i])) k++;
        if (k == matchKeys.size()) matchKeys.push_back(i);
        matchOf.push_back(static_cast<unsigned>(k));
    }
    unsigned maxBlock = 0;
    for (const ldml::DeferredRule & d : step.deferred) {
        if (d.selfReproducing) maxBlock = std::max(maxBlock, static_cast<unsigned>(d.replacement.size()) + config.maxCopy());
    }
    //  The positions written by the deferred rules in the rounds so far.
    StreamSet * written = nullptr;
    for (unsigned round = 0; round < DeferredRounds; round++) {
        StreamSet * const mask = activeMask();
        config.masked = mask != nullptr;
        config.written = written != nullptr;
        StreamSet * const classStreams = classes(classSets, U21);
        std::vector<StreamSet *> matches;
        for (const ldml::DeferredRule & d : step.deferred) {
            matches.push_back(matchPositions(d.engineBefore, d.engineAfter, d.text, U21));
        }
        //  The runs of the sets of the repetitions (with the overflow of the index).
        std::vector<StreamSet *> runs;
        for (size_t k = 0; k < step.copyCaptures.size(); k++) {
            if (config.starClass[k] < 0) continue;
            StreamSet * const index = P.CreateStreamSet(DeferredRunBits);
            StreamSet * const overflow = P.CreateStreamSet(1);
            P.CreateKernelCall<RunIndex>(streamutils::Select(P, classStreams, config.starClass[k]), index, overflow);
            StreamSet * const indexed = P.CreateStreamSet(DeferredRunBits + 1);
            std::vector<std::pair<unsigned, unsigned>> order;
            for (unsigned b = 0; b < DeferredRunBits; b++) order.emplace_back(0, b);
            order.emplace_back(1, 0);
            P.CreateKernelCall<MergeStreamSets>(std::vector<StreamSet *>{index, overflow}, order, indexed);
            runs.push_back(indexed);
        }
        StreamSet * const count = P.CreateStreamSet(config.countBits());
        StreamSet * const info = P.CreateStreamSet(step.deferred.size() + 1);
        StreamSet * const reach = P.CreateStreamSet(std::max(1u, config.maxCopy()));
        P.CreateKernelCall<DeferredPlan>(config, matches, classStreams, runs, mask, written, count, info, reach);
        SHOW_BIXNUM(count);
        StreamSet * const region = P.CreateStreamSet(1);
        P.CreateKernelCall<DeferredRegion>(reach, region);
        SHOW_STREAM(region);
        StreamSet * const SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, count, SpreadMask, kernel::InsertPosition::After);
        StreamSet * const spreadU = P.CreateStreamSet(21);
        SpreadByMask(P, SpreadMask, U21, spreadU);
        StreamSet * const spreadInfo = P.CreateStreamSet(info->getNumElements());
        SpreadByMask(P, SpreadMask, info, spreadInfo);
        StreamSet * const extracted = P.CreateStreamSet(21);
        FilterByMask(P, region, U21, extracted);
        StreamSet * const slots = P.CreateStreamSet(1);
        StreamSet * const chars = P.CreateStreamSet(21);
        StreamSet * const keep = P.CreateStreamSet(1);
        P.CreateKernelCall<DeferredSlots>(config, spreadInfo, SpreadMask, slots, chars, keep);
        StreamSet * const withR = combine(spreadU, chars, true);
        StreamSet * const notSlots = P.CreateStreamSet(1);
        Invert(P, slots, notSlots);
        StreamSet * const others = P.CreateStreamSet(21);
        FilterByMask(P, notSlots, withR, others);
        StreamSet * const merged = P.CreateStreamSet(21);
        MergeByMask(P, slots, extracted, others, merged);
        StreamSet * const result = P.CreateStreamSet(21);
        FilterByMask(P, keep, merged, result);
        spreadMasks(SpreadMask);
        filterMasks(keep);
        StreamSet * const expanded = P.CreateStreamSet(1);
        if (written) {
            ExpandFilter(P, SpreadMask, written, expanded);
        } else {
            Invert(P, SpreadMask, expanded);
        }
        StreamSet * const writtenNext = P.CreateStreamSet(1);
        FilterByMask(P, keep, expanded, writtenNext);
        written = writtenNext;
        SHOW_BIXNUM(result);
        U21 = result;
        //  The runs of the texts of the self-reproducing rules applied (see
        //  DeferredRuns): the blocks they wrote, and where their texts may follow.
        if (srcCount > 0) {
            StreamSet * const spreadBlocks = P.CreateStreamSet(2 + srcBits);
            P.CreateKernelCall<DeferredHeadBlocks>(srcIndex, srcBits, spreadInfo, SpreadMask, spreadBlocks);
            StreamSet * const blocks = P.CreateStreamSet(2 + srcBits);
            FilterByMask(P, keep, spreadBlocks, blocks);
            std::vector<StreamSet *> textMatches;
            for (const auto & key : matchKeys) {
                const ldml::DeferredRule & d = step.deferred[key];
                textMatches.push_back(matchPositions(nullptr, d.engineAfter, d.text, U21));
            }
            StreamSet * const candidates = P.CreateStreamSet(srcCount);
            std::vector<std::vector<size_t>> interferers;
            for (const ldml::DeferredRule & d : step.deferred) interferers.push_back(d.runInterferers);
            P.CreateKernelCall<DeferredCandidates>(srcIndex, matchOf, config.textLengths, interferers, srcBits, textMatches,
                                                   written, activeMask(), candidates);
            SHOW_STREAM(candidates);
            //  The text with the masks and the text written, kept in step.
            std::vector<StreamSet *> textSets{U21};
            std::vector<std::pair<unsigned, unsigned>> textOrder;
            for (unsigned b = 0; b < 21; b++) textOrder.emplace_back(0, b);
            for (StreamSet * m : mMasks) {
                textOrder.emplace_back(textSets.size(), 0);
                textSets.push_back(m);
            }
            textOrder.emplace_back(textSets.size(), 0);
            textSets.push_back(written);
            StreamSet * const textIn = P.CreateStreamSet(textOrder.size());
            P.CreateKernelCall<MergeStreamSets>(textSets, textOrder, textIn);
            std::vector<std::pair<unsigned, unsigned>> markOrder{{0, 0}, {0, 1}};
            for (unsigned k = 0; k < srcBits; k++) markOrder.emplace_back(0, 2 + k);
            for (unsigned r = 0; r < srcCount; r++) markOrder.emplace_back(1, r);
            StreamSet * const marks = P.CreateStreamSet(markOrder.size());
            P.CreateKernelCall<MergeStreamSets>(std::vector<StreamSet *>{blocks, candidates}, markOrder, marks);
            StreamSet * const textOut = P.CreateStreamSet(textOrder.size());
            P.CreateKernelCall<DeferredRuns>(srcTextLengths, maxBlock, textIn, marks, textOut);
            auto select = [&](unsigned k) {
                StreamSet * const out = P.CreateStreamSet(1);
                P.CreateKernelCall<StreamSelect>(out, Select(textOut, {k}));
                return out;
            };
            StreamSet * const runs = P.CreateStreamSet(21);
            std::vector<uint32_t> bits;
            for (unsigned b = 0; b < 21; b++) bits.push_back(b);
            P.CreateKernelCall<StreamSelect>(runs, Select(textOut, bits));
            for (size_t m = 0; m < mMasks.size(); m++) mMasks[m] = select(static_cast<unsigned>(21 + m));
            written = select(static_cast<unsigned>(21 + mMasks.size()));
            SHOW_BIXNUM(runs);
            U21 = runs;
        }
    }
    return U21;
}

StreamSet * RulePipelineBuilder::applyAnalysis(const TransformAnalysis & a, StreamSet * U21) {
    const bool invokedWithFilter = mInvokedWithFilter;
    mInvokedWithFilter = false;
    const bool insertAnywhere = mInsertAnywhere;
    mInsertAnywhere = a.plan.steps.size() == 1 && (a.plan.filter || invokedWithFilter);
    if (a.plan.filter) pushFilter(a.plan.filterSet, U21);
    for (const ldml::TransformStep & step : a.plan.steps) {
        if (step.kind == ldml::TransformStep::Kind::Transform) {
            if (step.filter) pushFilter(step.filterSet, U21);
            mInvokedWithFilter = step.filter != nullptr;
            U21 = transform(mRegistry.lookup(step.transform.getText()), U21);
            mInvokedWithFilter = false;
            if (step.filter) popFilter();
        } else {
            if (!step.subgroups.empty() || !step.stringRules.empty() || !step.captureReplacements.empty() || !step.insertions.empty()) {
                U21 = conversionGroup(step, U21);
            }
            if (!step.deferred.empty()) U21 = deferredRules(step, U21);
        }
    }
    if (a.plan.filter) popFilter();
    mInsertAnywhere = insertAnywhere;
    return U21;
}

static TransformFunctionType generateRulePipeline(CPUDriver & driver, const ldml::TransformRegistry & registry,
                                                  TransformAnalyzer & analyzer, const ldml::TransformEntry * entry) {
    auto P = CreatePipeline(driver, Input<uint32_t>("inputFileDescriptor"));
    Scalar * const fileDescriptor = P.getInputScalar("inputFileDescriptor");
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    StreamSet * const U21 = U21_Of(P, BasisOf(P, ByteStream));
    RulePipelineBuilder builder(P, registry, analyzer);
    StreamSet * const Result = builder.transform(entry, U21);
    StreamSet * const OutputBytes = BytesOf(P, UTF8_Of(P, Result));
    P.CreateKernelCall<StdOutKernel>(OutputBytes);
    return P.compile();
}

//  A pipeline for a transform compiled from an ad hoc rules file (--rules-file),
//  given its already-computed analysis (see TransformAnalyzer::analyzeRulesFile).
static TransformFunctionType generateRulePipelineFromAnalysis(CPUDriver & driver, const ldml::TransformRegistry & registry,
                                                               TransformAnalyzer & analyzer, const TransformAnalysis & a) {
    auto P = CreatePipeline(driver, Input<uint32_t>("inputFileDescriptor"));
    Scalar * const fileDescriptor = P.getInputScalar("inputFileDescriptor");
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    StreamSet * const U21 = U21_Of(P, BasisOf(P, ByteStream));
    RulePipelineBuilder builder(P, registry, analyzer);
    StreamSet * const Result = builder.applyAnalysis(a, U21);
    StreamSet * const OutputBytes = BytesOf(P, UTF8_Of(P, Result));
    P.CreateKernelCall<StdOutKernel>(OutputBytes);
    return P.compile();
}

//  Applies a transform function to the input: the whole text, or with --lines,
//  each line as a text of its own (given through a pipe), followed by its line
//  break.
static void applyToInput(TransformFunctionType fn, int fd) {
    if (!Lines) {
        fn(fd);
        return;
    }
    std::string text;
    char buffer[1 << 16];
    for (ssize_t n; (n = read(fd, buffer, sizeof(buffer))) > 0; ) text.append(buffer, static_cast<size_t>(n));
    for (size_t pos = 0; pos < text.size(); ) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        int fds[2];
        if (pipe(fds) != 0) {
            llvm::errs() << "Error: cannot create a pipe.\n";
            exit(1);
        }
        std::thread writer([&] {
            for (size_t i = pos; i < end; ) {
                const ssize_t n = write(fds[1], text.data() + i, end - i);
                if (n <= 0) break;
                i += static_cast<size_t>(n);
            }
            close(fds[1]);
        });
        fn(fds[0]);
        writer.join();
        close(fds[0]);
        if (nl == std::string::npos) break;
        if (write(STDOUT_FILENO, "\n", 1) != 1) exit(1);
        pos = nl + 1;
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

    if (!RulesFile.empty()) {
        //  Only one positional argument is expected (the input file); if it was
        //  parsed as TransformName (the first positional), it is the input file.
        std::string inputFile = InputFile;
        if (!TransformName.empty()) {
            if (!InputFile.empty()) {
                llvm::errs() << "Error: --rules-file takes only an input file, not a transform name.\n";
                return 1;
            }
            inputFile = TransformName;
        }
        if (inputFile.empty() && !ShowPlan) {
            llvm::errs() << "Usage: " << argv[0] << " [options] --rules-file=<rules file> <input file>\n";
            return 1;
        }
        std::vector<ldml::Rule *> parsed;
        try {
            parsed = ldml::parseTransformRules({readFile(RulesFile)});
        } catch (const ldml::TransformRuleParseErrors & e) {
            for (const ldml::TransformRuleParseError & err : e.getErrors()) llvm::errs() << RulesFile << ": " << err.what() << "\n";
            return 1;
        } catch (const std::exception & e) {
            llvm::errs() << "Error: " << e.what() << "\n";
            return 1;
        }
        TransformAnalyzer analyzer(registry);
        const TransformAnalysis a = analyzer.analyzeRulesFile(parsed);
        if (ShowPlan) {
            printPlan("rules file " + RulesFile, a);
            return a.implementable ? 0 : 2;
        }
        if (!a.implementable) {
            llvm::errs() << "Rules file " << RulesFile << " is not implementable";
            if (!a.problems.empty()) {
                llvm::errs() << ": " << a.problems.size() << " problems (see --plan), e.g.\n  " << a.problems.front();
            }
            llvm::errs() << "\n";
            return 2;
        }
        const int fd = open(inputFile.c_str(), O_RDONLY);
        if (LLVM_UNLIKELY(fd == -1)) {
            llvm::errs() << "Error: cannot open " << inputFile << " for processing.\n";
            return 1;
        }
        CPUDriver driver("tconv");
        TransformFunctionType fn = generateRulePipelineFromAnalysis(driver, registry, analyzer, a);
        applyToInput(fn, fd);
        close(fd);
        return 0;
    }

    if (TransformName.empty() || (InputFile.empty() && !ShowPlan)) {
        llvm::errs() << "Usage: " << argv[0] << " [options] <transform name> <input file>\n"
                     << "       " << argv[0] << " [options] --rules-file=<rules file> <input file>\n"
                     << "       " << argv[0] << " [options] --list-transforms\n";
        return 1;
    }

    const ldml::TransformEntry * entry = registry.lookup(TransformName);
    if (entry == nullptr) {
        llvm::errs() << "Error: unknown transform " << TransformName << "\n";
        return 1;
    }
    if (ShowPlan) {
        TransformAnalyzer analyzer(registry);
        const TransformAnalysis & a = analyzer.analyze(entry);
        printPlan(entry->canonicalName + (entry->isBuiltIn() ? " (built-in)" : " (" + entry->file + ")"), a);
        return a.implementable ? 0 : 2;
    }
    auto impl = std::find_if(implementedTransforms.begin(), implementedTransforms.end(),
                             [&](const std::pair<std::string, Transform> & p) {return p.first == entry->canonicalName;});
    TransformAnalyzer analyzer(registry);
    if (impl == implementedTransforms.end() && !analyzer.analyze(entry).implementable) {
        llvm::errs() << "Transform " << TransformName << " (canonical name " << entry->canonicalName
                     << (entry->isBuiltIn() ? ", built-in" : ", defined in " + entry->file)
                     << ") is known but not yet implemented";
        const TransformAnalysis & a = analyzer.analyze(entry);
        if (!a.problems.empty()) {
            llvm::errs() << ": " << a.problems.size() << " problems (see --plan), e.g.\n  " << a.problems.front();
        }
        llvm::errs() << "\n";
        return 2;
    }

    const int fd = open(InputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << InputFile << " for processing.\n";
        return 1;
    }
    CPUDriver driver("tconv");
    //  Lowercasing needs the Final_Sigma context, and titlecasing word
    //  boundaries, provided by the rule pipeline.
    const bool direct = impl != implementedTransforms.end() && impl->second != Transform::Lower
                        && impl->second != Transform::FCD && impl->second != Transform::FCC
                        && impl->second != Transform::Title && impl->second != Transform::HexJava
                        && impl->second != Transform::HexUnicode
                        && impl->second != Transform::HexC && impl->second != Transform::HexXML && impl->second != Transform::HexXML10
                        && impl->second != Transform::HexPerl && impl->second != Transform::HexDecode
                        && impl->second != Transform::Remove;
    TransformFunctionType fn = direct ? generatePipeline(driver, impl->second)
                                      : generateRulePipeline(driver, registry, analyzer, entry);
    applyToInput(fn, fd);
    close(fd);
    return 0;
}

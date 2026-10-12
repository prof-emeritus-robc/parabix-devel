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
static cl::opt<bool> ShowPlan("plan", cl::desc("Report how the transform would be implemented and exit"),
                              cl::init(false), cl::cat(TconvOptions));
static cl::opt<bool> ShowWarnings("show-transform-warnings", cl::desc("Report problems found in the transform files"),
                                  cl::init(false), cl::cat(TconvOptions));

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

enum class Transform {NFD, NFKD, NFC, NFKC, Lower, Upper, Title, HexJava, HexUnicode, HexC, HexXML};

static const std::vector<std::pair<std::string, Transform>> implementedTransforms = {
    {"Any-NFD", Transform::NFD}, {"Any-NFKD", Transform::NFKD},
    {"Any-NFC", Transform::NFC}, {"Any-NFKC", Transform::NFKC},
    {"Any-Lower", Transform::Lower}, {"Any-Upper", Transform::Upper}, {"Any-Title", Transform::Title},
    {"Any-Hex", Transform::HexJava},{"Any-Hex/Java", Transform::HexJava},
    {"Any-Hex/Unicode", Transform::HexUnicode}, {"Any-Hex/C", Transform::HexC},
    {"Any-Hex/XML", Transform::HexXML},
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
        case Transform::Title:
        case Transform::HexJava:
        case Transform::HexUnicode:
        case Transform::HexC:
        case Transform::HexXML:
            llvm::report_fatal_error("tconv: Any-Title and Any-Hex (/Java, /Unicode, /C, /XML) are implemented by the rule pipeline");
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

TransformAnalysis TransformAnalyzer::analyzeRules(const std::vector<ldml::Rule *> & parsed, ldml::TransformDirection direction) {
    TransformAnalysis a;
    try {
        std::vector<ldml::Rule *> rules = direction == ldml::TransformDirection::Forward
                                          ? ldml::ExtractForwardRules(parsed) : ldml::ExtractReverseBackwardRules(parsed);
        rules = ldml::TrivialCaptureElimination(rules);
        rules = ldml::NullableCaptureElimination(rules);
        ldml::DisambiguationStats dstats;
        rules = ldml::DisambiguateOrder(rules, &dstats);
        a.plan = ldml::planTransform(rules);
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
                    llvm::outs() << "not single pass: two-way overlaps";
                } else if (overlaps.acyclicApartFromSelf) {
                    llvm::outs() << "not single pass: self-overlaps (otherwise single pass in "
                                 << std::max(overlaps.stagesApartFromSelf, 1u) << " stages)";
                } else {
                    llvm::outs() << "not single pass: cycle of " << overlaps.cycle.size() << " rules";
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
                      std::string sig;
                      for (unsigned k : counts) sig += "_" + std::to_string(k);
                      return sig;
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
    if (isa<CC>(re) || isa<PropertyExpression>(re) || isa<Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)) return true;
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
                if (o.out.empty()) return true;
                if (d > 1 && o.out.size() != 1) return true;
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
    const bool needCoverage = step.coverStringRules && changingMappings;
    for (const ldml::CharMapSubgroup & s : step.subgroups) {
        FusedContexts before, after;
        if (s.engineBefore && fusedContexts(s.engineBefore, true, before, true) && fusedContexts(s.engineAfter, false, after)
                && convertedContextMatters(before, outcomes)) {
            llvm::outs() << "    before context matched against the converted text: [" << s.contextKey << "]\n";
        }
    }
    for (const ldml::StringRule & sr : step.stringRules) {
        FusedContexts before, after;
        if (sr.engineBefore && fusedContexts(sr.engineBefore, true, before, true) && fusedContexts(sr.engineAfter, false, after)
                && convertedContextMatters(before, outcomes)) {
            const bool wholeGroupFits = step.stringRules.size() <= FusedMaxWholeGroupRules && step.subgroups.size() <= FusedMaxWholeGroupRules;
            if ((step.stringOrder.empty() && !needCoverage) || wholeGroupFits) {
                llvm::outs() << "    before context matched against the converted text: " << ldml::printRule(sr.rule) << "\n";
            } else {
                llvm::outs() << "    warning: the before context of " << ldml::printRule(sr.rule)
                             << " may match differently in the converted text\n";
            }
        }
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

struct FusedRulesConfig {
    std::vector<FusedStringRule> strings;
    std::vector<FusedSubgroup> subgroups;
    std::vector<FusedMatchSource> sources;
    //  Only the matches of the string rules (in the kernel), at their first
    //  positions (Firsts, a stream per rule), for SelectStringMatches.
    bool matchOnly = false;
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
            const unsigned n = r.text.size(), m = r.replacement.size();
            if (r.match < 0) {
                for (const FusedContext & a : r.after) lookahead = std::max<unsigned>(lookahead, n - 1 + a.size());
                maskLookahead = std::max<unsigned>(maskLookahead, n - 1);
            }
            if (!c.matchOnly) {
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
        if (!c.matchOnly) {
            changePositions = std::max<unsigned>(changePositions, in.changes.size());
            for (const auto & k : in.insertions) insertCounts.insert(k.first);
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
    explicit FusedSubgroupPlan(const FusedSubgroup & g) {
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
        if (r.match >= 0) continue;
        for (const UCD::UnicodeSet & t : r.text) add(t);
        addContext(r.before);
        addContext(r.after);
    }
    if (c.matchOnly) return sets;
    for (const FusedSubgroup & g : c.subgroups) {
        const FusedSubgroupPlan plan(g);
        if (g.match < 0) {
            add(plan.all);
            addContext(g.before);
            addContext(g.after);
        }
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
        if (!c.matchOnly) {
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
        std::string s = "FusedRulesCC" + std::to_string(DeletionMark) + "_" + std::to_string(SmallMappingLimit);
        s += c.matchOnly ? "M" : "C";
        if (c.rounds) {
            s += "R" + std::to_string(c.rounds);
            appendSet(s, c.replacementChars);
            if (c.groupCoverage) s += "G";
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
        for (const FusedStringRule & r : c.strings) {
            s += "|s";
            for (const UCD::UnicodeSet & t : r.text) appendSet(s, t);
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
    if (mConfig.matchOnly) {
        Var * const firsts = getOutputStreamVar("Firsts");
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
            if (mConfig.coverage) res.firsts = pb.createOr(res.firsts, first);
        }
        for (size_t g = 0; g < mConfig.subgroups.size(); g++) {
            const FusedSubgroupPlan plan(mConfig.subgroups[g]);
            PabloAST * const at = atOf[g];
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

    std::vector<PabloAST *> firstOf = starts, atOf = ats;
    //  In a correction kernel for a whole group, given the matches of the string
    //  rules and the positions where the rules of the subgroups match: the matches
    //  that apply (selected as in SelectStringMatches) and the positions where the
    //  subgroups apply (not covered by them, and within the mask).
    PabloAST * const odd = pb.createRepeat(1, pb.getInteger(0xAA, 8));
    auto settle = [&]() {
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
                for (unsigned j = 1; j < mConfig.strings[r].text.size(); j++) inside = pb.createOr(inside, pb.createAdvance(firstOf[r], j));
                first = pb.createOr(first, firstOf[r]);
            }
            PabloAST * uncovered = pb.createInFile(pb.createNot(pb.createAnd(inside, pb.createNot(first))));
            if (mask) uncovered = pb.createAnd(uncovered, mask);
            for (size_t g = 0; g < atOf.size(); g++) atOf[g] = pb.createAnd(atOf[g], uncovered);
        }
    };
    if (correction) {
        PabloAST * const firstPosition = beforeStart(1);
        const UCD::UnicodeSet replaced = mConfig.replacementChars - UCD::UnicodeSet(DeletionMark);
        //  The before contexts of the rules matched against the converted text
        //  given by results (with no results, the text itself).
        auto matchConverted = [&](const Results & res) {
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
        };
        Results none;
        none.changes.assign(mLayout.changePositions, std::vector<PabloAST *>(21, zeroes));
        //  Round 0: the before contexts matched against the text itself.
        matchConverted(none);
        settle();
        //  Each further round: the converted text from the results so far, and the
        //  before contexts matched against it.  (A rule whose before context sees
        //  the result of another such rule is right after as many rounds as such
        //  rules precede it in a chain.)
        for (unsigned round = 1; round <= mConfig.rounds; round++) {
            Results res = stateIn();
            addResults(res, firstOf, atOf);
            matchConverted(res);
            settle();
        }
    }

    Results res = stateIn();
    addResults(res, firstOf, atOf);
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
    StreamSet * maskedNormalization(const std::string & name, StreamSet * U21);
    StreamSet * conversionGroup(const ldml::TransformStep & step, StreamSet * U21);
    StreamSet * titlecase(StreamSet * U21);
    StreamSet * hexJava(StreamSet * U21);
    StreamSet * hexUnicode(StreamSet * U21);
    StreamSet * hexC(StreamSet * U21);
    StreamSet * hexXML(StreamSet * U21);
    StreamSet * wordBoundaries(StreamSet * U21);
    StreamSet * maskedStringOverrides(const std::vector<std::pair<UCD::property_t, StreamSet *>> & overrides, StreamSet * U21);
    StreamSet * matchPositions(re::RE * engineBefore, re::RE * engineAfter, const std::vector<UCD::UnicodeSet> & text,
                               StreamSet * U21, StreamSet ** spans = nullptr);
    StreamSet * classes(const std::vector<UCD::UnicodeSet> & sets, StreamSet * U21);
    StreamSet * combine(StreamSet * a, StreamSet * b, bool isOr);
    PipelineBuilder & mP;
    const ldml::TransformRegistry & mRegistry;
    TransformAnalyzer & mAnalyzer;
    NFD_PipelineBuilder mNFD;
    std::vector<StreamSet *> mMasks;
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
    if (name == "Any-NFD" || name == "Any-NFKD" || name == "Any-NFC" || name == "Any-NFKC") {
        return activeMask() ? maskedNormalization(name, U21) : normalization(name, U21);
    }
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
    llvm::report_fatal_error(llvm::StringRef("tconv: built-in transform " + name + " is not implemented"));
}

StreamSet * RulePipelineBuilder::normalization(const std::string & name, StreamSet * U21) {
    PipelineBuilder & P = mP;
    if (name == "Any-NFD") return mNFD.NFD_U21_Pipeline(U21);
    if (name == "Any-NFKD") return mNFD.NFKD_U21_Pipeline(U21);
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
StreamSet * RulePipelineBuilder::maskedNormalization(const std::string & name, StreamSet * U21) {
    PipelineBuilder & P = mP;
    StreamSet * const barrierChars = classes({UCD::UnicodeSet(BarrierMark)}, U21);
    StreamSet * const notBarrierChars = P.CreateStreamSet(1);
    Invert(P, barrierChars, notBarrierChars);
    StreamSet * const mask = combine(activeMask(), notBarrierChars, false);
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
    for (StreamSet *& m : mMasks) {
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
        {8, UCD::UnicodeSet(0x10000, 0xFFFFF)},    // &#x and 5 digits and ;
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
    //  ICU matches contexts possessively (see ldml::possessiveContext).  (The
    //  contexts are resolved to sets of characters by ldml::expandedContext,
    //  without inserted positions.)
    const Insertions none;
    if (engineBefore) {
        items.push_back(re::makeLookBehindAssertion(
            ldml::possessiveContext(ldml::expandedContext(engineBefore, none, DeletionMark), false)));
    }
    for (const UCD::UnicodeSet & chars : text) items.push_back(re::makeCC(chars, &cc::Unicode));
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
    if (!step.stringRules.empty()) return false;
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
StreamSet * RulePipelineBuilder::conversionGroup(const ldml::TransformStep & step, StreamSet * U21) {
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
        if (!texts.back().empty() && rule.replacement.size() < rule.text.size()) needDeletion = true;
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
        fusedMatch[r] = !texts[r].empty() && fusedContexts(rule.engineBefore, true, befores[r])
                   && fusedContexts(rule.engineAfter, false, afters[r]);
    }
    //  The rules whose before contexts the conversions of the group may make match
    //  differently in the converted text: matched against it by a correction
    //  kernel (FusedRules with rounds), after the other rules; or, if such string
    //  rules must be selected or other rules need their coverage, together with
    //  all the rules of the group (see wholeGroup below).
    const std::vector<ConversionOutcome> outcomes = conversionOutcomes(step);
    const bool needCoverage = step.coverStringRules &&
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
        std::vector<re::CC *> ccs;
        for (const UCD::UnicodeSet & set : sets) ccs.push_back(re::makeCC(set, &cc::Unicode));
        StreamSet * const classes = P.CreateStreamSet(ccs.size());
        P.CreateKernelCall<CharClassesKernel>(ccs, U21, classes);
        return classes;
    };
    //  The results of the rules, accumulated by the fused kernels (each ORing its
    //  results into those of the kernels before it).
    FusedState state;
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
    //  A group with string rules whose before contexts are matched against the
    //  converted text, and which must be selected (or whose coverage other rules
    //  need): all its rules in one correction kernel, as which matches apply, and
    //  where the single-character rules may apply, then depend on the converted
    //  text too.
    bool wholeGroup = false;
    if (correct && (!step.stringOrder.empty() || needCoverage)) {
        for (size_t r = 0; r < numStringRules; r++) wholeGroup |= convertible[r];
        if (numStringRules > FusedMaxWholeGroupRules || step.subgroups.size() > FusedMaxWholeGroupRules) wholeGroup = false;
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
            for (char32_t c : o.out) config.replacementChars.insert(c);
        }
        applyFused(std::move(config), nullptr);
    } else {
        std::vector<size_t> applying;               // the string rules, by their matches
        std::vector<FusedMatchSource> applied;      // and the source of their matches that apply
        if (!step.stringOrder.empty()) {
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
                config.coverage = needCoverage && step.stringOrder.empty();
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
                    FusedContexts starBefore, after;
                    if (correct && fusedContexts(subgroup.engineAfter, false, after) && fusedContexts(subgroup.engineBefore, true, starBefore, true)
                            && convertedContextMatters(starBefore, outcomes)) {
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
        if (!correction.strings.empty() || !correction.subgroups.empty()) {
            correction.rounds = ConvertedContextRounds;
            for (const ConversionOutcome & o : outcomes) {
                for (char32_t c : o.out) correction.replacementChars.insert(c);
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
    }
    return result;
}

StreamSet * RulePipelineBuilder::transform(const ldml::TransformEntry * entry, StreamSet * U21) {
    if (entry->isBuiltIn()) return builtIn(entry->canonicalName, U21);
    const TransformAnalysis & a = mAnalyzer.analyze(entry);
    assert (a.implementable);
    return applyAnalysis(a, U21);
}

StreamSet * RulePipelineBuilder::applyAnalysis(const TransformAnalysis & a, StreamSet * U21) {
    if (a.plan.filter) pushFilter(a.plan.filterSet, U21);
    for (const ldml::TransformStep & step : a.plan.steps) {
        if (step.kind == ldml::TransformStep::Kind::Transform) {
            if (step.filter) pushFilter(step.filterSet, U21);
            U21 = transform(mRegistry.lookup(step.transform.getText()), U21);
            if (step.filter) popFilter();
        } else {
            U21 = conversionGroup(step, U21);
        }
    }
    if (a.plan.filter) popFilter();
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
        fn(fd);
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
                        && impl->second != Transform::Title && impl->second != Transform::HexJava
                        && impl->second != Transform::HexUnicode
                        && impl->second != Transform::HexC && impl->second != Transform::HexXML;
    TransformFunctionType fn = direct ? generatePipeline(driver, impl->second)
                                      : generateRulePipeline(driver, registry, analyzer, entry);
    fn(fd);
    close(fd);
    return 0;
}

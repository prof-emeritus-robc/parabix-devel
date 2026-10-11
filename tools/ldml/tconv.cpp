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
#include <ldml/transform_registry.h>
#include <kernel/re/regexp_engine.h>
#include <re/adt/adt.h>
#include <re/cc/cc_kernel.h>
#include <ucd/utf/transchar.h>
#include <ldml/transform_plan.h>
#include <ldml/transform_rules_parser.h>
#include <ldml/transform_rules_printer.h>
#include <fstream>
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
static cl::opt<bool> ListTransforms("list-transforms", cl::desc("List the known transform names and exit"),
                                    cl::init(false), cl::cat(TconvOptions));
static cl::opt<bool> ShowPlan("plan", cl::desc("Report how the transform would be implemented and exit"),
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
            llvm::report_fatal_error("tconv: Any-Title is implemented by the rule pipeline");
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
private:
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
        std::vector<ldml::Rule *> rules = entry->direction == ldml::TransformDirection::Forward
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
    mInProgress.erase(name);
    a.implementable = a.problems.empty();
    return mResults.emplace(name, std::move(a)).first->second;
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

static void printPlan(const ldml::TransformEntry * entry, const TransformAnalysis & a) {
    llvm::outs() << entry->canonicalName << (entry->isBuiltIn() ? " (built-in)" : " (" + entry->file + ")")
                 << ": " << (a.implementable ? "implementable" : "not implementable") << "\n";
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
            if (s.before == nullptr) continue;
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
//  marked at their first positions (Firsts): a match of a rule applies only if
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
    SelectStringMatches(LLVMTypeSystemInterface & ts, const std::vector<Rule> & rules,
                        const std::vector<StreamSet *> & Firsts, const std::vector<StreamSet *> & Applied,
                        StreamSet * Inside, StreamSet * First)
    : PabloKernel(ts, "SelectStringMatches" + signature(rules) + (Inside ? "_spans" : ""),
                  [&] {
                      Bindings inputs;
                      for (size_t i = 0; i < Firsts.size(); i++) inputs.emplace_back("Firsts" + std::to_string(i), Firsts[i]);
                      return inputs;
                  }(),
                  [&] {
                      Bindings outputs;
                      for (size_t i = 0; i < Applied.size(); i++) outputs.emplace_back("Applied" + std::to_string(i), Applied[i]);
                      if (Inside) {
                          outputs.emplace_back("Inside", Inside);
                          outputs.emplace_back("First", First);
                      }
                      return outputs;
                  }()),
      mRules(rules), mSpans(Inside != nullptr) {}
protected:
    //  The rules, as a hash of their lengths, predecessors and self-overlaps (the
    //  description itself may be too long for a kernel name).
    static std::string signature(const std::vector<Rule> & rules) {
        std::string sig;
        for (const Rule & r : rules) {
            sig += "_" + std::to_string(r.length) + (r.selfOverlapping ? "s" : "");
            for (size_t p : r.predecessors) sig += "." + std::to_string(p);
        }
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
            PabloAST * matches = getInputStreamSet("Firsts" + std::to_string(i))[0];
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
            writeOutputStreamSet("Applied" + std::to_string(i), std::vector<PabloAST *>{matches});
        }
        if (mSpans) {
            writeOutputStreamSet("Inside", std::vector<PabloAST *>{inside});
            writeOutputStreamSet("First", std::vector<PabloAST *>{first});
        }
    }
private:
    const std::vector<Rule> mRules;
    const bool mSpans;
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

//  The mappings of at most this many characters change by the per-bit sets of
//  their targets (see xorSets); larger ones by TargetBits.
static constexpr unsigned SmallMappingLimit = 64;

//  The 21 bits to change, at the positions marked in At, to change the
//  character there (given by Basis) to the target codepoint, or, without Basis,
//  to change the deletion mark (at an inserted position) to the target.
class TargetBits : public pablo::PabloKernel {
public:
    TargetBits(LLVMTypeSystemInterface & ts, StreamSet * At, StreamSet * Basis, UCD::codepoint_t target, StreamSet * Bits)
    : PabloKernel(ts, "TargetBits" + std::to_string(target) + (Basis ? "_basis" : "_mark" + std::to_string(DeletionMark)),
                  [&] {
                      Bindings inputs{Binding{"At", At}};
                      if (Basis) inputs.emplace_back("Basis", Basis);
                      return inputs;
                  }(),
                  {Binding{"Bits", Bits}}),
      mTarget(target), mHasBasis(Basis != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const at = getInputStreamSet("At")[0];
        std::vector<PabloAST *> bits(21);
        std::vector<PabloAST *> basis;
        if (mHasBasis) basis = getInputStreamSet("Basis");
        for (unsigned b = 0; b < 21; b++) {
            const bool targetBit = (mTarget >> b) & 1;
            if (mHasBasis) {
                bits[b] = pb.createAnd(at, targetBit ? pb.createNot(basis[b]) : basis[b]);
            } else {
                bits[b] = (targetBit != ((DeletionMark >> b) & 1)) ? at : pb.createZeroes();
            }
        }
        writeOutputStreamSet("Bits", bits);
    }
private:
    const UCD::codepoint_t mTarget;
    const bool mHasBasis;
};

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

class RulePipelineBuilder {
public:
    RulePipelineBuilder(PipelineBuilder & P, const ldml::TransformRegistry & registry, TransformAnalyzer & analyzer)
    : mP(P), mRegistry(registry), mAnalyzer(analyzer), mNFD(P) {}
    // Apply the transform of a registry entry to U21 (an implementable transform).
    StreamSet * transform(const ldml::TransformEntry * entry, StreamSet * U21);
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
    StreamSet * builtIn(const std::string & name, StreamSet * U21);
    StreamSet * normalization(const std::string & name, StreamSet * U21);
    StreamSet * maskedNormalization(const std::string & name, StreamSet * U21);
    StreamSet * conversionGroup(const ldml::TransformStep & step, StreamSet * U21);
    StreamSet * titlecase(StreamSet * U21);
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

//  The bits to change, at the positions of characters, to change each character
//  of a map to its target (21 sets, one per bit).
static std::vector<UCD::UnicodeSet> xorSets(const std::map<UCD::codepoint_t, UCD::codepoint_t> & targets) {
    std::vector<UCD::UnicodeSet> sets(21);
    for (const auto & t : targets) {
        const UCD::codepoint_t diff = t.first ^ t.second;
        for (unsigned b = 0; b < 21; b++) {
            if ((diff >> b) & 1) sets[b].insert(t.first);
        }
    }
    return sets;
}

//  The bits to change, at the positions of characters, to change an inserted
//  position (holding the deletion mark) to the target of each character.
static std::vector<UCD::UnicodeSet> markXorSets(const std::map<UCD::codepoint_t, UCD::codepoint_t> & targets) {
    std::vector<UCD::UnicodeSet> sets(21);
    for (const auto & t : targets) {
        const UCD::codepoint_t diff = DeletionMark ^ t.second;
        for (unsigned b = 0; b < 21; b++) {
            if ((diff >> b) & 1) sets[b].insert(t.first);
        }
    }
    return sets;
}

static bool anyBits(const std::vector<UCD::UnicodeSet> & sets) {
    for (const UCD::UnicodeSet & s : sets) {
        if (!s.empty()) return true;
    }
    return false;
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
StreamSet * RulePipelineBuilder::conversionGroup(const ldml::TransformStep & step, StreamSet * U21) {
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
    auto inverted = [&](StreamSet * s) {
        StreamSet * const result = P.CreateStreamSet(1);
        Invert(P, s, result);
        return result;
    };

    //  Phase 1.  The positions of the last characters of the matches of a string
    //  rule (all of them, and those within the mask), and with spans, their
    //  spans.
    auto ruleMatches = [&](size_t r, StreamSet ** ruleSpans) -> std::pair<StreamSet *, StreamSet *> {
        const ldml::StringRule & rule = step.stringRules[r];
        const std::vector<UCD::UnicodeSet> & text = texts[r];
        StreamSet * const allMatches = matchPositions(rule.engineBefore, rule.engineAfter, text, U21, ruleSpans);
        StreamSet * matches = allMatches;
        //  Within a filter, the n characters of the text (at the last position and
        //  the n - 1 before it) must all be within the mask.
        if (mask) {
            for (size_t j = 0; j < text.size(); j++) {
                StreamSet * within = mask;
                if (j > 0) {
                    within = P.CreateStreamSet(1);
                    P.CreateKernelCall<ShiftForward>(mask, within, static_cast<unsigned>(j));
                }
                matches = combine(matches, within, false);
            }
        }
        SHOW_STREAM(matches);
        return {allMatches, matches};
    };
    //  The applied matches of each string rule, at their last characters; and the
    //  spans and first positions of the applied matches (for the coverage of the
    //  string rules).
    std::vector<StreamSet *> ruleEnds(step.stringRules.size(), nullptr);
    StreamSet * spans = nullptr;
    StreamSet * starts = nullptr;
    if (!step.stringOrder.empty()) {
        //  The matches of the rules, in the order to apply them, at their first
        //  positions (n - 1 before their last), from which those that apply are
        //  selected (see ldml::TransformStep::stringOrder).
        std::vector<size_t> rules;
        std::vector<size_t> index(step.stringRules.size(), SIZE_MAX);
        for (size_t r : step.stringOrder) {
            if (texts[r].empty()) continue;
            index[r] = rules.size();
            rules.push_back(r);
        }
        std::vector<StreamSet *> firsts, applied;
        std::vector<SelectStringMatches::Rule> select;
        for (size_t r : rules) {
            const size_t n = texts[r].size();
            StreamSet * const first = P.CreateStreamSet(1);
            P.CreateKernelCall<ShiftBack>(ruleMatches(r, nullptr).second, first, static_cast<unsigned>(n - 1));
            firsts.push_back(first);
            applied.push_back(P.CreateStreamSet(1));
            SelectStringMatches::Rule sr;
            sr.length = static_cast<unsigned>(n);
            sr.selfOverlapping = step.stringRules[r].selfOverlapping;
            for (size_t a : step.stringPredecessors[r]) {
                if (index[a] != SIZE_MAX) sr.predecessors.push_back(index[a]);
            }
            select.push_back(std::move(sr));
        }
        StreamSet * const inside = step.coverStringRules ? P.CreateStreamSet(1) : nullptr;
        StreamSet * const first = step.coverStringRules ? P.CreateStreamSet(1) : nullptr;
        P.CreateKernelCall<SelectStringMatches>(select, firsts, applied, inside, first);
        for (size_t i = 0; i < rules.size(); i++) {
            const size_t n = texts[rules[i]].size();
            StreamSet * const ends = P.CreateStreamSet(1);
            P.CreateKernelCall<ShiftForward>(applied[i], ends, static_cast<unsigned>(n - 1));
            SHOW_STREAM(ends);
            ruleEnds[rules[i]] = ends;
        }
        //  The spans of the matches that apply (the engine's spans, the union of
        //  those of all the matches, would include the others).
        if (step.coverStringRules) {
            spans = combine(inside, first, true);
            starts = first;
        }
    } else {
        for (size_t r = 0; r < step.stringRules.size(); r++) {
            if (texts[r].empty()) continue;
            const size_t n = texts[r].size();
            StreamSet * ruleSpans = nullptr;
            StreamSet * allMatches, * matches;
            std::tie(allMatches, matches) = ruleMatches(r, step.coverStringRules ? &ruleSpans : nullptr);
            if (step.coverStringRules) {
                //  Within a filter, the spans of the matches that do not apply
                //  (those extending beyond the mask) do not count: as the matches
                //  of the string rules do not overlap, these are the n positions up
                //  to their ends.
                if (mask) {
                    StreamSet * const rejected = combine(allMatches, inverted(matches), false);
                    StreamSet * rejectedSpans = rejected;
                    for (size_t j = 1; j < n; j++) {
                        StreamSet * const back = P.CreateStreamSet(1);
                        P.CreateKernelCall<ShiftBack>(rejected, back, static_cast<unsigned>(j));
                        rejectedSpans = combine(rejectedSpans, back, true);
                    }
                    ruleSpans = combine(ruleSpans, inverted(rejectedSpans), false);
                }
                spans = combine(spans, ruleSpans, true);
                StreamSet * const first = P.CreateStreamSet(1);
                P.CreateKernelCall<ShiftBack>(matches, first, static_cast<unsigned>(n - 1));
                starts = combine(starts, first, true);
            }
            ruleEnds[r] = matches;
        }
    }
    //  The positions where the single-character rules may apply: not within the
    //  applied matches of string rules (other than at their first positions), as
    //  in ICU, which does not apply rules within the text it has replaced, and
    //  within the mask.
    StreamSet * allowed = mask;
    if (spans && std::any_of(maps.begin(), maps.end(), [](const std::vector<ldml::CharMapping> & m) {return !m.empty();})) {
        SHOW_STREAM(spans);
        SHOW_STREAM(starts);
        StreamSet * const uncovered = P.CreateStreamSet(1);
        P.CreateKernelCall<UncoveredPositions>(spans, starts, uncovered);
        SHOW_STREAM(uncovered);
        allowed = combine(allowed, uncovered, false);
    }

    //  Phase 2.  The changes at the positions of the characters, for each
    //  position of the replacements, and the positions after which positions are
    //  to be inserted, by their number.
    std::vector<StreamSet *> changesAt;
    auto addChanges = [&](size_t i, StreamSet * bits) {
        if (changesAt.size() <= i) changesAt.resize(i + 1, nullptr);
        SHOW_BIXNUM(bits);
        changesAt[i] = combine(changesAt[i], bits, true);
    };
    std::map<unsigned, StreamSet *> inserting;
    auto addInsertion = [&](unsigned k, StreamSet * at) {
        StreamSet *& positions = inserting[k];
        positions = combine(positions, at, true);
    };
    //  The changes at the positions of At to a target: of the character there,
    //  or (for i > 0) of the deletion mark at a position to be inserted.
    auto addTargetChanges = [&](size_t i, StreamSet * At, UCD::codepoint_t target) {
        if (i > 0 && target == DeletionMark) return;
        StreamSet * const bits = P.CreateStreamSet(21);
        P.CreateKernelCall<TargetBits>(At, i == 0 ? U21 : nullptr, target, bits);
        addChanges(i, bits);
    };

    for (size_t g = 0; g < step.subgroups.size(); g++) {
        const ldml::CharMapSubgroup & subgroup = step.subgroups[g];
        const std::vector<ldml::CharMapping> & map = maps[g];
        if (map.empty()) continue;
        //  The positions where the rules of the subgroup apply (with contexts), if
        //  not all those of its characters.
        StreamSet * at = allowed;
        if (subgroup.engineBefore || subgroup.engineAfter) {
            UCD::UnicodeSet all;
            for (const ldml::CharMapping & m : map) all = all + m.chars;
            StreamSet * const matches = matchPositions(subgroup.engineBefore, subgroup.engineAfter, {all}, U21);
            SHOW_STREAM(matches);
            at = combine(matches, at, false);
        }
        //  Small sets of characters change by the per-bit sets of their targets;
        //  large ones by their targets, at their positions.
        size_t maxLength = 1;
        std::map<size_t, UCD::UnicodeSet> byLength;     // the characters by the length of their replacements
        for (const ldml::CharMapping & m : map) {
            maxLength = std::max(maxLength, m.replacement.size());
            if (m.replacement.size() > 1) byLength[m.replacement.size()] = byLength[m.replacement.size()] + m.chars;
        }
        for (size_t i = 0; i < maxLength; i++) {
            std::map<UCD::codepoint_t, UCD::codepoint_t> targets;
            for (const ldml::CharMapping & m : map) {
                if (i > 0 && m.replacement.size() <= i) continue;
                const UCD::codepoint_t target = m.replacement.empty() ? DeletionMark : static_cast<UCD::codepoint_t>(m.replacement[i]);
                if (m.chars.count() <= SmallMappingLimit) {
                    for (const auto & range : m.chars) {
                        for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) targets.emplace(cp, target);
                    }
                } else {
                    StreamSet * charsAt = classes({m.chars}, U21);
                    if (at) charsAt = combine(charsAt, at, false);
                    addTargetChanges(i, charsAt, target);
                }
            }
            //  Position i of the replacements: at the character, the bits that
            //  change it; at the positions to be inserted, those that change the
            //  deletion mark.
            const std::vector<UCD::UnicodeSet> sets = i == 0 ? xorSets(targets) : markXorSets(targets);
            if (anyBits(sets)) {
                StreamSet * bits = classes(sets, U21);
                if (at) {
                    StreamSet * const selected = P.CreateStreamSet(21);
                    ZeroByMask(P, at, bits, selected);
                    bits = selected;
                }
                addChanges(i, bits);
            }
        }
        //  The characters replaced by longer strings, after which positions are
        //  to be inserted.
        for (const auto & l : byLength) {
            StreamSet * positions = classes({l.second}, U21);
            if (at) positions = combine(positions, at, false);
            addInsertion(static_cast<unsigned>(l.first - 1), positions);
        }
    }
    for (size_t r = 0; r < step.stringRules.size(); r++) {
        StreamSet * const ends = ruleEnds[r];
        if (ends == nullptr) continue;
        const ldml::StringRule & rule = step.stringRules[r];
        const size_t n = texts[r].size(), m = rule.replacement.size();
        //  The characters of the text, at n - 1 - j positions before the last,
        //  change to the characters of the replacement (or the deletion mark).
        for (size_t j = 0; j < n; j++) {
            const UCD::codepoint_t target = j < m ? static_cast<UCD::codepoint_t>(rule.replacement[j]) : DeletionMark;
            StreamSet * charAt = ends;
            if (j + 1 < n) {
                charAt = P.CreateStreamSet(1);
                P.CreateKernelCall<ShiftBack>(ends, charAt, static_cast<unsigned>(n - 1 - j));
            }
            addTargetChanges(0, charAt, target);
        }
        //  The further characters of the replacement, at the positions to be
        //  inserted after the last character of the text.
        for (size_t j = n; j < m; j++) {
            addTargetChanges(j - n + 1, ends, static_cast<UCD::codepoint_t>(rule.replacement[j]));
        }
        if (m > n) addInsertion(static_cast<unsigned>(m - n), ends);
    }

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
    if (TransformName.empty() || (InputFile.empty() && !ShowPlan)) {
        llvm::errs() << "Usage: " << argv[0] << " [options] <transform name> <input file>\n"
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
        printPlan(entry, a);
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
                        && impl->second != Transform::Title;
    TransformFunctionType fn = direct ? generatePipeline(driver, impl->second)
                                      : generateRulePipeline(driver, registry, analyzer, entry);
    fn(fd);
    close(fd);
    return 0;
}

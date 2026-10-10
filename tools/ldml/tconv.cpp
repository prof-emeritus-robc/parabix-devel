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
        //  The characters of the texts of string rules, other than the last, must
        //  not be expanded for longer replacements (so that the characters of each
        //  text are consecutive).
        for (const ldml::TransformStep & step : a.plan.steps) {
            UCD::UnicodeSet expanded;
            for (const ldml::CharMapSubgroup & sg : step.subgroups) {
                for (const ldml::CharMapping & m : sg.mappings) if (m.replacement.size() > 1) expanded = expanded + m.chars;
            }
            for (const ldml::StringRule & sr : step.stringRules) {
                if (sr.replacement.size() > sr.text.size()) expanded = expanded + sr.text.back();
            }
            for (const ldml::StringRule & sr : step.stringRules) {
                for (size_t j = 0; j + 1 < sr.text.size(); j++) {
                    if (!(sr.text[j] & expanded).empty()) {
                        a.problems.push_back(ldml::printRule(sr.rule) + "  (a character of the text other than the last "
                                             "is expanded for a longer replacement; not yet supported)");
                        break;
                    }
                }
            }
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

//  Insert k positions after the characters of a set (each character keeps the
//  largest number of positions required for it).
static void addInsertions(Insertions & insertions, UCD::UnicodeSet chars, unsigned k) {
    for (const auto & e : insertions) {
        if (e.first >= k) chars = chars - e.second;
    }
    if (chars.empty()) return;
    for (auto & e : insertions) {
        if (e.first < k) e.second = e.second - chars;
    }
    insertions[k] = insertions[k] + chars;
    for (auto e = insertions.begin(); e != insertions.end(); ) {
        e = e->second.empty() ? insertions.erase(e) : std::next(e);
    }
}

//  The characters of a set by the number of positions inserted after them
//  (0 for the others).
static std::map<unsigned, UCD::UnicodeSet> byInsertions(const UCD::UnicodeSet & chars, const Insertions & insertions) {
    std::map<unsigned, UCD::UnicodeSet> result;
    UCD::UnicodeSet rest = chars;
    for (const auto & e : insertions) {
        const UCD::UnicodeSet followed = chars & e.second;
        if (followed.empty()) continue;
        result[e.first] = followed;
        rest = rest - followed;
    }
    if (!rest.empty()) result[0] = rest;
    return result;
}

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
                               unsigned marks, const Insertions & insertions, StreamSet * U21);
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
//  sets) with contexts, followed by the given number of inserted positions.
StreamSet * RulePipelineBuilder::matchPositions(re::RE * engineBefore, re::RE * engineAfter, const std::vector<UCD::UnicodeSet> & text,
                                                unsigned marks, const Insertions & insertions, StreamSet * U21) {
    std::vector<re::RE *> items;
    //  ICU matches contexts possessively (see ldml::possessiveContext).
    if (engineBefore) {
        items.push_back(re::makeLookBehindAssertion(
            ldml::possessiveContext(ldml::expandedContext(engineBefore, insertions, DeletionMark), false)));
    }
    for (const UCD::UnicodeSet & chars : text) items.push_back(re::makeCC(chars, &cc::Unicode));
    //  The lookahead begins after the positions inserted after the last character.
    if (engineAfter) {
        std::vector<re::RE *> ahead(marks, re::makeCC(DeletionMark, &cc::Unicode));
        ahead.push_back(ldml::possessiveContext(ldml::expandedContext(engineAfter, insertions, DeletionMark), true));
        items.push_back(re::makeLookAheadAssertion(re::makeSeq(ahead.begin(), ahead.end())));
    }
    re::RE * const pattern = re::makeSeq(items.begin(), items.end());
    //  The engine marks a match at an offset after its last character (e.g. 1
    //  for a pattern ending with a zero-width lookahead): shift the marks back.
    const PreparedRE prepared = prepareRE(pattern);
    StreamSet * matches = mP.CreateStreamSet(1);
    RE_PipelineBuilder engine(mP, RE_context{&cc::Unicode, U21});
    engine.matchSearchPipeline(prepared, matches);
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

StreamSet * RulePipelineBuilder::combine(StreamSet * a, StreamSet * b, bool isOr) {
    if (a == nullptr) return b;
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

//  A conversion group.  Characters to be replaced by longer strings are first
//  expanded: positions holding the deletion mark are inserted after each, for its
//  longest replacement (after the last character of a text, for the replacements
//  of texts longer than the text).  Contexts are matched with each character
//  followed by its inserted positions.  The rules then compute the bits to change,
//  at all positions at once (as no two rules apply at the same position, they are
//  combined by OR, and applied by XOR):
//    - a single-character rule changes the character to the first character of its
//      replacement (or to the deletion mark, for an empty replacement), and the
//      inserted positions after it to the further characters;
//    - a string rule, of a text of n characters replaced by m characters, changes
//      the characters of the text to those of the replacement, the characters
//      beyond the replacement (if m < n) to the deletion mark, and the inserted
//      positions after the text to the further characters (if m > n).
//  The positions holding the deletion mark are then deleted.
//
//  Within a filter, the rules apply only at the positions of the active mask: the
//  changes elsewhere are discarded, and a string rule applies only where all the
//  characters of its text are within the mask.  Positions are inserted after the
//  characters regardless of the mask (so that contexts, which match the whole
//  text, are matched as without a filter); those inserted after characters
//  outside the mask keep the deletion mark, and are deleted.
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
    Insertions insertions;
    bool needDeletion = false;
    for (const ldml::CharMapSubgroup & subgroup : step.subgroups) {
        maps.emplace_back();
        for (const ldml::CharMapping & m : subgroup.mappings) {
            const UCD::UnicodeSet chars = restricted(m.chars);
            if (chars.empty()) continue;
            maps.back().push_back(ldml::CharMapping{chars, m.replacement});
            if (m.replacement.size() > 1) addInsertions(insertions, chars, m.replacement.size() - 1);
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
        if (texts.back().empty()) continue;
        const size_t n = rule.text.size(), m = rule.replacement.size();
        if (m < n) needDeletion = true;
        if (m > n) addInsertions(insertions, texts.back().back(), m - n);
    }
    if (!insertions.empty()) needDeletion = true;

    if (!insertions.empty()) {
        std::vector<UCD::UnicodeSet> bixnum;
        for (const auto & ins : insertions) {
            for (unsigned bit = 0; (ins.first >> bit) != 0; bit++) {
                if (bixnum.size() <= bit) bixnum.resize(bit + 1);
                if ((ins.first >> bit) & 1) bixnum[bit] = bixnum[bit] + ins.second;
            }
        }
        StreamSet * const InsertBixNum = classes(bixnum, U21);
        SHOW_BIXNUM(InsertBixNum);
        StreamSet * const SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, InsertBixNum, SpreadMask, kernel::InsertPosition::After);
        SHOW_STREAM(SpreadMask);
        StreamSet * const Expanded = P.CreateStreamSet(21, 1);
        SpreadByMask(P, SpreadMask, U21, Expanded);
        StreamSet * const Marked = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<FillInsertedPositions>(SpreadMask, Expanded, Marked);
        SHOW_BIXNUM(Marked);
        U21 = Marked;
        spreadMasks(SpreadMask);
    }
    StreamSet * const mask = activeMask();

    StreamSet * changes = nullptr;
    auto shiftAndAdd = [&](StreamSet * bits, int shift) {
        if (shift != 0) {
            StreamSet * const shifted = P.CreateStreamSet(21);
            if (shift > 0) {
                P.CreateKernelCall<ShiftForward>(bits, shifted, shift);
            } else {
                P.CreateKernelCall<ShiftBack>(bits, shifted, -shift);
            }
            bits = shifted;
        }
        SHOW_BIXNUM(bits);
        changes = combine(changes, bits, true);
    };
    //  The changes of per-bit sets of characters, at the positions of matches.
    auto addChanges = [&](const std::vector<UCD::UnicodeSet> & sets, StreamSet * matches, int shift) {
        if (!anyBits(sets)) return;
        StreamSet * bits = classes(sets, U21);
        if (matches) {
            StreamSet * const selected = P.CreateStreamSet(21);
            ZeroByMask(P, matches, bits, selected);
            bits = selected;
        }
        shiftAndAdd(bits, shift);
    };
    //  The changes at the positions of At to a target: of the character there,
    //  or of the deletion mark at an inserted position.
    auto addTargetChanges = [&](StreamSet * At, UCD::codepoint_t target, bool ofCharacter, int shift) {
        if (!ofCharacter && target == DeletionMark) return;
        StreamSet * const bits = P.CreateStreamSet(21);
        P.CreateKernelCall<TargetBits>(At, ofCharacter ? U21 : nullptr, target, bits);
        shiftAndAdd(bits, shift);
    };

    for (size_t g = 0; g < step.subgroups.size(); g++) {
        const ldml::CharMapSubgroup & subgroup = step.subgroups[g];
        const std::vector<ldml::CharMapping> & map = maps[g];
        if (map.empty()) continue;
        //  The positions where the rules of the subgroup apply (with contexts).
        StreamSet * matches = nullptr;
        if (subgroup.engineBefore || subgroup.engineAfter) {
            UCD::UnicodeSet all;
            for (const ldml::CharMapping & m : map) all = all + m.chars;
            for (const auto & k : byInsertions(all, insertions)) {
                matches = combine(matches, matchPositions(subgroup.engineBefore, subgroup.engineAfter, {k.second},
                                                          k.first, insertions, U21), true);
            }
            SHOW_STREAM(matches);
        }
        //  Small sets of characters change by the per-bit sets of their targets;
        //  large ones by their targets, at their positions.
        size_t maxLength = 1;
        for (const ldml::CharMapping & m : map) maxLength = std::max(maxLength, m.replacement.size());
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
                    StreamSet * at = classes({m.chars}, U21);
                    if (matches) at = combine(at, matches, false);
                    addTargetChanges(at, target, i == 0, static_cast<int>(i));
                }
            }
            //  Position i of the replacements: at the character, the bits that
            //  change it; at the inserted positions, those that change the mark.
            addChanges(i == 0 ? xorSets(targets) : markXorSets(targets), matches, static_cast<int>(i));
        }
    }

    for (size_t r = 0; r < step.stringRules.size(); r++) {
        const ldml::StringRule & rule = step.stringRules[r];
        const std::vector<UCD::UnicodeSet> & text = texts[r];
        if (text.empty()) continue;
        const size_t n = text.size(), m = rule.replacement.size();
        //  The positions of the last characters of the matches.
        StreamSet * matches = nullptr;
        for (const auto & k : byInsertions(text.back(), insertions)) {
            std::vector<UCD::UnicodeSet> t = text;
            t.back() = k.second;
            matches = combine(matches, matchPositions(rule.engineBefore, rule.engineAfter, t, k.first, insertions, U21), true);
        }
        //  Within a filter, the n characters of the text (at the last position
        //  and the n - 1 before it, as only the last may be followed by inserted
        //  positions) must all be within the mask.
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
        SHOW_STREAM(matches);
        //  The characters of the text, at n - 1 - j positions before the last,
        //  change to the characters of the replacement (or the deletion mark).
        for (size_t j = 0; j < n; j++) {
            const UCD::codepoint_t target = j < m ? static_cast<UCD::codepoint_t>(rule.replacement[j]) : DeletionMark;
            StreamSet * at = matches;
            if (j + 1 < n) {
                at = P.CreateStreamSet(1);
                P.CreateKernelCall<ShiftBack>(matches, at, static_cast<unsigned>(n - 1 - j));
            }
            addTargetChanges(at, target, true, 0);
        }
        //  The further characters of the replacement, at the inserted positions.
        for (size_t j = n; j < m; j++) {
            addTargetChanges(matches, static_cast<UCD::codepoint_t>(rule.replacement[j]), false, static_cast<int>(j - n + 1));
        }
    }

    StreamSet * result = U21;
    if (changes && mask) {
        StreamSet * const masked = P.CreateStreamSet(21);
        ZeroByMask(P, mask, changes, masked);
        changes = masked;
    }
    if (changes) {
        result = P.CreateStreamSet(21);
        XorCombine(P, U21, changes, result);
        SHOW_BIXNUM(result);
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

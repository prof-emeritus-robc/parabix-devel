/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <string>
#include <vector>
#include <llvm/Support/Casting.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/normalization/normalization.h>
#include <re/adt/re_cc.h>
#include <re/cc/cc_compiler_target.h>
#include <ucd/algo/normalization.h>
#include <ucd/core/unicode_set.h>
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>

using namespace kernel;
using namespace pablo;

//  The leading and trailing canonical combining classes (lccc, tccc) of each
//  character are those of the first and last characters of its NFD.  They are
//  given as indexes of the ccc enumeration, whose order is that of the values
//  (as SortRuns relies on): the sets of the codepoints with each bit of the
//  index set, the lccc sets first.
static std::vector<UCD::UnicodeSet> FCD_ClassSets() {
    auto * cccObj = llvm::cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    const std::vector<UCD::UnicodeSet> & cccBasis = cccObj->GetEnumerationBasisSets();
    const unsigned bits = cccBasis.size();
    auto cccIndex = [&](UCD::codepoint_t cp) {
        unsigned idx = 0;
        for (unsigned i = 0; i < bits; i++) {
            if (cccBasis[i].contains(cp)) idx |= 1u << i;
        }
        return idx;
    };
    std::vector<UCD::UnicodeSet> add(2 * bits), remove(2 * bits);
    UCD::NFD_Engine NFD_engine(UCD::DecompositionOptions::NFD);
    auto * decompMappingObj = llvm::cast<UCD::StringPropertyObject>(UCD::getPropertyObject(UCD::dm));
    for (UCD::codepoint_t cp : decompMappingObj->GetExplicitCps()) {
        std::u32string decomp;
        NFD_engine.NFD_append1(decomp, cp);
        if (decomp.empty() || (decomp.size() == 1 && decomp[0] == cp)) continue;
        const unsigned own = cccIndex(cp);
        const unsigned lead = cccIndex(decomp.front());
        const unsigned trail = cccIndex(decomp.back());
        for (unsigned i = 0; i < bits; i++) {
            const unsigned bit = 1u << i;
            if ((lead & bit) != (own & bit)) ((lead & bit) ? add : remove)[i].insert(cp);
            if ((trail & bit) != (own & bit)) ((trail & bit) ? add : remove)[bits + i].insert(cp);
        }
    }
    //  Hangul syllables (ccc 0) decompose to conjoining jamo (ccc 0): no change.
    std::vector<UCD::UnicodeSet> sets(2 * bits);
    for (unsigned i = 0; i < 2 * bits; i++) {
        sets[i] = (cccBasis[i % bits] - remove[i]) + add[i];
    }
    return sets;
}

//  From the lccc and tccc of each character (U21-indexed), the positions where
//  the text fails the FCD test, and where a region to decompose may begin.
//    NZ: lccc != 0, the characters that are not FCD boundaries before them;
//    V: the violations, lccc != 0 and tccc of the previous character > lccc;
//    C: the possible region starts: the characters with lccc = 0, those after
//       a character with tccc <= 1 (ICU's boundaries after), and the first;
//    FirstV: the first violation after each position of C (a violation is never
//       in C, since tccc(v - 1) > lccc(v) >= 1).
//  Output: Index = C | FirstV, FirstV and NZ.
class FCD_Violations : public PabloKernel {
public:
    FCD_Violations(LLVMTypeSystemInterface & ts, StreamSet * Classes,
                   StreamSet * Index, StreamSet * FirstV, StreamSet * NonStarters)
    : PabloKernel(ts, "FCD_Violations" + std::to_string(Classes->getNumElements()),
                  {Binding{"Classes", Classes}},
                  {Binding{"Index", Index}, Binding{"FirstV", FirstV}, Binding{"NonStarters", NonStarters}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> classes = getInputStreamSet("Classes");
        const unsigned bits = classes.size() / 2;
        BixNum LCCC(classes.begin(), classes.begin() + bits);
        BixNum TCCC(classes.begin() + bits, classes.end());
        BixNumCompiler bnc(pb);
        PabloAST * const NZ = pb.createInFile(bnc.NEQ(LCCC, 0));
        BixNum TCCC_prev(bits);
        for (unsigned i = 0; i < bits; i++) TCCC_prev[i] = pb.createAdvance(TCCC[i], 1);
        PabloAST * const V = pb.createAnd(NZ, bnc.UGT(TCCC_prev, LCCC));
        //  tccc <= 1: the enumeration indexes of NR (0) and OV (1).
        PabloAST * const B = bnc.ULE(TCCC, 1);
        PabloAST * const SOT = pb.createNot(pb.createAdvance(pb.createOnes(), 1));
        PabloAST * const C = pb.createInFile(pb.createOr3(pb.createNot(NZ), pb.createAdvance(B, 1), SOT));
        PabloAST * const notC = pb.createNot(C);
        //  The violations after another one in the same span (without a C between).
        PabloAST * const laterV = pb.createMatchStar(pb.createAnd(pb.createAdvance(V, 1), notC), notC);
        PabloAST * const FirstV = pb.createAnd(V, pb.createNot(laterV));
        Var * const Index = getOutputStreamVar("Index");
        pb.createAssign(pb.createExtract(Index, pb.getInteger(0)), pb.createOr(C, FirstV));
        pb.createAssign(pb.createExtract(getOutputStreamVar("FirstV"), pb.getInteger(0)), FirstV);
        pb.createAssign(pb.createExtract(getOutputStreamVar("NonStarters"), pb.getInteger(0)), NZ);
    }
};

//  The regions to decompose: from each start, through the run of non-starters
//  that follows it (up to the next character with lccc = 0).
class FCD_Regions : public PabloKernel {
public:
    FCD_Regions(LLVMTypeSystemInterface & ts, StreamSet * Starts, StreamSet * NonStarters, StreamSet * Regions)
    : PabloKernel(ts, "FCD_Regions",
                  {Binding{"Starts", Starts}, Binding{"NonStarters", NonStarters}},
                  {Binding{"Regions", Regions}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const starts = getInputStreamSet("Starts")[0];
        PabloAST * const NZ = getInputStreamSet("NonStarters")[0];
        //  MatchStar also marks the position following each run: the next
        //  character with lccc = 0, which is not in the region.
        PabloAST * const run = pb.createMatchStar(pb.createAnd(pb.createAdvance(starts, 1), NZ), NZ);
        PabloAST * const after = pb.createAnd(run, NZ);
        pb.createAssign(pb.createExtract(getOutputStreamVar("Regions"), pb.getInteger(0)), pb.createOr(starts, after));
    }
};

void FCD_RegionMask(PipelineBuilder & P, StreamSet * U21, StreamSet * Regions) {
    std::vector<re::CC *> ccs;
    for (const UCD::UnicodeSet & s : FCD_ClassSets()) ccs.push_back(re::makeCC(s, &cc::Unicode));
    StreamSet * const Classes = P.CreateStreamSet(ccs.size());
    P.CreateKernelCall<CharClassesKernel>(ccs, U21, Classes);
    StreamSet * const Index = P.CreateStreamSet(1);
    StreamSet * const FirstV = P.CreateStreamSet(1);
    StreamSet * const NonStarters = P.CreateStreamSet(1);
    P.CreateKernelCall<FCD_Violations>(Classes, Index, FirstV, NonStarters);
    //  Each first violation moves back to the previous index position: the
    //  position of C beginning its span.
    StreamSet * const Starts = P.CreateStreamSet(1);
    P.CreateKernelCall<IndexedShiftBack>(Index, FirstV, Starts);
    P.CreateKernelCall<FCD_Regions>(Starts, NonStarters, Regions);
}

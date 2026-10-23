#include <kernel/re/regexp_engine.h>
#include <kernel/core/kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/core/streamset.h>
#include <kernel/io/source_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/scan/scanmatchgen.h>
#include <kernel/streamutils/sentinel.h>
#include <re/cc/cc_kernel.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/unicode/utf8gen.h>
#include <re/analysis/replacement_analysis.h>
#include <kernel/bitwise/bixlogic.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/streamutils/streams_merge.h>
#include <kernel/unicode/boundary_kernels.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/util/linebreak_kernel.h>
#include <pablo/pablo.h>
#include <re/adt/adt.h>
#include <re/alphabet/alphabet.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/analysis/re_analysis.h>
#include <re/analysis/validation.h>
#include <re/analysis/collect_ccs.h>
#include <re/transforms/re_reverse.h>
#include <kernel/streamutils/region_reversal.h>
#include <ucd/core/unicode_set.h>
#include <re/analysis/re_name_gather.h>
#include <re/analysis/re_inspector.h>
#include <re/analysis/nullable.h>
#include <re/analysis/capture-ref.h>
#include <re/printer/re_printer.h>
#include <re/toolchain/toolchain.h>
#include <re/unicode/regex_passes.h>
#include <re/transforms/to_utf8.h>
#include <re/transforms/re_multiplex.h>
#include <re/transforms/capture_removal.h>
#include <re/transforms/expand_permutes.h>
#include <re/transforms/name_lookaheads.h>
#include <re/transforms/reference_transform.h>
#include <re/transforms/remove_nullable.h>
#include <re/transforms/variable_alt_promotion.h>
#include <re/unicode/boundaries.h>
#include <re/unicode/resolve_properties.h>
#include <re/cc/cc_compiler.h>         // for CC_Compiler
#include <re/cc/cc_compiler_target.h>
#include <re/compile/re_compiler.h>
#include <re/transforms/name_intro.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>
#include <ucd/utf/utf_compiler.h>
#include <toolchain/toolchain.h>

using namespace re;
using namespace pablo;
using namespace kernel;
using namespace llvm;

RE_CompilerContext::RE_CompilerContext() : mCodeUnitAlphabet(nullptr), mCodeUnitStream(nullptr),
    mMultiplexed(nullptr), mMatchStarts(nullptr), mMatchFollows(nullptr), mLengthAlphabet(nullptr), mIndexStream(nullptr),
    mCombiningType(RE_CombiningType::None), mCombiningStream(nullptr) {}

void RE_CompilerContext::setCodeUnitContext(const cc::Alphabet * a, StreamSet * basis) {
    mCodeUnitAlphabet = a;
    mLengthAlphabet = a; // default
    mCodeUnitStream = basis;
    mMultiplexed = nullptr;
    addAlphabet(a, basis);
}

void RE_CompilerContext::setMultiplexedCodeUnitContext(const cc::MultiplexedAlphabet * mpx, StreamSet * basis) {
    mCodeUnitAlphabet = mpx->getSourceAlphabet();
    mLengthAlphabet = mCodeUnitAlphabet;
    mCodeUnitStream = basis;
    mMultiplexed = mpx;
    addAlphabet(mpx, basis);
}

void RE_CompilerContext::setMatchRegions(StreamSet * starts, StreamSet * follows) {
    mMatchStarts = starts;
    mMatchFollows = follows;
}

void RE_CompilerContext::setIndexingContext(const cc::Alphabet * a, StreamSet * s) {
    mLengthAlphabet = a;
    mIndexStream = s;
}

void RE_CompilerContext::addAlphabet(const cc::Alphabet * a, StreamSet * basis) {
    mAlphabets.emplace_back(a, basis);
}

void RE_CompilerContext::addExternal(std::string extName, ExternalStream s) {
    mExternals.emplace(extName, s);
}

void RE_CompilerContext::setCombiningStream(kernel::StreamSet * s, RE_CombiningType k) {
    mCombiningStream = s;
    mCombiningType = k;
}


namespace {
// The CCs of the source alphabet of a multiplexed alphabet translated to
// their codes, within the definitions of the names that are not externals
// (which are compiled from their definitions, see RE_Compiler).
struct CodeUnitMultiplexer final : public RE_Transformer {
    CodeUnitMultiplexer(const cc::MultiplexedAlphabet * mpx, const ExternalNameMap & externals)
    : RE_Transformer("CodeUnitMultiplexer"), mMultiplexed(mpx), mExternals(externals) {}
    RE * transformCC(CC * cc) override {
        if (cc->getAlphabet() == mMultiplexed->getSourceAlphabet()) return mMultiplexed->transformCC(cc);
        return cc;
    }
    RE * transformName(Name * n) override {
        RE * const defn = n->getDefinition();
        if ((defn == nullptr) || (mExternals.count(n->getFullName()) == 1)) return n;
        RE * const t = transform(defn);
        if (t == defn) return n;
        return n->hasNamespace() ? makeName(n->getNamespace(), n->getName(), t) : makeName(n->getName(), t);
    }
private:
    const cc::MultiplexedAlphabet * const mMultiplexed;
    const ExternalNameMap & mExternals;
};

RE * multiplexedCodeUnits(RE * re, const cc::MultiplexedAlphabet * mpx, const ExternalNameMap & externals) {
    if (mpx == nullptr) return re;
    return CodeUnitMultiplexer(mpx, externals).transformRE(re);
}
}

RE_Kernel::RE_Kernel(LLVMTypeSystemInterface & ts, RE_CompilerContext & ctxt, RE * re, StreamSet * results)
: RE_Kernel(ts, ctxt, Multiplexed{multiplexedCodeUnits(re, ctxt.mMultiplexed, ctxt.mExternals)}, results) {}

RE_Kernel::RE_Kernel(LLVMTypeSystemInterface & ts, RE_CompilerContext & ctxt, Multiplexed m, StreamSet * results)
: PabloKernel(ts, makeSignature(ctxt, m.re), makeInputBindings(ctxt, m.re), makeOutputBindings(ctxt, m.re, results), {}, {}),
mContext(ctxt), mRE(m.re) {
    addAttribute(InfrequentlyUsed());
}

std::string RE_Kernel::makeSignature(RE_CompilerContext & ctxt, RE * re) {
    std::string signature;
    llvm::raw_string_ostream sigstrm(signature);
    sigstrm << AnnotateWithREflags("RE");
    if (ctxt.mMatchStarts) {
        // Match start and follow streams to mark regions are normally expected.
        if (anyEndAnchor(re)) {
            // For end anchors, a lookahead attribute is added.
            sigstrm << "+R";
        }
    } else {
        sigstrm << "-R";
    }
    if (ctxt.mIndexStream) {
        sigstrm << "+X";
    }
    std::set<std::string> localExternals;
    std::set<std::string> localAlphabets;
    gatherExternals(re, localExternals, localAlphabets);
    for (auto & a : ctxt.mAlphabets) {
        auto alphaName = a.first->getName();
        if (localAlphabets.count(alphaName) == 1) {
            sigstrm << '!' << a.second->getNumElements() << 'x' << a.second->getFieldWidth();
        }
    }
    std::vector<std::string> externalList;
    for (const auto & e : ctxt.mExternals) {
        auto name = e.first;
        if (localExternals.count(name) == 1) {
            auto ext = e.second;
            if (ext.kind == ExternalStreamKind::ZeroWidth) {
                sigstrm << "+Z";
            } else if (ext.kind == ExternalStreamKind::FixedLength) {
                sigstrm << "+F";
                sigstrm << ext.lgthRange.second;
                if (ext.offset == 1) sigstrm << "o";
            } else if (ext.kind == ExternalStreamKind::StringClassRep) {
                sigstrm << "+C";
            } else if (ext.kind == ExternalStreamKind::StartIndexed) {
                sigstrm << "+S";
                sigstrm << ext.offset;
                if (ext.negated) sigstrm << "n";
            } else {
                sigstrm << "+E";
                sigstrm << ext.lgthRange.first << "-" << ext.lgthRange.second;
                if (ext.offset == 1) sigstrm << "o";
            }
            externalList.push_back(name);
        }
    }
    if (ctxt.mCombiningType == RE_CombiningType::Exclude) {
        sigstrm << "&~";
    } else if (ctxt.mCombiningType == RE_CombiningType::Include) {
        sigstrm << "|=";
    }
    std::string canon_string = Printer_RE::PrintRE(canonicalizeExternals(re, externalList));
    //llvm::errs() << "RE_Kernel: " << canon_string << "\n";
    sigstrm << ":" << Kernel::getStringHash(canon_string);
    sigstrm.flush();
    return signature;
}

unsigned round_up_to_blocksize(int lgth) {
    unsigned lookahead_blocks = (codegen::BlockSize - 1 + lgth)/codegen::BlockSize;
    return lookahead_blocks * codegen::BlockSize;
}

// TODO:  limit the alphabets and externals to those in the RE top-level
Bindings RE_Kernel::makeInputBindings(RE_CompilerContext & ctxt, RE * re) {
    std::set<std::string> localExternals;
    std::set<std::string> localAlphabets;
    gatherExternals(re, localExternals, localAlphabets);
    Bindings externalBindings;
    if (ctxt.mMatchStarts) {
        externalBindings.emplace_back("mStarts", ctxt.mMatchStarts);
        if (anyEndAnchor(re)) {
            externalBindings.emplace_back("mFollows", ctxt.mMatchFollows, FixedRate(), LookAhead(1));
        } else {
            externalBindings.emplace_back("mFollows", ctxt.mMatchFollows);
        }
    }
    if (ctxt.mIndexStream) {
        externalBindings.emplace_back("mIndexing", ctxt.mIndexStream);
    }
    for (auto & a : ctxt.mAlphabets) {
        auto alphaName = a.first->getName();
        //llvm::errs() << "alphaName: " << alphaName << " count = " << localAlphabets.count(alphaName) << "\n";
        if (localAlphabets.count(alphaName) == 1) {
            externalBindings.emplace_back(alphaName + "_basis", a.second);
        }
    }
    std::vector<std::string> externalList;
    for (const auto & e : ctxt.mExternals) {
        auto name = e.first;
        if (localExternals.count(name) == 1) {
            //llvm::errs() << "RE_Kernel - adding external: " << name << "\n";
            auto ext = e.second;
            if (ext.kind == StartIndexed) {
                unsigned blk_ahead = round_up_to_blocksize(ext.offset);
                externalBindings.emplace_back(name, ext.extStream, FixedRate(), LookAhead(blk_ahead));
            } else {
                externalBindings.emplace_back(name, ext.extStream);
            }
        }
    }
    if (ctxt.mCombiningType != RE_CombiningType::None) {
        externalBindings.emplace_back("toCombine", ctxt.mCombiningStream, FixedRate(), Add1());
    }
    return externalBindings;
}

Bindings RE_Kernel::makeOutputBindings(RE_CompilerContext & ctxt, RE * re, StreamSet * results) {
    if (grepOffset(re, ctxt.mIndexStream != nullptr) == 0) {
        return {Binding{"matches", results}};
    }
    return {Binding{"matches", results, FixedRate(), Add1()}};    
}

void RE_Kernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::set<std::string> localExternals;
    std::set<std::string> localAlphabets;
    gatherExternals(mRE, localExternals, localAlphabets);
    PabloAST * matchStarts = nullptr;
    PabloAST * matchFollows = nullptr;
    if (mContext.mMatchStarts) {
        matchStarts = pb.createExtract(getInputStreamVar("mStarts"), pb.getInteger(0));
        matchFollows = pb.createExtract(getInputStreamVar("mFollows"), pb.getInteger(0));
    }
    // With a multiplexed context, the code units are its codes (the CCs of mRE).
    const cc::Alphabet * const codeUnits = mContext.mMultiplexed ? mContext.mMultiplexed : mContext.mCodeUnitAlphabet;
    RE_Compiler re_compiler(getEntryScope(), matchStarts, matchFollows, codeUnits);
    for (auto & a : mContext.mAlphabets) {
        auto alphaName = a.first->getName();
        if (localAlphabets.count(alphaName) == 1) {
            auto basis = getInputStreamSet(alphaName + "_basis");
            re_compiler.addAlphabet(a.first, basis);
        }
    }
    if (mContext.mIndexStream) {
        PabloAST * idxStrm = pb.createExtract(getInputStreamVar("mIndexing"), pb.getInteger(0));
        re_compiler.setIndexing(&cc::Unicode, idxStrm);
    }
    for (auto & e : mContext.mExternals) {
        auto name = e.first;
        if (localExternals.count(name) == 1) {
            auto ext = e.second;
            if (ext.kind == ExternalStreamKind::StringClassRep) {
                std::vector<PabloAST *> fse = getInputStreamSet(name);
                re_compiler.addStringClassRep(name, fse[0], fse[1], fse[2]);
                continue;
            }
            PabloAST * extStrm = pb.createExtract(getInputStreamVar(name), pb.getInteger(0));
            unsigned offset = ext.offset;
            bool fromFirst = false;
            if (ext.kind == ExternalStreamKind::StartIndexed) {
                fromFirst = true;
            }
            re_compiler.addPrecompiled(name, RE_Compiler::ExternalStream(extStrm, offset, ext.lgthRange, fromFirst));
        }
    }
    Var * const final_matches = pb.createVar("final_matches", pb.createZeroes());
    RE_Compiler::Marker matches = re_compiler.compileRE(mRE);
    PabloAST * matchResult = matches.stream();
    //if (matches.offset() != mOffset) {
        //errs() << Printer_RE::PrintRE(mContext.mRE) <<"\n mOffset = " << mOffset << "\n";
        //report_fatal_error("matches.offset() != mOffset");
    //}
    pb.createAssign(final_matches, matchResult);
    Var * const output = pb.createExtract(getOutputStreamVar("matches"), pb.getInteger(0));

    PabloAST * value = nullptr;
    if (mContext.mCombiningType == RE_CombiningType::None) {
        value = final_matches;
    } else {
        PabloAST * toCombine = pb.createExtract(getInputStreamVar("toCombine"), pb.getInteger(0));
        if (mContext.mCombiningType == RE_CombiningType::Exclude) {
            value = pb.createAnd(toCombine, pb.createNot(final_matches), "toCombine");
        } else {
            value = pb.createOr(toCombine, final_matches, "toCombine");
        }
    }
    pb.createAssign(output, value);
}

// Checks a `length`-character (fixed) window at fixed `distance`: basis1
// advanced by distance is XOR'd against basis2 to give a single per-character
// mismatch signal (differ). A match of the whole length-character capture
// requires `length` consecutive 0 bits in differ; OR-ing `length - 1` shifted
// copies of differ collapses that run down to a single 0 bit at the position
// where the whole window matches.
PabloAST * matchDistanceCheck(PabloBuilder & b, unsigned distance, unsigned length, std::vector<PabloAST *> basis1, std::vector<PabloAST *> basis2) {
    PabloAST * differ = b.createZeroes();
    for (unsigned i = 0; i < basis1.size(); i++) {
        PabloAST * advanced = b.createAdvance(basis1[i], distance);
        differ = b.createOr(differ, b.createXor(advanced, basis2[i]));
    }
    PabloAST * match = b.createInFile(b.createNot(differ));
    for (unsigned j = 1; j < length; j++) {
        match = b.createAnd(match, b.createAdvance(match, 1), "dist_match_" + std::to_string(distance) + "_len_" + std::to_string(j+1));
    }
    return match;
}

void FixedDistanceMatchesKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    auto basis = getInputStreamSet("Basis");
    Var * match = pb.createVar("match", pb.createZeroes());
    if (mHasCheckStream) {
        auto ToCheck = getInputStreamSet("ToCheck")[0];
        auto it = pb.createScope();
        pb.createIf(ToCheck, it);
        PabloAST * m = matchDistanceCheck(it, mMatchDistance, mMatchLength, basis, basis);
        it.createAssign(match, it.createAnd(ToCheck, m));
    } else {
        pb.createAssign(match, matchDistanceCheck(pb, mMatchDistance, mMatchLength, basis, basis));
    }
    Var * const MatchVar = getOutputStreamVar("Matches");
    pb.createAssign(pb.createExtract(MatchVar, pb.getInteger(0)), match);
}

FixedDistanceMatchesKernel::FixedDistanceMatchesKernel (LLVMTypeSystemInterface & ts, unsigned distance, unsigned length, StreamSet * Basis, StreamSet * Matches, StreamSet * ToCheck)
: PabloKernel(ts, "Distance_" + std::to_string(distance) + "_len" + std::to_string(length) + "_Matches_" + std::to_string(Basis->getNumElements()) + "x1" + (ToCheck == nullptr ? "" : "_withCheck"),
// inputs
{Binding{"Basis", Basis}},
// output
{Binding{"Matches", Matches}}), mMatchDistance(distance), mMatchLength(length), mHasCheckStream(ToCheck != nullptr) {
    if (mHasCheckStream) {
        mInputStreamSets.push_back({"ToCheck", ToCheck});
    }
}

void CodePointMatchKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    UCD::PropertyObject * propObj = UCD::getPropertyObject(mProperty);
    if (UCD::CodePointPropertyObject * p = dyn_cast<UCD::CodePointPropertyObject>(propObj)) {
        const UCD::UnicodeSet & nullSet = p->GetNullSet();
        std::vector<UCD::UnicodeSet> & xfrm_ccs = p->GetBitTransformSets();
        UTF::UTF_Compiler unicodeCompiler(getInput(0), pb);
        std::vector<Var *> xfrm_vars;
        for (unsigned i = 0; i < xfrm_ccs.size(); i++) {
            xfrm_vars.push_back(pb.createVar("xfrm_cc_" + std::to_string(i), pb.createZeroes()));
        }
        Var * nullVar = nullptr;
        if (!nullSet.empty()) {
            xfrm_ccs.push_back(nullSet);
            nullVar = pb.createVar("null_set", pb.createZeroes());
            xfrm_vars.push_back(nullVar);
        }
        unicodeCompiler.compile(xfrm_vars, xfrm_ccs);
        std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        std::vector<PabloAST *> transformed(basis.size());
        for (unsigned i = 0; i < basis.size(); i++) {
            if (i < xfrm_vars.size()) {
                transformed[i] = pb.createXor(xfrm_vars[i], basis[i]);
            } else {
                transformed[i] = basis[i];
            }
        }
        PabloAST * match;
        bool involution = ((mProperty == UCD::bpb) || (mProperty == UCD::bmg));
        if (involution) {
            match = matchDistanceCheck(pb, mMatchDistance, mMatchLength, transformed, basis);
        } else {
            match = matchDistanceCheck(pb, mMatchDistance, mMatchLength, transformed, transformed);
        }
        if (!nullSet.empty()) {
            match = pb.createAnd(match, pb.createNot(nullVar));
        }
        PabloAST * matches = pb.createInFile(match);
        Var * const MatchVar = getOutputStreamVar("Matches");
        pb.createAssign(pb.createExtract(MatchVar, pb.getInteger(0)), matches);
    } else {
        llvm::report_fatal_error("Expecting codepoint property");
    }
}

CodePointMatchKernel::CodePointMatchKernel (LLVMTypeSystemInterface & ts, UCD::property_t prop, unsigned distance, unsigned length, StreamSet * Basis, StreamSet * Matches)
: PabloKernel(ts, getPropertyEnumName(prop) + "_dist_" + std::to_string(distance) + "_len" + std::to_string(length) + "_Matches_" + std::to_string(Basis->getNumElements()) + "x1" + UTF::kernelAnnotation(),
// inputs
{Binding{"Basis", Basis}},
// output
{Binding{"Matches", Matches}}),
    mMatchDistance(distance),
    mMatchLength(length),
    mProperty(prop) {
}

FixedMatchSpansKernel::FixedMatchSpansKernel(LLVMTypeSystemInterface & ts, unsigned length, unsigned offset, StreamSet * MatchMarks, StreamSet * MatchSpans)
: PabloKernel(ts, "FixedMatchSpansKernel" + std::to_string(MatchMarks->getNumElements()) + "x1_by" + std::to_string(length) + '@' + std::to_string(offset),
{Binding{"MatchMarks", MatchMarks, FixedRate(1), LookAhead(round_up_to_blocksize(length))}}, {Binding{"MatchSpans", MatchSpans}}),
mMatchLength(length), mOffset(offset) {
}

void FixedMatchSpansKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * marks = pb.createExtract(getInputStreamVar("MatchMarks"), pb.getInteger(0));
    Var * matchSpansVar = getOutputStreamVar("MatchSpans");
    // (Named for the illustrator: "fms<length>_...".)
    const std::string tag = "fms" + std::to_string(mMatchLength) + "_";
    marks = pb.createAnd(marks, pb.createOnes(), tag + "marks");
    // starts of all the matches
    PabloAST * starts = pb.createLookahead(marks, mMatchLength + mOffset - 1, tag + "starts");
    // now find all consecutive positions within mMatchLength of any start.
    unsigned consecutiveCount = 1;
    PabloAST * consecutive = starts;
    for (unsigned i = 1; i <= mMatchLength/2; i *= 2) {
        consecutiveCount += i;
        consecutive = pb.createOr(consecutive,
                                  pb.createAdvance(consecutive, i),
                                  tag + "consecutive" + std::to_string(consecutiveCount));
    }
    if (consecutiveCount < mMatchLength) {
        consecutive = pb.createOr(consecutive,
                                  pb.createAdvance(consecutive, mMatchLength - consecutiveCount),
                                  tag + "consecutive" + std::to_string(mMatchLength));
    }
    pb.createAssign(pb.createExtract(matchSpansVar, 0), consecutive);
}

void LongestSpan::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * pfxStrm = getInputStreamSet("pfxStrm")[0];
    PabloAST * endBack = getInputStreamSet("endBack")[0];
    PabloAST * matchEnd = getInputStreamSet("matchEnd")[0];
    PabloAST * pfxStart = pb.createAnd(pb.createLookahead(pfxStrm, mPfxOffset), pb.createLookahead(endBack, mPfxOffset));
    PabloAST * longestEnd = pb.createAnd(matchEnd, pb.createNot(endBack));
    PabloAST * spans = nullptr;
    if (mEndOffset > 0) {
        spans = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {pfxStart, longestEnd});
    } else {
        spans = pb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, {pfxStart, longestEnd});
    }
    writeOutputStreamSet("spans", std::vector<PabloAST*>{spans});
}

LongestSpan::LongestSpan (LLVMTypeSystemInterface & ts, unsigned pfxOffset, unsigned endOffset, 
    StreamSet * pfxStrm, StreamSet * endBack, StreamSet * matchEnd, StreamSet * spans)
: PabloKernel(ts, "LongestSpan_" + std::to_string(pfxOffset) + ":" + std::to_string(endOffset),
// inputs
{Binding{"pfxStrm", pfxStrm, FixedRate(1), LookAhead(pfxOffset)},
 Binding{"endBack", endBack, FixedRate(1), LookAhead(pfxOffset)},
 Binding{"matchEnd", matchEnd}},
// output
{Binding{"spans", spans}}), mPfxOffset(pfxOffset),  mEndOffset(endOffset) {

}

static Bindings starLookaheadInputs(StreamSet * B, StreamSet * breaks, unsigned lookahead) {
    Bindings inputs;
    inputs.emplace_back("B", B, FixedRate(1), LookAhead(lookahead));
    if (breaks) {
        inputs.emplace_back("breaks", breaks, FixedRate(1), LookAhead(lookahead));
    }
    return inputs;
}

StarLookaheadIndex::StarLookaheadIndex(LLVMTypeSystemInterface & ts, StreamSet * B, StreamSet * breaks, StreamSet * index)
: PabloKernel(ts, std::string("StarLookaheadIndex") + (breaks ? "_br" : ""),
              starLookaheadInputs(B, breaks, 0), {Binding{"index", index}}),
  mHasBreaks(breaks != nullptr) {
}

void StarLookaheadIndex::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * Bp = getInputStreamSet("B")[0];
    if (mHasBreaks) {
        Bp = pb.createAnd(Bp, pb.createNot(getInputStreamSet("breaks")[0]));
    }
    PabloAST * runStarts = pb.createAnd(Bp, pb.createNot(pb.createAdvance(Bp, 1)));
    PabloAST * index = pb.createOr(pb.createNot(Bp), runStarts);
    writeOutputStreamSet("index", std::vector<PabloAST *>{index});
}

static unsigned maxStringLength(const std::vector<std::vector<re::CC *>> & strings) {
    size_t L = 1;
    for (const auto & str : strings) L = std::max(L, str.size());
    return static_cast<unsigned>(L);
}

static std::string stringClassSignature(const std::vector<std::vector<re::CC *>> & strings, StreamSet * basis) {
    std::string sig;
    raw_string_ostream out(sig);
    out << basis->getNumElements();
    for (const auto & str : strings) {
        out << "|";
        for (re::CC * cc : str) out << cc->canonicalName() << ";";
    }
    out.flush();
    return sig;
}

StringClassKernel::StringClassKernel(LLVMTypeSystemInterface & ts, std::vector<std::vector<re::CC *>> strings,
                                     StreamSet * basis, StreamSet * fillStartsEnds, StreamSet * follows)
: PabloKernel(ts, "StringClass_" + Kernel::getStringHash(stringClassSignature(strings, basis) + (follows ? "_f" : "")),
              [&] {
                  const unsigned la = maxStringLength(strings) - 1;
                  Bindings inputs;
                  inputs.emplace_back("basis", basis, FixedRate(1), LookAhead(la));
                  if (follows) inputs.emplace_back("follows", follows, FixedRate(1), LookAhead(la));
                  return inputs;
              }(),
              {Binding{"fillStartsEnds", fillStartsEnds}}),
  mStrings(std::move(strings)),
  mHasFollows(follows != nullptr),
  mSignature(stringClassSignature(mStrings, basis) + (follows ? "_f" : "")) {
}

void StringClassKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const std::vector<PabloAST *> basis = getInputStreamSet("basis");
    const unsigned L = maxStringLength(mStrings);
    // The basis looked ahead by each offset, and a character class compiler for each.
    std::vector<std::unique_ptr<cc::Parabix_CC_Compiler>> compilers;
    for (unsigned v = 0; v < L; ++v) {
        std::vector<PabloAST *> ahead;
        for (PabloAST * b : basis) ahead.push_back(v == 0 ? b : pb.createLookahead(b, v));
        compilers.emplace_back(std::make_unique<cc::Parabix_CC_Compiler>(ahead));
    }
    // The positions with no region follow within the next v + 1 positions.
    std::vector<PabloAST *> followFree;
    if (mHasFollows) {
        PabloAST * const follows = getInputStreamSet("follows")[0];
        PabloAST * any = follows;
        followFree.push_back(pb.createNot(any));
        for (unsigned v = 1; v < L; ++v) {
            any = pb.createOr(any, pb.createLookahead(follows, v));
            followFree.push_back(pb.createNot(any));
        }
    }
    // The start positions of the strings, by length.
    std::map<unsigned, PabloAST *> startsByLength;
    PabloAST * allStarts = pb.createZeroes();
    for (const auto & str : mStrings) {
        PabloAST * start = pb.createInFile(pb.createOnes());
        for (unsigned k = 0; k < str.size(); ++k) {
            start = pb.createAnd(start, compilers[k]->compileCC(str[k], pb));
        }
        if (mHasFollows) {
            start = pb.createAnd(start, followFree[str.size() - 1]);
        }
        const unsigned M = static_cast<unsigned>(str.size());
        auto f = startsByLength.find(M);
        startsByLength[M] = (f == startsByLength.end()) ? start : pb.createOr(f->second, start);
        allStarts = pb.createOr(allStarts, start);
    }
    // Fill the M positions of each occurrence of a string of length M; the
    // last of them is the end of the occurrence.
    PabloAST * fill = pb.createZeroes();
    PabloAST * ends = pb.createZeroes();
    for (const auto & m : startsByLength) {
        PabloAST * shifted = m.second;
        PabloAST * filled = shifted;
        for (unsigned i = 1; i < m.first; ++i) {
            shifted = pb.createAdvance(shifted, 1);
            filled = pb.createOr(filled, shifted);
        }
        fill = pb.createOr(fill, filled);
        ends = pb.createOr(ends, shifted);
    }
    writeOutputStreamSet("fillStartsEnds", std::vector<PabloAST *>{fill, allStarts, ends});
}

StringClassStarIndex::StringClassStarIndex(LLVMTypeSystemInterface & ts, StreamSet * fillStartsEnds, StreamSet * H,
                                           StreamSet * breaks, StreamSet * index, StreamSet * good)
: PabloKernel(ts, std::string("StringClassStarIndex") + (breaks ? "_br" : ""),
              [&] {
                  Bindings inputs;
                  inputs.emplace_back("fillStartsEnds", fillStartsEnds);
                  inputs.emplace_back("H", H);
                  if (breaks) inputs.emplace_back("breaks", breaks);
                  return inputs;
              }(),
              {Binding{"index", index}, Binding{"good", good}}),
  mHasBreaks(breaks != nullptr) {
}

void StringClassStarIndex::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const std::vector<PabloAST *> fse = getInputStreamSet("fillStartsEnds");
    PabloAST * const H = getInputStreamSet("H")[0];
    PabloAST * good = pb.createAnd(H, pb.createAdvance(fse[2], 1));
    PabloAST * fill = fse[0];
    if (mHasBreaks) {
        fill = pb.createAnd(fill, pb.createNot(getInputStreamSet("breaks")[0]));
    }
    // The first position of each run of fill is an index position (so that a
    // run at the beginning of the text has one).
    PabloAST * runStarts = pb.createAnd(fill, pb.createNot(pb.createAdvance(fill, 1)));
    PabloAST * index = pb.createOr(pb.createOr(good, pb.createNot(fill)), runStarts);
    writeOutputStreamSet("index", std::vector<PabloAST *>{index});
    writeOutputStreamSet("good", std::vector<PabloAST *>{good});
}

StringClassStarSpans::StringClassStarSpans(LLVMTypeSystemInterface & ts, unsigned lb, StreamSet * fillStartsEnds,
                                           StreamSet * H, StreamSet * index, StreamSet * goodNext, StreamSet * result)
: PabloKernel(ts, "StringClassStarSpans" + std::to_string(lb),
              {Binding{"fillStartsEnds", fillStartsEnds}, Binding{"H", H},
               Binding{"index", index}, Binding{"goodNext", goodNext}},
              {Binding{"result", result}}),
  mLB(lb) {
}

void StringClassStarSpans::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const std::vector<PabloAST *> fse = getInputStreamSet("fillStartsEnds");
    PabloAST * const H = getInputStreamSet("H")[0];
    PabloAST * const index = getInputStreamSet("index")[0];
    PabloAST * const goodNext = getInputStreamSet("goodNext")[0];
    // Each position whose next index position (strictly after it) is good:
    // an index position marked by goodNext, and the positions after it up
    // to the next index position.
    PabloAST * const notIndex = pb.createNot(index);
    PabloAST * following = pb.createMatchStar(pb.createAdvance(goodNext, 1), notIndex);
    PabloAST * spans = pb.createOr(goodNext, pb.createAnd(following, notIndex));
    PabloAST * result = pb.createAnd(spans, fse[1]);
    if (mLB == 0) {
        result = pb.createOr(result, H);
    }
    writeOutputStreamSet("result", std::vector<PabloAST *>{result});
}

static std::string chainAssertionEndName(const std::vector<bool> & negated, bool follows, bool rest) {
    std::string name = "ChainAssertionEnd_";
    for (bool n : negated) name += n ? 'n' : 'p';
    return name + (follows ? "_f" : "") + (rest ? "_h" : "");
}

EndOfTextMark::EndOfTextMark(LLVMTypeSystemInterface & ts, StreamSet * source, StreamSet * mark)
: PabloKernel(ts, "EndOfTextMark" + std::to_string(source->getNumElements()) + "x" + std::to_string(source->getFieldWidth()),
              {Binding{"source", source}},
              {Binding{"mark", mark, FixedRate(), Add1()}}) {
}

void EndOfTextMark::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const advOnes = pb.createAdvance(pb.createOnes(), 1);
    writeOutputStreamSet("mark", std::vector<PabloAST *>{pb.createAtEOF(advOnes)});
}

ExtendByOne::ExtendByOne(LLVMTypeSystemInterface & ts, StreamSet * source, StreamSet * extended)
: PabloKernel(ts, "ExtendByOne" + std::to_string(source->getNumElements()) + "x1",
              {Binding{"source", source}},
              {Binding{"extended", extended, FixedRate(), Add1()}}) {
}

void ExtendByOne::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const inText = pb.createNot(pb.createAtEOF(pb.createOnes()));
    std::vector<PabloAST *> extended;
    for (PabloAST * strm : getInputStreamSet("source")) extended.push_back(pb.createAnd(strm, inText));
    writeOutputStreamSet("extended", extended);
}

StartOfTextMark::StartOfTextMark(LLVMTypeSystemInterface & ts, StreamSet * source, StreamSet * mark)
: PabloKernel(ts, "StartOfTextMark" + std::to_string(source->getNumElements()) + "x" + std::to_string(source->getFieldWidth()),
              {Binding{"source", source}},
              {Binding{"mark", mark}}) {
}

void StartOfTextMark::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    writeOutputStreamSet("mark", std::vector<PabloAST *>{pb.createNot(pb.createAdvance(pb.createOnes(), 1))});
}

ChainAssertionEnd::ChainAssertionEnd(LLVMTypeSystemInterface & ts, std::vector<StreamSet *> classes,
                                     std::vector<bool> negated, StreamSet * follows, StreamSet * Hrest, StreamSet * H)
: PabloKernel(ts, chainAssertionEndName(negated, follows != nullptr, Hrest != nullptr),
              [&] {
                  Bindings inputs;
                  for (unsigned i = 0; i < classes.size(); ++i) {
                      inputs.emplace_back("Y" + std::to_string(i), classes[i]);
                  }
                  if (follows) inputs.emplace_back("follows", follows);
                  if (Hrest) inputs.emplace_back("Hrest", Hrest);
                  return inputs;
              }(),
              {Binding{"H", H}}),
  mNegated(std::move(negated)), mHasFollows(follows != nullptr), mHasRest(Hrest != nullptr) {
}

void ChainAssertionEnd::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * H = pb.createOnes();
    bool allNegated = true;
    for (unsigned i = 0; i < mNegated.size(); ++i) {
        PabloAST * Y = getInputStreamSet("Y" + std::to_string(i))[0];
        H = pb.createAnd(H, mNegated[i] ? pb.createNot(Y) : Y);
        allNegated &= mNegated[i];
    }
    if (mHasFollows) {
        PabloAST * const follows = getInputStreamSet("follows")[0];
        H = allNegated ? pb.createOr(H, follows) : pb.createAnd(H, pb.createNot(follows));
    }
    if (mHasRest) {
        //  Within the chain: the rest must also hold at the position.
        H = pb.createAnd(H, getInputStreamSet("Hrest")[0]);
    }
    writeOutputStreamSet("H", std::vector<PabloAST *>{H});
}

static std::string chainCoverageName(const std::vector<ChainCoverage::Step> & steps,
                                     const std::vector<ChainCoverage::Final> & finals, bool follows) {
    std::string name = "ChainCoverage_";
    for (const auto & st : steps) {
        name += st.kind + std::to_string(st.n) + (st.Hnext ? "h" : "") + "_";
    }
    if (!finals.empty()) {
        name += "final";
        for (const auto & f : finals) name += f.kind;
    }
    return name + (follows ? "_f" : "");
}

ChainCoverage::ChainCoverage(LLVMTypeSystemInterface & ts, StreamSet * starts, std::vector<Step> steps,
                             std::vector<Final> finals, StreamSet * follows, StreamSet * coverage)
: PabloKernel(ts, chainCoverageName(steps, finals, follows != nullptr),
              [&] {
                  Bindings inputs;
                  inputs.emplace_back("starts", starts);
                  for (unsigned i = 0; i < steps.size(); ++i) {
                      const std::string k = std::to_string(i);
                      const Step & st = steps[i];
                      if (st.Hnext) inputs.emplace_back("H" + k, st.Hnext);
                      if (st.kind == 's') inputs.emplace_back("cls" + k, st.cls);
                      if (st.kind == 'c') {
                          inputs.emplace_back("fse" + k, st.fse);
                          inputs.emplace_back("index" + k, st.index);
                          inputs.emplace_back("goodNext" + k, st.goodNext);
                      }
                  }
                  for (unsigned i = 0; i < finals.size(); ++i) {
                      inputs.emplace_back("final" + std::to_string(i), finals[i].strm);
                  }
                  if (follows) inputs.emplace_back("follows", follows);
                  return inputs;
              }(),
              {Binding{"coverage", coverage}}),
  mSteps(std::move(steps)), mFinals(std::move(finals)), mHasFollows(follows != nullptr) {
}

void ChainCoverage::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const notFollow = mHasFollows ? pb.createNot(getInputStreamSet("follows")[0]) : pb.createOnes();
    PabloAST * E = getInputStreamSet("starts")[0];    // the entries of the current segment
    PabloAST * cover = pb.createZeroes();
    for (unsigned i = 0; i < mSteps.size(); ++i) {
        const std::string k = std::to_string(i);
        const Step & st = mSteps[i];
        PabloAST * const H = st.Hnext ? getInputStreamSet("H" + k)[0] : nullptr;
        if (st.kind == 'f') {
            PabloAST * shifted = E;
            for (unsigned j = 0; j < st.n; ++j) {
                cover = pb.createOr(cover, shifted);
                shifted = pb.createAdvance(shifted, 1);
            }
            E = H ? pb.createAnd(shifted, H) : shifted;
        } else if (st.kind == 's') {
            PabloAST * const B = pb.createAnd(getInputStreamSet("cls" + k)[0], notFollow);
            PabloAST * const reach = pb.createMatchStar(E, B);
            cover = pb.createOr(cover, pb.createAnd(reach, B));
            E = pb.createAnd(reach, pb.createNot(B));
            if (H) E = pb.createAnd(E, H);
        } else if (st.kind == 'c') {
            const std::vector<PabloAST *> fse = getInputStreamSet("fse" + k);
            PabloAST * const fill = pb.createAnd(fse[0], notFollow);
            PabloAST * const reach = pb.createMatchStar(pb.createAnd(E, fse[1]), fill);
            // The positions with an end of an occurrence followed by H after
            // them in the run (see StringClassStarSpans).
            PabloAST * const notIndex = pb.createNot(getInputStreamSet("index" + k)[0]);
            PabloAST * const goodNext = getInputStreamSet("goodNext" + k)[0];
            PabloAST * const following = pb.createMatchStar(pb.createAdvance(goodNext, 1), notIndex);
            PabloAST * const beforeGood = pb.createOr(goodNext, pb.createAnd(following, notIndex));
            cover = pb.createOr(cover, pb.createAnd(pb.createAnd(reach, fill), beforeGood));
            PabloAST * next = pb.createAnd(reach, pb.createAdvance(fse[2], 1));
            if (st.n == 0) next = pb.createOr(next, E);
            E = H ? pb.createAnd(next, H) : next;
        }
        // 'z': zero-width, the entries are unchanged.
    }
    // The final stars: each covers its runs from the entries, and every
    // position it reaches is an entry of the next.
    for (unsigned i = 0; i < mFinals.size(); ++i) {
        const std::string name = "final" + std::to_string(i);
        if (mFinals[i].kind == 's') {
            PabloAST * const B = pb.createAnd(getInputStreamSet(name)[0], notFollow);
            PabloAST * const reach = pb.createMatchStar(E, B);
            cover = pb.createOr(cover, pb.createAnd(reach, B));
            E = reach;
        } else {
            const std::vector<PabloAST *> fse = getInputStreamSet(name);
            PabloAST * const fill = pb.createAnd(fse[0], notFollow);
            PabloAST * const reach = pb.createMatchStar(pb.createAnd(E, fse[1]), fill);
            cover = pb.createOr(cover, pb.createAnd(reach, fill));
            E = pb.createOr(E, pb.createAnd(reach, pb.createAdvance(fse[2], 1)));
        }
    }
    writeOutputStreamSet("coverage", std::vector<PabloAST *>{cover});
}

CharClassFillStartsEnds::CharClassFillStartsEnds(LLVMTypeSystemInterface & ts, StreamSet * X, StreamSet * follows,
                                                 StreamSet * fillStartsEnds)
: PabloKernel(ts, std::string("CharClassFillStartsEnds") + (follows ? "_f" : ""),
              [&] {
                  Bindings inputs;
                  inputs.emplace_back("X", X);
                  if (follows) inputs.emplace_back("follows", follows);
                  return inputs;
              }(),
              {Binding{"fillStartsEnds", fillStartsEnds}}),
  mHasFollows(follows != nullptr) {
}

void CharClassFillStartsEnds::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * X = getInputStreamSet("X")[0];
    if (mHasFollows) {
        X = pb.createAnd(X, pb.createNot(getInputStreamSet("follows")[0]));
    }
    writeOutputStreamSet("fillStartsEnds", std::vector<PabloAST *>{X, X, X});
}

void MatchedLinesKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    auto matchResults = getInputStreamSet("matchResults");
    PabloAST * lineBreaks = pb.createExtract(getInputStreamVar("lineBreaks"), pb.getInteger(0));
    PabloAST * notLB = pb.createNot(lineBreaks);
    PabloAST * match_follow = pb.createMatchStar(matchResults.back(), notLB);
    Var * const matchedLines = getOutputStreamVar("matchedLines");
    pb.createAssign(pb.createExtract(matchedLines, pb.getInteger(0)), pb.createAnd(match_follow, lineBreaks, "matchedLines"));
}

MatchedLinesKernel::MatchedLinesKernel (LLVMTypeSystemInterface & ts, StreamSet * Matches, StreamSet * LineBreakStream, StreamSet * MatchedLines)
: PabloKernel(ts, "MatchedLines" + std::to_string(Matches->getNumElements()),
// inputs
{Binding{"matchResults", Matches}
,Binding{"lineBreaks", LineBreakStream, FixedRate(), Principal()}},
// output
{Binding{"matchedLines", MatchedLines}}) {

}

StarChainFixedStep::StarChainFixedStep(LLVMTypeSystemInterface & ts, unsigned length, StreamSet * Fends,
                                       StreamSet * H, StreamSet * result)
: PabloKernel(ts, "StarChainFixed" + std::to_string(length) + (H ? "_h" : ""),
              [&] {
                  Bindings inputs;
                  inputs.emplace_back("Fends", Fends, FixedRate(1), LookAhead(length - 1));
                  if (H) {
                      inputs.emplace_back("H", H, FixedRate(1), LookAhead(length));
                  }
                  return inputs;
              }(),
              {Binding{"result", result}}),
  mLength(length), mHasH(H != nullptr) {
}

void StarChainFixedStep::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const Fends = getInputStreamSet("Fends")[0];
    // The starts of F matches, ...
    PabloAST * result = (mLength > 1) ? pb.createLookahead(Fends, mLength - 1) : Fends;
    // ... followed immediately by a position of H.
    if (mHasH) {
        result = pb.createAnd(result, pb.createLookahead(getInputStreamSet("H")[0], mLength));
    }
    writeOutputStreamSet("result", std::vector<PabloAST *>{result});
}

StarLookaheadSpans::StarLookaheadSpans(LLVMTypeSystemInterface & ts, unsigned lb, StreamSet * B, StreamSet * breaks,
                                       StreamSet * runStarts, StreamSet * Cstarts, StreamSet * spans)
: PabloKernel(ts, "StarLookaheadSpans" + std::to_string(lb) + (breaks ? "_br" : ""),
              [&] {
                  Bindings inputs = starLookaheadInputs(B, breaks, lb);
                  inputs.emplace_back("runStarts", runStarts);
                  inputs.emplace_back("Cstarts", Cstarts);
                  return inputs;
              }(),
              {Binding{"spans", spans}}),
  mLB(lb), mHasBreaks(breaks != nullptr) {
}

void StarLookaheadSpans::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * const B = getInputStreamSet("B")[0];
    PabloAST * const breaks = mHasBreaks ? getInputStreamSet("breaks")[0] : nullptr;
    // B' at the position i characters ahead.
    auto Bp = [&](unsigned i) -> PabloAST * {
        PabloAST * b = (i == 0) ? B : pb.createLookahead(B, i);
        if (breaks) {
            PabloAST * br = (i == 0) ? breaks : pb.createLookahead(breaks, i);
            b = pb.createAnd(b, pb.createNot(br));
        }
        return b;
    };
    PabloAST * const B0 = Bp(0);
    PabloAST * const Cstarts = getInputStreamSet("Cstarts")[0];
    // Keep only shifted C starts that reached the start of a run of B'.
    PabloAST * runStarts = pb.createAnd(getInputStreamSet("runStarts")[0],
                                        pb.createAnd(B0, pb.createNot(pb.createAdvance(B0, 1))));
    // From each run start through the following C start, and each C start itself.
    PabloAST * spans = pb.createOr(pb.createMatchStar(runStarts, B0), Cstarts);
    for (unsigned i = 0; i < mLB; ++i) {
        spans = pb.createAnd(spans, Bp(i));
    }
    writeOutputStreamSet("spans", std::vector<PabloAST *>{spans});
}




// In byte mode, a variable-length lookahead is compiled by the unique prefix
// method on UTF-8 code units, which can fail where it succeeds on characters
// (e.g. the prefix [éÉ] is an alternation of code unit sequences).
struct ByteModeLookaheads : public RE_Validator {
    ByteModeLookaheads() : RE_Validator("ByteModeLookaheads") {}

    bool validateAssertion(const Assertion * a) override {
        if (a->getKind() == Assertion::Kind::LookAhead) {
            const auto range = getLengthRange(a->getAsserted(), &cc::Unicode);
            if ((range.first != range.second) && !hasUniquePrefix(toUTF8(a->getAsserted()))) {
                return false;
            }
        }
        return validate(a->getAsserted());
    }
};

namespace {
// The matches of a bounded RE by their length (see below).
using LengthClasses = std::map<int, RE *>;
bool lengthClasses(RE * re, const cc::Alphabet * alphabet, unsigned limit, LengthClasses & classes);
// The limit on the number of length classes of an RE.
const unsigned LengthClassLimit = 16;
// Small bounded repetitions written out as alternations (BoundedRepExpander).
RE * expandBoundedReps(RE * re, const cc::Alphabet * lengthAlphabet);
}

namespace {
// Does the RE (including its assertions and the definitions of names) have a
// CC of U+FFFD, the character of each maximal ill-formed subpart of UTF-8
// input with codegen::UTF8ErrorMode::UseReplacementChar?
struct ReplacementCharClasses final : public RE_Inspector {
    ReplacementCharClasses() : RE_Inspector(NameProcessingMode::ProcessDefinition, InspectionMode::IgnoreNonUnique) {}
    void inspectCC(CC * cc) override {
        if ((cc->getAlphabet() == &cc::Unicode) && cc->contains(0xFFFD)) mFound = true;
    }
    bool mFound = false;
};

bool hasReplacementCharClass(RE * re) {
    ReplacementCharClasses classes;
    classes.inspectRE(re);
    return classes.mFound;
}

// Whether ill-formed UTF-8 input is characters U+FFFD (as it is up to the
// first error with Abort), rather than dropped.
bool replacedUTF8Errors() {
    return codegen::UTF8Errors != codegen::UTF8ErrorMode::DropBadInput;
}
}

static RE_Mode determineREMode(RE * re, const RE_ModeOptions & opts) {
    RE_Mode mode;
    // Ill-formed UTF-8 input dropped (see codegen::UTF8Errors) leaves the
    // characters around it adjacent: only the positions of characters (full
    // Unicode indexing) skip it, not those of code units.
    if (codegen::UTF8Errors == codegen::UTF8ErrorMode::DropBadInput) {
        mode.lengthAlphabet = &cc::Unicode;
        mode.indexAlphabet = &cc::Unicode;
        return mode;
    }
    // Determine the unit of length for the RE.  If the RE involves
    // fixed length UTF-8 sequences only, then UTF-8 can be used
    // for most efficient processing.   Otherwise we must use full
    // Unicode length calculations.
    bool useFixedUTF8 = !opts.unicodeIndexingOverride && validateFixedUTF8(re) && !hasPropertyReference(re);
    useFixedUTF8 = useFixedUTF8 && !opts.forceUnicodeIndexing;
    // A CC of U+FFFD matches each maximal ill-formed subpart of the input, of
    // 1 to 3 code units.
    if (useFixedUTF8 && replacedUTF8Errors() && hasReplacementCharClass(re)) {
        useFixedUTF8 = false;
    }
    // Byte mode tries every code unit position.  A match that consumes no
    // characters, but is conditioned by lookarounds, could then succeed
    // within a multibyte character.
    if (useFixedUTF8 && (getLengthRange(re, &cc::UTF8).first == 0) && hasAssertion(re)) {
        useFixedUTF8 = false;
    }
    // A lookahead chain (parseLookaheadChain) is compiled with one position
    // per character, which full Unicode indexing provides (its unbounded
    // length already rules out the UTF8-indexed mode).
    if (hasLookaheadChain(re)) {
        useFixedUTF8 = false;
    }
    if (useFixedUTF8 && !ByteModeLookaheads().validateRE(re)) {
        useFixedUTF8 = false;
    }
    // The starts of the matches (and search and replace) are computed in code
    // units: in byte mode only for alternatives of fixed length or with length
    // classes in UTF-8 code units (shifted back from their ends), which match
    // at least one character (an empty match would be found at every byte),
    // and otherwise with full Unicode indexing, never the UTF-8 indexed mode.
    const bool codeUnitGoal = (opts.goal == RE_Goal::MatchStarts) || (opts.goal == RE_Goal::SearchReplace);
    if (useFixedUTF8 && codeUnitGoal) {
        std::vector<RE *> alts;
        if (Alt * const alt = dyn_cast<Alt>(re)) {
            alts.assign(alt->begin(), alt->end());
        } else {
            alts.push_back(re);
        }
        // (Checking only: the passes print nothing.)
        QuietREPrinting quiet;
        for (RE * a : alts) {
            const auto range = getLengthRange(a, &cc::UTF8);
            LengthClasses classes;
            if (range.first == 0) {
                useFixedUTF8 = false;
            } else if ((range.first != range.second) &&
                    !lengthClasses(expandBoundedReps(toUTF8(a), &cc::UTF8), &cc::UTF8, LengthClassLimit, classes)) {
                useFixedUTF8 = false;
            }
        }
    }
    if (useFixedUTF8) {
        mode.lengthAlphabet = &cc::UTF8;
        mode.indexAlphabet = &cc::UTF8;
    } else {
        mode.lengthAlphabet = &cc::Unicode;
        // Determine whether UTF8-indexed or Unicode-indexed streams will
        // be used for regular expression processing.
        bool useIndexedUTF8 = !codeUnitGoal && !opts.unicodeBasisOverride
                                    && !hasReference(re)
                                    && !opts.forceUnicodeIndexing
                                    && !hasGraphemeClusterBoundary(re)
                                    && (maxLookaheadLength(re, &cc::Unicode) <= 1);
        if (useIndexedUTF8) {
            mode.indexAlphabet = &cc::UTF8;
        } else {
            mode.indexAlphabet = &cc::Unicode;
        }
    }
    return mode;
}

// An RE prepared for the goal of the options, with the replacement of a search
// and replace (keeping the captures it refers to).
static PreparedRE prepareFor(RE * re, const RE_ModeOptions & opts, RE * replacement) {
    PreparedRE prepared;
    // Property values given by regular expressions are resolved by a search
    // of the lines of property value names.
    re = resolveModesAndExternalSymbols(re, matchingLineNumbers, opts.goal, replacement);
    prepared.re = regular_expression_passes(re, &cc::Unicode, opts.goal);
    prepared.mode = determineREMode(prepared.re, opts);
    prepared.options = opts;
    return prepared;
}

PreparedRE prepareRE(RE * re, const RE_ModeOptions & opts) {
    return prepareFor(re, opts, nullptr);
}

PreparedSearchReplace prepareSearchReplace(RE * search, RE * replacement, const RE_ModeOptions & opts) {
    RE_ModeOptions o = opts;
    o.goal = RE_Goal::SearchReplace;
    // Captured text is kept with full Unicode indexing (the boundaries of
    // captures are found by walks over characters, see re::CaptureWalk), for
    // references that are not folded into literals.
    std::vector<re::ReplacementItem> items;
    if (replacement && foldReplacement(replacement, search, items)) {
        for (const re::ReplacementItem & item : items) {
            if (item.reference) o.forceUnicodeIndexing = true;
        }
    }
    PreparedSearchReplace sr;
    sr.search = prepareFor(search, o, replacement);
    sr.replacement = replacement;
    return sr;
}

std::pair<int, int> PreparedRE::lengthRange() const {
    return getLengthRange(re, mode.lengthAlphabet);
}

bool PreparedRE::endAnchored() const {
    return hasEndAnchor(re);
}

unsigned PreparedRE::grepOffset() const {
    // The UTF-8 indexed mode: Unicode length alphabet over UTF-8 code units.
    return re::grepOffset(re, (mode.lengthAlphabet == &cc::Unicode) && (mode.indexAlphabet == &cc::UTF8));
}

void matchingRecords(PipelineBuilder & P, RE * re, StreamSet * basis, StreamSet * u8index,
                     StreamSet * breaks, StreamSet * matchStarts, StreamSet * records) {
    const PreparedRE prepared = prepareRE(re);
    RE_PipelineBuilder RE_PB(P, RE_context{&cc::UTF8, basis, matchStarts, breaks});
    RE_PB.setU8IndexHint(u8index);
    StreamSet * const matches = P.CreateStreamSet();
    RE_PB.matchSearchPipeline(prepared, matches);
    // A match marks the end of the matched text; move each to its record break
    // (in the index space of the mode).
    if (!RE_PB.usesUnicodeIndexing()) {
        P.CreateKernelCall<MatchedLinesKernel>(matches, breaks, records);
        return;
    }
    StreamSet * const matchedRecords = P.CreateStreamSet();
    P.CreateKernelCall<MatchedLinesKernel>(matches, RE_PB.getMatchFollows(), matchedRecords);
    StreamSet * index = u8index;
    if (prepared.grepOffset() > 0) {
        index = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<AddSentinel>(u8index, index);
    }
    SpreadByMask(P, index, matchedRecords, records);
}

namespace {
// The line numbers reported by ScanMatchKernel.
struct LineNumberCollector {
    std::vector<uint64_t> lines;
};
void collect_line_number(LineNumberCollector * c, const size_t lineNum, char * /*start*/, char * /*end*/) {
    c->lines.push_back(lineNum);
}
void collect_finalize(LineNumberCollector * /*c*/, char * /*end*/) {
}
}

std::vector<uint64_t> matchingLineNumbers(RE * pattern, const char * buffer, size_t bufSize) {
    assert ((((uintptr_t)buffer) % (512 / 8)) == 0);
    CPUDriver driver("matchingLineNumbers");
    auto E = CreatePipeline(driver, Input<const char *>{"buffer"}, Input<size_t>{"length"},
                            Input<LineNumberCollector *>{"collector"});
    StreamSet * const ByteStream = E.CreateStreamSet(1, 8);
    E.CreateKernelCall<MemorySourceKernel>(E.getInputScalar(0), E.getInputScalar(1), ByteStream);
    StreamSet * const BasisBits = E.CreateStreamSet(8);
    E.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    StreamSet * const breaks = E.CreateStreamSet();
    E.CreateKernelCall<CharacterClassKernelBuilder>(std::vector<re::CC *>{re::makeCC(0x0A, &cc::UTF8)}, BasisBits, breaks);
    StreamSet * const matchStarts = E.CreateStreamSet(1, 1);
    E.CreateKernelCall<LineStartsKernel>(breaks, matchStarts);
    StreamSet * const u8index = E.CreateStreamSet();
    E.CreateKernelCall<UTF8_index>(BasisBits, u8index);
    StreamSet * const records = E.CreateStreamSet();
    matchingRecords(E, pattern, BasisBits, u8index, breaks, matchStarts, records);
    Kernel * const scanK = E.CreateKernelCall<ScanMatchKernel>(records, breaks, ByteStream, E.getInputScalar(2));
    E.LinkFunction(scanK, "accumulate_match_wrapper", collect_line_number);
    E.LinkFunction(scanK, "finalize_match_wrapper", collect_finalize);
    auto f = E.compile();
    LineNumberCollector collector;
    f(buffer, bufSize, &collector);
    return std::move(collector.lines);
}

RE_PipelineBuilder::RE_PipelineBuilder(PipelineBuilder & P, RE_context context)
: mPB(P), mCtxt(),
  mHaveSourceContext(true), mSourceContext(context), mPrepared(false), mHaveMode(false),
  mLineBreakHint(nullptr), mU8IndexHint(nullptr),
  mUsesUnicodeIndexing(false), mU8Index(nullptr),
  mFinalMatchStarts(context.matchStarts), mFinalMatchFollows(context.matchFollows) {
}

// Does the RE refer to the end of the text anywhere (including within
// assertions and the definitions of names): End, or a negative lookahead,
// which holds at the end of the text?
static bool hasEndAnchorAnywhere(const RE * re) {
    if (isa<End>(re)) return true;
    if (const Assertion * a = dyn_cast<Assertion>(re)) {
        if ((a->getKind() == Assertion::Kind::LookAhead) && (a->getSense() == Assertion::Sense::Negative)) return true;
    }
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (hasEndAnchorAnywhere(e)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (hasEndAnchorAnywhere(e)) return true;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return hasEndAnchorAnywhere(rep->getRE());
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        return hasEndAnchorAnywhere(a->getAsserted());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        return hasEndAnchorAnywhere(d->getLH()) || hasEndAnchorAnywhere(d->getRH());
    } else if (const re::Intersect * x = dyn_cast<re::Intersect>(re)) {
        return hasEndAnchorAnywhere(x->getLH()) || hasEndAnchorAnywhere(x->getRH());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return hasEndAnchorAnywhere(c->getCapturedRE());
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return hasEndAnchorAnywhere(g->getRE());
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() && hasEndAnchorAnywhere(n->getDefinition());
    }
    return false;
}

// Does the RE have End anchors anywhere (including within assertions and
// the definitions of names)?
static bool hasEndAnywhere(const RE * re) {
    if (isa<End>(re)) return true;
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (hasEndAnywhere(e)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (hasEndAnywhere(e)) return true;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return hasEndAnywhere(rep->getRE());
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        return hasEndAnywhere(a->getAsserted());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        return hasEndAnywhere(d->getLH()) || hasEndAnywhere(d->getRH());
    } else if (const re::Intersect * x = dyn_cast<re::Intersect>(re)) {
        return hasEndAnywhere(x->getLH()) || hasEndAnywhere(x->getRH());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return hasEndAnywhere(c->getCapturedRE());
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return hasEndAnywhere(g->getRE());
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() && hasEndAnywhere(n->getDefinition());
    }
    return false;
}

// Does the RE have Start or End anchors anywhere (including within
// assertions and the definitions of names)?
static bool hasAnchorAnywhere(const RE * re) {
    if (isa<Start>(re) || isa<End>(re)) return true;
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (hasAnchorAnywhere(e)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (hasAnchorAnywhere(e)) return true;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return hasAnchorAnywhere(rep->getRE());
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        return hasAnchorAnywhere(a->getAsserted());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        return hasAnchorAnywhere(d->getLH()) || hasAnchorAnywhere(d->getRH());
    } else if (const re::Intersect * x = dyn_cast<re::Intersect>(re)) {
        return hasAnchorAnywhere(x->getLH()) || hasAnchorAnywhere(x->getRH());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return hasAnchorAnywhere(c->getCapturedRE());
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return hasAnchorAnywhere(g->getRE());
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() && hasAnchorAnywhere(n->getDefinition());
    }
    return false;
}

namespace {
// The bits of a code at the marked positions, and of the classes elsewhere.
class CodeAtMarks final : public pablo::PabloKernel {
public:
    CodeAtMarks(LLVMTypeSystemInterface & ts, StreamSet * Classes, StreamSet * Marks, unsigned code, StreamSet * Result)
    : PabloKernel(ts, "CodeAtMarks_" + std::to_string(Classes->getNumElements()) + "_" + std::to_string(code),
                  {Binding{"Classes", Classes}, Binding{"Marks", Marks}}, {Binding{"Result", Result}}),
      mCode(code) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> classes = getInputStreamSet("Classes");
        PabloAST * const marks = getInputStreamSet("Marks")[0];
        PabloAST * const unmarked = pb.createNot(marks);
        std::vector<PabloAST *> result;
        for (unsigned i = 0; i < classes.size(); i++) {
            PabloAST * bit = pb.createAnd(classes[i], unmarked);
            if ((mCode >> i) & 1) bit = pb.createOr(bit, marks);
            result.push_back(bit);
        }
        writeOutputStreamSet("Result", result);
    }
private:
    const unsigned mCode;
};

// What, in an RE as prepared (including the definitions of names), needs the
// codepoints of the text rather than the classes of its CCs: properties
// (compiled as kernels over the basis), references, CCs of other alphabets
// and names without definitions.  The first found is reported.
struct CodepointNeeds final : public RE_Inspector {
    CodepointNeeds() : RE_Inspector(NameProcessingMode::ProcessDefinition, InspectionMode::IgnoreNonUnique) {}
    void inspectName(Name * n) override {
        if (n->getDefinition() == nullptr) {
            need("the name " + n->getFullName() + " has no definition");
        } else {
            RE_Inspector::inspectName(n);
        }
    }
    void inspectPropertyExpression(PropertyExpression * pe) override {
        need("the property " + pe->getFullName() + " is compiled from the codepoints");
    }
    void inspectReference(Reference * r) override {
        need("the reference " + r->getName() + " compares codepoints");
    }
    void inspectCC(CC * cc) override {
        if (cc->getAlphabet() != &cc::Unicode) need("a character class of the " + cc->getAlphabet()->getName() + " alphabet");
    }
    void need(const std::string & reason) {
        if (mReason.empty()) mReason = reason;
    }
    std::string mReason;
};
}

void RE_PipelineBuilder::ensurePrepared(RE *& re) {
    if (mPrepared) return;
    mPrepared = true;
    if (!mHaveSourceContext) {
        // Low-level construction: the caller already populated mCtxt by hand.
        return;
    }

    StreamSet * source = mSourceContext.source;
    StreamSet * matchStarts = mSourceContext.matchStarts;
    StreamSet * matchFollows = mSourceContext.matchFollows;

    // Without match regions, Start and End match at the start and the end of
    // the text: if the RE refers to the end of the text (End, or a negative
    // lookahead, which holds there), or a pipeline needs the follow of the
    // last match to be a position (mExtendText), the text (of bit streams) is made one
    // match region, from its first position to the position after its last
    // code unit, which the text is extended by (as an unterminated last line
    // is in grep).  The results then have that position too.
    if ((matchStarts == nullptr) && (source->getFieldWidth() == 1) && (mExtendText || hasEndAnchorAnywhere(re))) {
        StreamSet * const extended = mPB.CreateStreamSet(source->getNumElements(), 1);
        mPB.CreateKernelCall<ExtendByOne>(source, extended);
        matchFollows = mPB.CreateStreamSet(1, 1);
        mPB.CreateKernelCall<EndOfTextMark>(source, matchFollows);
        mTextExtended = true;
        matchStarts = mPB.CreateStreamSet(1, 1);
        mPB.CreateKernelCall<StartOfTextMark>(extended, matchStarts);
        source = extended;
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("textStart", matchStarts);
            mPB.captureBitstream("textFollow", matchFollows);
        }
    }

    const auto * const sourceCodes = dyn_cast<cc::MultiplexedAlphabet>(mSourceContext.encoding);
    if ((mSourceContext.encoding == &cc::Unicode) || sourceCodes) {
        // Already fully Unicode-encoded by the caller (e.g. csvgrep), or the
        // codes of a multiplexed alphabet of Unicode; use as-is.
        if (sourceCodes) {
            if (sourceCodes->getSourceAlphabet() != &cc::Unicode) {
                llvm::report_fatal_error("RE_PipelineBuilder: a multiplexed source must be of Unicode");
            }
            mCtxt.setMultiplexedCodeUnitContext(sourceCodes, source);
        } else {
            mCtxt.setCodeUnitContext(&cc::Unicode, source);
        }
        if (matchStarts) {
            mCtxt.setMatchRegions(matchStarts, matchFollows);
        }
        mUsesUnicodeIndexing = true;
        mFinalMatchStarts = matchStarts;
        mFinalMatchFollows = matchFollows;
        return;
    }

    RE_Mode mode = mHaveMode ? mMode : determineREMode(re, mModeOptions);

    if (mode.indexAlphabet == &cc::UTF8) {
        if ((source->getNumElements() == 1) &&
            ((mode.lengthAlphabet == &cc::Unicode) || hasReference(re) || !byteTestsWithinLimit(re, mModeOptions.byteCClimit))) {
            StreamSet * basis = mPB.CreateStreamSet(8, 1);
            Selected_S2P(mPB, source, basis);
            source = basis;
            if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
                mPB.captureBixNum("basis", basis);
            }
        }
        mCtxt.setCodeUnitContext(&cc::UTF8, source);
        if (matchStarts) {
            mCtxt.setMatchRegions(matchStarts, matchFollows);
        }
        if (mode.lengthAlphabet == &cc::Unicode) {
            if (mU8IndexHint) {
                mU8Index = mU8IndexHint;
            } else {
                mU8Index = mPB.CreateStreamSet(1, 1);
                mPB.CreateKernelCall<UTF8_index>(source, mU8Index, mLineBreakHint);
            }
            if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
                mPB.captureBitstream("mU8index", mU8Index);
            }
            mCtxt.setIndexingContext(&cc::Unicode, mU8Index);
        }
        mFinalMatchStarts = matchStarts;
        mFinalMatchFollows = matchFollows;
        return;
    }

    // Full Unicode indexing required.
    if (source->getNumElements() == 1) {
        StreamSet * basis = mPB.CreateStreamSet(8, 1);
        Selected_S2P(mPB, source, basis);
        source = basis;
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBixNum("basis", basis);
        }
    }
    if (mU8IndexHint) {
        mU8Index = mU8IndexHint;
    } else {
        mU8Index = mPB.CreateStreamSet(1, 1);
        mPB.CreateKernelCall<UTF8_index>(source, mU8Index, mLineBreakHint);
    }
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        mPB.captureBitstream("mU8index", mU8Index);
    }

    // The code units: the codes of a multiplexed alphabet of the CCs of the
    // RE (including those of its assertions and the definitions of names),
    // computed from the UTF-8 basis at the final code units of the
    // characters, or the 21-bit Unicode basis.  The CCs of the passes of the
    // pipelines are unions, intersections and complements of these, which
    // the codes express (see cc::MultiplexedAlphabet::transformCC).
    cc::MultiplexedAlphabet * mpx = nullptr;
    if (re::Multiplexing == re::MultiplexMode::Off) {
        mMultiplexingFailure = "multiplexing is off (--re-multiplex)";
    } else if (mModeOptions.unicodeBasisOverride) {
        mMultiplexingFailure = "the Unicode basis is required (unicodeBasisOverride)";
    } else if (mNeedsCodepoints) {
        mMultiplexingFailure = "the text of the replacement needs the codepoints";
    } else {
        CodepointNeeds needs;
        needs.inspectRE(re);
        mMultiplexingFailure = needs.mReason;
    }
    if (mMultiplexingFailure.empty()) {
        mpx = cc::makeMultiplexedAlphabet("mpx", re::collectCCs(re, cc::Unicode, re::NameProcessingMode::ProcessDefinition));
        const unsigned bits = mpx->getMultiplexedCCs().size();
        if (bits == 0) {
            mMultiplexingFailure = "the regular expression has no character classes";
        } else if ((re::Multiplexing == re::MultiplexMode::Auto) && (bits > re::MultiplexedStreamLimit)) {
            // More streams than the limit are not worth computing from the
            // UTF-8 basis in place of the 21 of the Unicode basis.
            mMultiplexingFailure = "its codes need " + std::to_string(bits) + " streams, more than " +
                                   std::to_string(re::MultiplexedStreamLimit) + " (--multiplexed-stream-limit)";
        }
        if (!mMultiplexingFailure.empty()) mpx = nullptr;
    }
    if (re::PrintOptionIsSet(re::ShowREs)) {
        if (mpx) {
            llvm::errs() << "Multiplexed: " << mpx->getSize() << " codes, " << mpx->getMultiplexedCCs().size() << " streams\n";
        } else {
            llvm::errs() << "Not multiplexed: " << mMultiplexingFailure << "\n";
        }
    }
    if (mpx) {
        const auto & mpx_basis = mpx->getMultiplexedCCs();
        StreamSet * u8CharClasses = mPB.CreateStreamSet(mpx_basis.size());
        mPB.CreateKernelFamilyCall<CharClassesKernel>(mpx_basis, source, u8CharClasses);
        if (replacedUTF8Errors()) {
            // Each maximal ill-formed subpart of the input is the character
            // U+FFFD, at its final code unit (see UTF8_index).
            StreamSet * const errors = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<UTF8_ErrorMarks>(source, errors, pablo::BitMovementMode::Advance);
            StreamSet * const replaced = mPB.CreateStreamSet(mpx_basis.size());
            mPB.CreateKernelCall<CodeAtMarks>(u8CharClasses, errors, mpx->codeOf(0xFFFD), replaced);
            u8CharClasses = replaced;
        }
        StreamSet * const codes = mPB.CreateStreamSet(mpx_basis.size());
        FilterByMask(mPB, mU8Index, u8CharClasses, codes);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBixNum("mpxCodes", codes);
        }
        mCtxt.setMultiplexedCodeUnitContext(mpx, codes);
        mMultiplexed = true;
    } else {
        StreamSet * u21_u8indexed = mPB.CreateStreamSet(21);
        mPB.CreateKernelCall<UTF8_Decoder>(source, u21_u8indexed);
        StreamSet * u21 = mPB.CreateStreamSet(21);
        FilterByMask(mPB, mU8Index, u21_u8indexed, u21);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBixNum("u21basis", u21);
        }
        mCtxt.setCodeUnitContext(&cc::Unicode, u21);
    }
    mUsesUnicodeIndexing = true;

    if (matchStarts) {
        // Recalculate the match regions in the new (Unicode-codeunit) index
        // space. Rather than independently filtering the byte-space starts
        // (which were already InFile-masked at byte granularity, and so can
        // disagree with an InFile mask applied at the coarser U21
        // granularity near EOF), filter follows and rederive starts from it
        // -- the same construction grep_engine itself uses to get starts
        // from a line-break (follows) stream in the first place.
        StreamSet * newFollows = mPB.CreateStreamSet(1);
        FilterByMask(mPB, mU8Index, matchFollows, newFollows);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("mU21_LB", newFollows);
        }
        StreamSet * newStarts = mPB.CreateStreamSet(1, 1);
        mPB.CreateKernelCall<LineStartsKernel>(newFollows, newStarts);
        mCtxt.setMatchRegions(newStarts, newFollows);
        mFinalMatchStarts = newStarts;
        mFinalMatchFollows = newFollows;
    }
}

void RE_PipelineBuilder::addExternal(std::string extName, ExternalStream s) {
    //llvm::errs() << "addExternal(" << extName << ", ";
    //if (s.kind == ExternalStreamKind::StartIndexed) llvm::errs() << "StartIndexed";
    //if (s.kind == ExternalStreamKind::ZeroWidth) llvm::errs() << "ZeroWidth";
    //if (s.kind == ExternalStreamKind::FixedLength) llvm::errs() << "FixedLength";
    //if (s.kind == ExternalStreamKind::EndIndexed) llvm::errs() << "EndIndexed";
    //llvm::errs() << ", (" << s.lgthRange.first << ", " << s.lgthRange.second << "), " << s.offset << ")\n";
    mCtxt.mExternals.emplace(extName, s);
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        mPB.captureBitstream(extName, s.extStream);
    }
}

void RE_PipelineBuilder::compileProperty(PropertyExpression * pe) {
    if (mCtxt.mMultiplexed) {
        llvm::report_fatal_error("RE_PipelineBuilder: property " + llvm::StringRef(pe->getFullName()) +
                                 " is compiled from the codepoints, not with a multiplexed basis");
    }
    StreamSet * pStrm  = mPB.CreateStreamSet(1);
    UnicodePropertyLogic(mPB, pe, mCtxt.mCodeUnitStream, mCtxt.mIndexStream, pStrm);
    std::string propName = pe->getFullName();
    if (pe->getKind() == re::PropertyExpression::Kind::Codepoint) {
        addExternal(propName, ExternalStream{ExternalStreamKind::FixedLength, 0, {1, 1}, pStrm});
    } else { //PropertyExpression::Kind::Boundary
        // The name given by UCD::PropertyExternalizer, e.g. \b{g} or \b{gc}.
        addExternal("\\b{" + propName + "}", ExternalStream{ExternalStreamKind::ZeroWidth, 1, {0, 0}, pStrm});
    }
}

void RE_PipelineBuilder::prepareExternals(RE * re) {
    std::set<re::Name *> externals;
    re::gatherNames(re, externals);
    for (const auto & e : externals) {
        compileExternal(e);
    }
}

void RE_PipelineBuilder::compileExternal(Name * n) {
    auto name = n->getFullName();
    auto f = mCtxt.mExternals.find(name);
    if (f != mCtxt.mExternals.end()) {
        // already compiled.
        return;
    }
    RE * defn = n->getDefinition();
    if (defn == nullptr) {
        llvm::report_fatal_error("RE_PipelineBuilder: Name is undefined.");
    }
    if (PropertyExpression * pe = dyn_cast<PropertyExpression>(defn)) {
        compileProperty(pe);
        return;
    }
    if (name.compare(0, ReversedLookaheadPrefix.size(), ReversedLookaheadPrefix) == 0) {
        // A lookahead matched by reversal (see ReversedLookaheadNamer): the
        // starts of the matches of its asserted RE, at their first characters.
        Assertion * const a = cast<Assertion>(defn);
        StreamSet * const starts = mPB.CreateStreamSet(1);
        reversedStarts(a->getAsserted(), starts);
        const auto r = getLengthRange(a->getAsserted(), mCtxt.mLengthAlphabet);
        addExternal(name, ExternalStream{ExternalStreamKind::StartIndexed, 1, r, starts,
                                         a->getSense() == Assertion::Sense::Negative});
        return;
    }
    //  Need to compile this defn.   Make sure all referenced externals
    //  are compiled first.
    prepareExternals(defn);
    //
    // The defining expression can now be compiled.  In most cases,
    // a single RE_Kernel can be used.   However, for lookahead
    // assertions (other than zero-width ones), the starts of the matches
    // of the asserted expression are needed (see matchStartPipeline).
    Assertion * const la = dyn_cast<Assertion>(defn);
    const bool lookahead = la && (la->getKind() == Assertion::Kind::LookAhead) &&
                           (getLengthRange(la->getAsserted(), mCtxt.mLengthAlphabet).second > 0);
    unsigned offset = grepOffset(defn, mCtxt.mIndexStream != nullptr);
    if (!lookahead) {
        // Not a lookahead; compile using a single kernel call.
        StreamSet * extStrm = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, defn, extStrm);
        auto r = getLengthRange(defn, mCtxt.mCodeUnitAlphabet);
        if (r.second == 0) {
            addExternal(name, ExternalStream{ExternalStreamKind::ZeroWidth, 1, r, extStrm});
        } else if (r.first == r.second) {
            addExternal(name, ExternalStream{ExternalStreamKind::FixedLength, offset, r, extStrm});
        } else {
            addExternal(name, ExternalStream{ExternalStreamKind::EndIndexed, offset, r, extStrm});
        }
    } else {
        // The external stream marks where the positive lookahead holds, for
        // either sense; negative lookaheads are negated where the stream is
        // read, so that they also hold for positions whose lookahead extends
        // past the end of the data.  The sense is part of the RE_Kernel signature.
        Assertion * a = llvm::cast<Assertion>(defn);
        RE * asserted = a->getAsserted();
        const bool negated = (a->getSense() == Assertion::Sense::Negative);
        auto r = getLengthRange(asserted, mCtxt.mLengthAlphabet);
        const MatchStarts starts = matchStartPipeline(asserted);
        addExternal(name, ExternalStream{ExternalStreamKind::StartIndexed, starts.offset, r, starts.stream, negated});
    }
}

RE_PipelineBuilder::MatchStarts RE_PipelineBuilder::matchStartPipeline(RE * re) {
    const auto r = getLengthRange(re, mCtxt.mLengthAlphabet);
    Name * prefName = nullptr;
    if (Seq * const seq = dyn_cast<Seq>(re)) {
        Name * const front = seq->empty() ? nullptr : dyn_cast<Name>(seq->front());
        if (front && isUniquePrefixName(front)) prefName = front;
    }
    if ((r.first != r.second) && (prefName == nullptr)) {
        // A lookahead chain, compiled with one position per character.
        std::vector<LookaheadSegment> segments;
        if ((mCtxt.mCodeUnitAlphabet != &cc::Unicode) || !parseLookaheadChain(re, mCtxt.mLengthAlphabet, segments)) {
            llvm::report_fatal_error(llvm::StringRef("Unsupported lookahead assertion: ") + Printer_RE::PrintRE(re));
        }
        return MatchStarts{chainMatchStarts(segments), 1};
    }
    StreamSet * const ends = mPB.CreateStreamSet(1);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, re, ends);
    if (r.first == r.second) {
        // Fixed length: the ends are the starts, offset by the length.
        return MatchStarts{ends, static_cast<unsigned>(r.second)};
    }
    // A unique prefix: the ends are shifted back to the matches of the prefix
    // that begin them.
    auto f = mCtxt.mExternals.find(prefName->getFullName());
    assert(f != mCtxt.mExternals.end());
    StreamSet * const prefStrm = f->second.extStream;
    StreamSet * const endsBack = uniquePrefixEndsBack(prefStrm, ends);
    StreamSet * const starts = mPB.CreateStreamSet(1);
    AndCombine(mPB, prefStrm, endsBack, starts);
    const auto pfxRange = getLengthRange(prefName, mCtxt.mLengthAlphabet);
    if (pfxRange.first != pfxRange.second) {
        llvm::report_fatal_error("Expecting a fixed length unique prefix");
    }
    return MatchStarts{starts, static_cast<unsigned>(pfxRange.first)};
}

//
// The starts of the matches of a lookahead chain (e.g. B*C D{2,}E), with one
// position per character (see StarChainFixedStep, StarLookaheadIndex and
// StarLookaheadSpans).  Processing the segments from the right, the stream H
// marks the first character after each position where the rest of the chain
// holds; for the whole chain, these are the starts of its matches.
//
StreamSet * RE_PipelineBuilder::chainMatchStarts(const std::vector<LookaheadSegment> & segments,
                                                  std::vector<ChainStepStreams> * steps, StreamSet * Hend) {
    if (steps) steps->assign(segments.size(), ChainStepStreams{});
    StreamSet * H = Hend;  // where the rest after the chain holds (nullptr: all positions)
    for (auto seg = segments.rbegin(); seg != segments.rend(); ++seg) {
        ChainStepStreams * const step = steps ? &(*steps)[segments.rend() - seg - 1] : nullptr;
        if (step) step->Hnext = H;
        if (seg->assertion) {
            // One-character lookaheads (see ChainAssertionEnd), within the
            // chain (where the rest must also hold) or at its end.
            std::vector<StreamSet *> classes;
            std::vector<bool> negated;
            for (Assertion * la : seg->assertions) {
                StreamSet * const Y = mPB.CreateStreamSet(1);
                mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, la->getAsserted(), Y);
                classes.push_back(Y);
                negated.push_back(la->getSense() == Assertion::Sense::Negative);
            }
            StreamSet * const Hrest = H;
            H = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<ChainAssertionEnd>(classes, negated, mCtxt.mMatchFollows, Hrest, H);
            continue;
        }
        if (seg->end) {
            // The end of the text holds at a position followed by the end of
            // a match region (as End is compiled): H is the region follows.
            // (Without match regions, the text is made one region, see
            // ensurePrepared.)
            if (mCtxt.mMatchFollows == nullptr) {
                llvm::report_fatal_error("A lookahead ending with the end of the text requires match regions");
            }
            H = mCtxt.mMatchFollows;
            continue;
        }
        if (!seg->star) {
            StreamSet * const Fends = mPB.CreateStreamSet(1);
            mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, seg->re, Fends);
            StreamSet * const result = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<StarChainFixedStep>(seg->length, Fends, H, result);
            H = result;
            continue;
        }
        if (!seg->strings.empty()) {
            // A string class (see StringClassStarIndex).
            StreamSet * const fillStartsEnds = mPB.CreateStreamSet(3);
            if ((seg->strings.size() == 1) && (seg->strings[0].size() == 1)) {
                // A class X (of one-character strings): fill, starts and ends
                // are all X, which may already be an external.
                StreamSet * const X = classStream(seg->re, seg->cc);
                mPB.CreateKernelCall<CharClassFillStartsEnds>(X, mCtxt.mMatchFollows, fillStartsEnds);
            } else {
                mPB.CreateKernelCall<StringClassKernel>(codeUnitStrings(seg->strings), codeUnitBasis(), fillStartsEnds,
                                                        mCtxt.mMatchFollows);
            }
            StreamSet * const index = mPB.CreateStreamSet(1);
            StreamSet * const good = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<StringClassStarIndex>(fillStartsEnds, H, mCtxt.mMatchFollows, index, good);
            StreamSet * const goodNext = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<IndexedShiftBack>(index, good, goodNext);
            StreamSet * const result = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<StringClassStarSpans>(seg->lb, fillStartsEnds, H, index, goodNext, result);
            if (step) {
                step->fse = fillStartsEnds;
                step->index = index;
                step->goodNext = goodNext;
            }
            if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
                mPB.captureBixNum("scFSE", fillStartsEnds);
                mPB.captureBitstream("scH", H);
                mPB.captureBitstream("scIndex", index);
                mPB.captureBitstream("scGood", good);
                mPB.captureBitstream("scGoodNext", goodNext);
                mPB.captureBitstream("scResult", result);
            }
            H = result;
            continue;
        }
        StreamSet * const Bstrm = classStream(seg->re, seg->cc);
        if (step) step->cls = Bstrm;
        StreamSet * const index = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<StarLookaheadIndex>(Bstrm, mCtxt.mMatchFollows, index);
        StreamSet * const runStarts = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<IndexedShiftBack>(index, H, runStarts);
        StreamSet * const spans = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<StarLookaheadSpans>(seg->lb, Bstrm, mCtxt.mMatchFollows, runStarts, H, spans);
        H = spans;
    }
    return H;
}

StreamSet * RE_PipelineBuilder::uniquePrefixEndsBack(StreamSet * prefix, StreamSet * ends) {
    // An indexed shift back over the positions of the prefix and the ends moves
    // each end to the position of the preceding prefix (or end).
    StreamSet * const mask = mPB.CreateStreamSet(1);
    OrCombine(mPB, prefix, ends, mask);
    StreamSet * const endsBack = mPB.CreateStreamSet(1);
    mPB.CreateKernelCall<IndexedShiftBack>(mask, ends, endsBack);
    return endsBack;
}

void RE_PipelineBuilder::matchSearchPipeline(RE * re, StreamSet * results) {
    if (mHaveSourceContext) {
        matchSearchPipeline(::prepareRE(re, mModeOptions), results);
        return;
    }
    // A hand-built RE_CompilerContext: the RE is compiled as given.
    mRE = applyModePasses(re, RE_Goal::MatchEnds);
    mRE = processReferences(mRE);
    prepareExternals(mRE);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, mRE, results);
}

void RE_PipelineBuilder::matchSearchPipeline(const PreparedRE & prepared, StreamSet * results) {
    usePrepared(prepared);
    RE * re = prepared.re;
    ensurePrepared(re);
    mRE = applyModePasses(re, RE_Goal::MatchEnds);
    mRE = processReferences(mRE);
    prepareExternals(mRE);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, mRE, results);
}

void RE_PipelineBuilder::usePrepared(const PreparedRE & prepared) {
    if (mHaveSourceContext) {
        mModeOptions = prepared.options;
        mMode = prepared.mode;
        mHaveMode = true;
    }
}

void RE_PipelineBuilder::matchSpanPipeline(RE * re, StreamSet * matches, StreamSet * spans) {
    if (mHaveSourceContext) {
        matchSpanPipeline(::prepareRE(re, mModeOptions), matches, spans);
        return;
    }
    matchSpanPipeline(PreparedRE{re, RE_Mode{nullptr, nullptr}, mModeOptions}, matches, spans);
}

void RE_PipelineBuilder::matchSpanPipeline(const PreparedRE & prepared, StreamSet * matches, StreamSet * spans) {
    usePrepared(prepared);
    RE * re = prepared.re;
    ensurePrepared(re);
    mRE = applyModePasses(re, RE_Goal::MatchSpans);
    mRE = processReferences(mRE);
    prepareExternals(mRE);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, mRE, matches);
    // The spans are the union of the extents of the nonempty matches,
    // computed for the alternatives of a factored form of the RE.
    RE * const spanRE = spanFactoring(mRE);
    prepareExternals(spanRE);
    getSpan(spanRE, spans);
}

void RE_PipelineBuilder::getSpan(RE * re, StreamSet * spans) {
    if (Alt * a = dyn_cast<Alt>(re)) {
        if (a->size() == 1) {
            getSpan(a->front(), spans);
        } else {
            std::vector<StreamSet *> allSpans;
            for (auto & e : *a) {
                StreamSet * span = mPB.CreateStreamSet(1);
                getSpan(e, span);
                allSpans.push_back(span);
            }
            mPB.CreateKernelCall<StreamsMerge>(allSpans, spans);
        }
    } else if (Name * n = dyn_cast<Name>(re)) {
        auto name = n->getFullName();
        //llvm::errs() << "getSpan finding external: " << name << "\n";
        auto f = mCtxt.mExternals.find(name);
        assert(f != mCtxt.mExternals.end());
        auto matchEnd = f->second.extStream;
        auto minlgth = f->second.lgthRange.first;
        auto endOffset = f->second.offset;
        //llvm::errs() << "endOffset: " << endOffset << "\n";
        mPB.CreateKernelCall<FixedMatchSpansKernel>(minlgth, endOffset, matchEnd, spans);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            auto spanName = name + "Span";
            mPB.captureBitstream(spanName, spans);
        }
    } else if (chainSpans(re, spans)) {
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("chainSpans", spans);
        }
    } else if (!uniquePrefixSpans(re, spans) && !lengthClassSpans(re, spans)) {
        StreamSet * matchEnd = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, re, matchEnd);
        auto minlgth = getLengthRange(re, mCtxt.mLengthAlphabet).first;
        auto offset = grepOffset(re, mCtxt.mIndexStream != nullptr);
        mPB.CreateKernelCall<FixedMatchSpansKernel>(minlgth, offset, matchEnd, spans);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            auto spanName = "minlen" + std::to_string(minlgth + offset);
            mPB.captureBitstream(spanName, spans);
        }
    }
}

StreamSet * RE_PipelineBuilder::codeUnitBasis() {
    if (mCodeUnitBasis == nullptr) {
        mCodeUnitBasis = mCtxt.mCodeUnitStream;
        if (mCodeUnitBasis->getFieldWidth() > 1) {
            mCodeUnitBasis = mPB.CreateStreamSet(8, 1);
            Selected_S2P(mPB, mCtxt.mCodeUnitStream, mCodeUnitBasis);
        }
    }
    return mCodeUnitBasis;
}

CC * RE_PipelineBuilder::codeUnitCC(CC * cc) const {
    if (mCtxt.mMultiplexed && (cc->getAlphabet() == mCtxt.mMultiplexed->getSourceAlphabet())) {
        return mCtxt.mMultiplexed->transformCC(cc);
    }
    return cc;
}

std::vector<std::vector<CC *>> RE_PipelineBuilder::codeUnitStrings(const std::vector<std::vector<CC *>> & strings) const {
    std::vector<std::vector<CC *>> translated;
    for (const auto & str : strings) {
        translated.emplace_back();
        for (CC * cc : str) translated.back().push_back(codeUnitCC(cc));
    }
    return translated;
}

const cc::Alphabet * RE_PipelineBuilder::codeUnitEncoding() const {
    if (mCtxt.mMultiplexed) return mCtxt.mMultiplexed;
    return mCtxt.mCodeUnitAlphabet;
}

namespace {
// The streams of classes of the codes of a multiplexed alphabet (given as
// CCs of the alphabet), over the basis of the codes.
class MultiplexedClassesKernel final : public pablo::PabloKernel {
public:
    MultiplexedClassesKernel(LLVMTypeSystemInterface & ts, std::vector<CC *> ccs, StreamSet * basis, StreamSet * classes)
    : MultiplexedClassesKernel(ts, signature(ccs, basis), std::move(ccs), basis, classes) {}
    bool hasSignature() const override { return true; }
    llvm::StringRef getSignature() const override { return mSignature; }
protected:
    MultiplexedClassesKernel(LLVMTypeSystemInterface & ts, std::string sig, std::vector<CC *> && ccs,
                             StreamSet * basis, StreamSet * classes)
    : PabloKernel(ts, "mpxcc_" + getStringHash(sig), {Binding{"basis", basis}}, {Binding{"classes", classes}}),
      mCCs(std::move(ccs)), mSignature(std::move(sig)) {}
    static std::string signature(const std::vector<CC *> & ccs, StreamSet * basis) {
        std::string sig = std::to_string(basis->getNumElements());
        for (const CC * cc : ccs) sig += "|" + cc->canonicalName();
        return sig;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        cc::Parabix_CC_Compiler_Builder ccc(getInputStreamSet("basis"));
        std::vector<PabloAST *> classes;
        for (CC * cc : mCCs) classes.push_back(pb.createInFile(ccc.compileCC(cc, pb)));
        writeOutputStreamSet("classes", classes);
    }
private:
    const std::vector<CC *> mCCs;
    const std::string mSignature;
};
}

void RE_PipelineBuilder::compileClasses(const std::vector<CC *> & ccs, StreamSet * out, StreamSet * basis) {
    if (mCtxt.mMultiplexed) {
        std::vector<CC *> codes;
        for (CC * cc : ccs) codes.push_back(codeUnitCC(cc));
        mPB.CreateKernelCall<MultiplexedClassesKernel>(codes, mCtxt.mCodeUnitStream, out);
    } else {
        mPB.CreateKernelFamilyCall<CharClassesKernel>(ccs, basis ? basis : codeUnitBasis(), out);
    }
}

StreamSet * RE_PipelineBuilder::classStream(RE * re, CC * cc) {
    // The class may already be an external (e.g. a property), whose kernel
    // must then be the one used.
    if (Name * const n = dyn_cast<Name>(re)) {
        auto f = mCtxt.mExternals.find(n->getFullName());
        if (f != mCtxt.mExternals.end()) return f->second.extStream;
    }
    StreamSet * const strm = mPB.CreateStreamSet(1);
    compileClasses(std::vector<re::CC *>{cc}, strm);
    return strm;
}

bool RE_PipelineBuilder::uniquePrefixSpans(RE * re, StreamSet * spans) {
    RE * prefix, * suffix;
    std::tie(prefix, suffix) = ParseUniquePrefix(re);
    if (isEmptySeq(prefix) || isEmptySeq(suffix)) return false;
    Name * const pfxName = makeUniquePrefixName(prefix);
    compileExternal(pfxName);
    auto fp = mCtxt.mExternals.find(pfxName->getFullName());
    assert(fp != mCtxt.mExternals.end());
    StreamSet * const pfxStrm = fp->second.extStream;
    const auto pfxLgth = fp->second.lgthRange.first;
    const auto pfxOffset = fp->second.offset;
    RE * const split = makeSeq({pfxName, suffix});
    StreamSet * const matchEnd = mPB.CreateStreamSet(1);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, split, matchEnd);
    StreamSet * const endBack = uniquePrefixEndsBack(pfxStrm, matchEnd);
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        mPB.captureBitstream("pfxStrm", pfxStrm);
        mPB.captureBitstream("endBack", endBack);
    }
    mPB.CreateKernelCall<LongestSpan>(pfxLgth + pfxOffset - 1, grepOffset(split, mCtxt.mIndexStream != nullptr), pfxStrm, endBack, matchEnd, spans);
    return true;
}

namespace {
// The strings matched by r (an alternation of sequences of items resolving
// to character classes), or false if r is not of that form.
bool classStrings(RE * r, std::vector<std::vector<CC *>> & strings) {
    if (CC * const cc = resolveCharClass(r)) {
        strings.push_back({cc});
        return !cc->empty();
    }
    return parseStringClass(r, strings);
}

// Is every string matched by Y also matched by X (character classes, or
// string classes; see classStrings)?
bool coversStrings(RE * X, RE * Y) {
    std::vector<std::vector<CC *>> xs, ys;
    if (!classStrings(X, xs) || !classStrings(Y, ys)) return false;
    for (const auto & y : ys) {
        bool covered = false;
        for (const auto & x : xs) {
            if (x.size() != y.size()) continue;
            covered = true;
            for (size_t i = 0; covered && (i < y.size()); ++i) covered = y[i]->subset(*x[i]);
            if (covered) break;
        }
        if (!covered) return false;
    }
    return true;
}

// An assertion, possibly as the definition of a name (an externalized
// assertion), or nullptr.
Assertion * definedAssertion(RE * r) {
    while (Name * const n = dyn_cast<Name>(r)) {
        if (n->getDefinition() == nullptr) return nullptr;
        r = n->getDefinition();
    }
    return dyn_cast<Assertion>(r);
}

// A negative lookahead assertion (see definedAssertion), or nullptr.
Assertion * negativeLookahead(RE * r) {
    Assertion * const a = definedAssertion(r);
    if ((a == nullptr) || (a->getKind() != Assertion::Kind::LookAhead) || (a->getSense() != Assertion::Sense::Negative)) {
        return nullptr;
    }
    return a;
}

// Is r a lookahead assertion (see definedAssertion)?
bool isLookahead(RE * r) {
    Assertion * const a = definedAssertion(r);
    return a && (a->getKind() == Assertion::Kind::LookAhead);
}

// Do the negative lookaheads among the assertions fail wherever an
// occurrence of a string of X begins: does each string of X begin with (a
// subset of) a string of the body of one of them?
bool excludesStrings(RE * X, const std::vector<RE *> & assertions) {
    std::vector<std::vector<CC *>> xs;
    if (!classStrings(X, xs)) return false;
    // The one-character strings of the lookaheads exclude their union.
    std::vector<std::vector<CC *>> ys;
    CC * single = nullptr;
    for (RE * a : assertions) {
        if (Assertion * const la = negativeLookahead(a)) {
            std::vector<std::vector<CC *>> strings;
            if (!classStrings(la->getAsserted(), strings)) continue;
            for (auto & y : strings) {
                if ((y.size() == 1) && (single == nullptr || single->getAlphabet() == y[0]->getAlphabet())) {
                    single = single ? makeCC(single, y[0]) : y[0];
                } else {
                    ys.push_back(y);
                }
            }
        }
    }
    if (single) ys.push_back({single});
    for (const auto & x : xs) {
        bool excluded = false;
        for (const auto & y : ys) {
            if (y.size() > x.size()) continue;
            excluded = true;
            for (size_t i = 0; excluded && (i < y.size()); ++i) excluded = x[i]->subset(*y[i]);
            if (excluded) break;
        }
        if (!excluded) return false;
    }
    return true;
}

// Is r a lookbehind assertion (see definedAssertion)?
bool isLookbehind(RE * r) {
    Assertion * const a = definedAssertion(r);
    return a && (a->getKind() == Assertion::Kind::LookBehind);
}
}

namespace {
// The matches of a bounded RE by their length: classes[k] matches exactly
// the strings of length k that the RE matches (with the same contexts).
// Alternations are divided among the classes, the classes of the items of a
// sequence are combined, and X? is the alternation of X and the empty
// string; other repetitions must have a fixed length.  (Zero-width
// assertions, such as the alternative (?!X) of a possessive X?+, have length
// 0.)  Returns false if the RE is not of this form, or would have more than
// limit classes at any step.
bool lengthClasses(RE * re, const cc::Alphabet * alphabet, unsigned limit, LengthClasses & classes) {
    classes.clear();
    const auto range = getLengthRange(re, alphabet);
    if (range.first == range.second) {
        if (range.first == INT_MAX) return false;
        classes.emplace(range.first, re);
        return true;
    }
    if (range.second == INT_MAX) return false;
    auto add = [](LengthClasses & C, int k, RE * r) {
        auto f = C.find(k);
        if (f == C.end()) {
            C.emplace(k, r);
        } else {
            f->second = makeAlt({f->second, r});
        }
    };
    if (Alt * const alt = dyn_cast<Alt>(re)) {
        for (RE * e : *alt) {
            LengthClasses E;
            if (!lengthClasses(e, alphabet, limit, E)) return false;
            for (const auto & c : E) add(classes, c.first, c.second);
        }
    } else if (Seq * const seq = dyn_cast<Seq>(re)) {
        classes.emplace(0, makeSeq());
        for (RE * e : *seq) {
            LengthClasses E;
            if (!lengthClasses(e, alphabet, limit, E)) return false;
            LengthClasses next;
            for (const auto & a : classes) {
                for (const auto & b : E) add(next, a.first + b.first, makeSeq({a.second, b.second}));
            }
            if (next.size() > limit) return false;
            classes = std::move(next);
        }
    } else if (Rep * const rep = dyn_cast<Rep>(re)) {
        if ((rep->getLB() != 0) || (rep->getUB() != 1)) return false;
        if (!lengthClasses(rep->getRE(), alphabet, limit, classes)) return false;
        add(classes, 0, makeSeq());
    } else if (Name * const n = dyn_cast<Name>(re)) {
        if (n->getDefinition() == nullptr) return false;
        return lengthClasses(n->getDefinition(), alphabet, limit, classes);
    } else {
        return false;
    }
    return classes.size() <= limit;
}
}

bool RE_PipelineBuilder::lengthClassSpans(RE * re, StreamSet * spans) {
    // Spans are counted in code units.
    if (mCtxt.mIndexStream != nullptr) return false;
    LengthClasses classes;
    if (!lengthClasses(re, mCtxt.mLengthAlphabet, LengthClassLimit, classes)) return false;
    // Empty matches have no spans.
    classes.erase(0);
    if (classes.empty()) return false;
    std::vector<StreamSet *> allSpans;
    for (const auto & c : classes) {
        StreamSet * const matchEnd = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, c.second, matchEnd);
        StreamSet * const span = (classes.size() == 1) ? spans : mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<FixedMatchSpansKernel>(c.first, grepOffset(c.second, mCtxt.mIndexStream != nullptr), matchEnd, span);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("lengthClass" + std::to_string(c.first), span);
        }
        allSpans.push_back(span);
    }
    if (allSpans.size() > 1) {
        mPB.CreateKernelCall<StreamsMerge>(allSpans, spans);
    }
    return true;
}

//
// A lookahead chain, possibly preceded by lookbehinds and followed by final
// stars or lookaheads, as matched by chainSpans and chainFormStarts (see
// parseChainForm).
//
struct ChainForm {
    // A final star: 's' of a class cc, 'c' of a string class (strings).
    struct Final {
        char kind;
        RE * re;
        CC * cc;
        std::vector<std::vector<CC *>> strings;
    };
    std::vector<RE *> lookbehinds;      // at the start: restrict the starts
    std::vector<RE *> afterChain;       // lookaheads after the chain
    std::vector<Final> finals;          // from the right
    bool occurrenceRequired = false;    // an occurrence of the first final string class
    std::vector<LookaheadSegment> segments;
};

// Parse an RE as a chain with final stars (see chainSpans): no kernels are
// made, so that the form can be checked before compiling.
static bool parseChainForm(RE * re, const cc::Alphabet * lengthAlphabet, ChainForm & form) {
    std::vector<RE *> elems;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        elems.assign(seq->begin(), seq->end());
    } else {
        elems.push_back(re);
    }
    // Lookbehinds at the start hold at the starts of the matches: they are
    // compiled on their own and restrict the starts of the rest.
    const auto lookbehindEnd = std::find_if_not(elems.begin(), elems.end(), isLookbehind);
    form.lookbehinds.assign(elems.begin(), lookbehindEnd);
    elems.erase(elems.begin(), lookbehindEnd);
    if (elems.empty()) return false;
    // Negative lookaheads (?!Y) at the end (possibly externalized), following
    // a final star X{lb,} where every string of Y is a string of X (as in the
    // possessive form X{lb,} (?!X)), do not change the spans: the furthest
    // end of a match from each start cannot be followed by Y, as X{lb,} would
    // extend over it.  They are dropped.
    size_t n = elems.size();
    while ((n > 1) && negativeLookahead(elems[n - 1])) {
        n--;
    }
    if (n < elems.size()) {
        Rep * const rep = dyn_cast<Rep>(elems[n - 1]);
        if (rep && (rep->getUB() == Rep::UNBOUNDED_REP)) {
            bool covered = true;
            for (size_t i = n; covered && (i < elems.size()); ++i) {
                covered = coversStrings(rep->getRE(), negativeLookahead(elems[i])->getAsserted());
            }
            if (covered) elems.resize(n);
        }
    }
    // Lookaheads at the end following a final star X{lb,}, whose negative
    // lookaheads fail wherever an occurrence of X begins (as in the
    // possessive form X{lb,} (?!X) followed by an after context), hold only
    // at the ends of the runs of X.  Unless they are all one-character
    // lookaheads (which the chain itself handles), they are compiled on their
    // own, as the positions where the rest after the chain holds, and the
    // star is the last segment of the chain.
    std::vector<RE *> & afterChain = form.afterChain;
    {
        size_t m = elems.size();
        while ((m > 1) && isLookahead(elems[m - 1])) {
            m--;
        }
        const std::vector<RE *> lookaheads(elems.begin() + m, elems.end());
        Rep * const rep = lookaheads.empty() ? nullptr : dyn_cast<Rep>(elems[m - 1]);
        const bool oneCharacter = std::all_of(lookaheads.begin(), lookaheads.end(), [](RE * a) {
            return resolveCharClass(definedAssertion(a)->getAsserted()) != nullptr;
        });
        if (rep && (rep->getUB() == Rep::UNBOUNDED_REP) && !oneCharacter && excludesStrings(rep->getRE(), lookaheads)) {
            afterChain = lookaheads;
            elems.resize(m);
        }
    }
    // Final stars, each of a class or of a string class: a final X{lb,} of a
    // class with lb > 0 is X{lb} followed by X*, where X{lb} ends the chain;
    // a string class with lb = 1 must be the first final star, and the
    // chain then ends where an occurrence begins.  (Kernels are created only
    // once the form is known.)
    using Final = ChainForm::Final;
    std::vector<Final> & finals = form.finals;
    bool & occurrenceRequired = form.occurrenceRequired;
    while (afterChain.empty() && !elems.empty()) {
        Rep * const rep = dyn_cast<Rep>(elems.back());
        if ((rep == nullptr) || (rep->getUB() != Rep::UNBOUNDED_REP)) break;
        RE * const X = rep->getRE();
        const int lb = rep->getLB();
        Final f{0, X, nullptr, {}};
        if ((f.cc = resolveCharClass(X))) {
            f.kind = 's';
            elems.pop_back();
            finals.push_back(f);
            if (lb > 0) {
                elems.push_back(makeRep(X, lb, lb));
                break;
            }
        } else if ((lb <= 1) && parseStringClass(X, f.strings) && isRepeatableStringClass(f.strings)) {
            f.kind = 'c';
            elems.pop_back();
            finals.push_back(f);
            // With lb = 1, an occurrence must begin where the chain ends.
            if (lb == 1) {
                occurrenceRequired = true;
                break;
            }
        } else {
            break;
        }
    }
    std::vector<LookaheadSegment> & segments = form.segments;
    if (elems.empty()) {
        if (!occurrenceRequired) return false;
    } else {
        // When an occurrence of the first final string class is required,
        // the chain is parsed as followed by a lookahead to the first
        // characters of the strings (so that a star at its end is checked
        // against them), which is then replaced by the starts of the
        // occurrences (Hend, below).
        if (occurrenceRequired) {
            CC * first = nullptr;
            for (const auto & str : finals.back().strings) first = first ? makeCC(first, str[0]) : str[0];
            elems.push_back(makeLookAheadAssertion(first));
        }
        // Likewise, lookaheads after the chain are parsed as a lookahead to
        // the characters that can follow its final star (not of a class X,
        // or any character after a string class), which is then replaced by
        // the positions where they hold (Hend, below).
        if (!afterChain.empty()) {
            RE * const X = cast<Rep>(elems.back())->getRE();
            if (CC * const cc = resolveCharClass(X)) {
                elems.push_back(makeNegativeLookAheadAssertion(cc));
            } else {
                std::vector<std::vector<CC *>> strings;
                if (!classStrings(X, strings)) return false;
                const cc::Alphabet * const alphabet = strings[0][0]->getAlphabet();
                elems.push_back(makeLookAheadAssertion(makeCC(0, UCD::UNICODE_MAX, alphabet)));
            }
        }
        if (!parseLookaheadChain(makeSeq(elems.begin(), elems.end()), lengthAlphabet, segments, false)) {
            return false;
        }
        if (occurrenceRequired || !afterChain.empty()) segments.pop_back();
        if (segments.empty()) return false;
    }
    return true;
}

StreamSet * RE_PipelineBuilder::chainFormStarts(const ChainForm & form, std::vector<ChainStepStreams> & rec,
                                                std::vector<ChainCoverage::Final> & finalSteps, StreamSet * out) {
    // The streams of the final stars are made for the coverage (finalSteps
    // given), or for the starts when an occurrence of the first is required.
    // (A kernel whose output is not used would be removed from the pipeline.)
    const bool coverage = (out == nullptr);
    for (auto f = form.finals.rbegin(); f != form.finals.rend(); ++f) {
        if (!coverage && !(form.occurrenceRequired && finalSteps.empty())) break;
        StreamSet * strm = nullptr;
        if (f->kind == 's') {
            strm = classStream(f->re, f->cc);
        } else {
            strm = mPB.CreateStreamSet(3);
            mPB.CreateKernelCall<StringClassKernel>(codeUnitStrings(f->strings), codeUnitBasis(), strm, mCtxt.mMatchFollows);
        }
        finalSteps.push_back(ChainCoverage::Final{f->kind, strm});
    }
    // The starts of the occurrences of the first final star, if one is
    // required, or where the lookaheads after the chain hold (compiled as a
    // zero-width RE, whose matches are marked at the positions after which
    // they hold, as are the starts).
    StreamSet * Hend = nullptr;
    if (!form.afterChain.empty()) {
        Hend = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, makeSeq(form.afterChain.begin(), form.afterChain.end()), Hend);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("afterChainHolds", Hend);
        }
    } else if (form.occurrenceRequired) {
        Hend = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<StreamSelect>(Hend, Select(finalSteps.front().strm, {1}));
    }
    StreamSet * starts = form.segments.empty() ? Hend : chainMatchStarts(form.segments, &rec, Hend);
    if (!form.lookbehinds.empty()) {
        StreamSet * const holds = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, makeSeq(form.lookbehinds.begin(), form.lookbehinds.end()), holds);
        StreamSet * const restricted = out ? out : mPB.CreateStreamSet(1);
        AndCombine(mPB, starts, holds, restricted);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("chainStarts", starts);
            mPB.captureBitstream("lookbehindHolds", holds);
        }
        starts = restricted;
    }
    if (out && (starts != out)) {
        mPB.CreateKernelCall<StreamSelect>(out, Select(starts, {0}));
        starts = out;
    }
    return starts;
}

bool RE_PipelineBuilder::chainSpans(RE * re, StreamSet * spans) {
    // Chains are compiled with one position per code unit.
    if (mCtxt.mIndexStream != nullptr) return false;
    ChainForm form;
    if (!parseChainForm(re, mCtxt.mLengthAlphabet, form)) return false;
    std::vector<ChainStepStreams> rec;
    std::vector<ChainCoverage::Final> finalSteps;
    StreamSet * const starts = chainFormStarts(form, rec, finalSteps);
    const std::vector<LookaheadSegment> & segments = form.segments;
    std::vector<ChainCoverage::Step> steps;
    for (unsigned i = 0; i < segments.size(); ++i) {
        const LookaheadSegment & seg = segments[i];
        ChainCoverage::Step step{'z', 0, rec[i].Hnext, rec[i].cls, rec[i].fse, rec[i].index, rec[i].goodNext};
        if (seg.end || seg.assertion) {
            step.kind = 'z';
        } else if (!seg.star) {
            step.kind = 'f';
            step.n = static_cast<unsigned>(seg.length);
        } else {
            step.kind = seg.strings.empty() ? 's' : 'c';
            step.n = static_cast<unsigned>(seg.lb);
        }
        steps.push_back(step);
    }
    mPB.CreateKernelCall<ChainCoverage>(starts, steps, finalSteps, mCtxt.mMatchFollows, spans);
    return true;
}

//
// The strategies for the starts of the matches of an alternative of an RE as
// compiled (see RE_PipelineBuilder::matchStartsPipeline):
//   FixedLength:  the ends of the matches, shifted back by the length (and
//                 the offset of the marks after the ends);
//   UniquePrefix: after any lookbehinds at its start, a prefix of fixed length
//                 that can match nowhere else within a match (ParseUniquePrefix),
//                 followed by a suffix that is not nullable: the matches of the
//                 prefix that begin matches, restricted to where the
//                 lookbehinds hold;
//   LengthClasses: a bounded RE, as the alternation of its matches of each
//                 length (lengthClasses), each of fixed length;
//   Chain:        a lookahead chain with final stars (parseChainForm), with
//                 Unicode code units.
//
enum class StartsStrategy {FixedLength, UniquePrefix, LengthClasses, Chain};

struct MatchStartsAlternative {
    RE * re = nullptr;
    StartsStrategy strategy = StartsStrategy::FixedLength;
    std::vector<RE *> lookbehinds;      // UniquePrefix
    RE * prefix = nullptr;              // UniquePrefix
    RE * suffix = nullptr;              // UniquePrefix
    std::map<int, RE *> classes;        // LengthClasses: by length
    ChainForm chain;                    // Chain
};

static bool startsStrategy(RE * re, const cc::Alphabet * lengthAlphabet, const cc::Alphabet * codeUnitAlphabet,
                           MatchStartsAlternative & a) {
    a.re = re;
    const auto range = getLengthRange(re, lengthAlphabet);
    if ((range.first == range.second) && (range.first != INT_MAX)) {
        a.strategy = StartsStrategy::FixedLength;
        return true;
    }
    std::vector<RE *> elems;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        elems.assign(seq->begin(), seq->end());
    } else {
        elems.push_back(re);
    }
    const auto lookbehindEnd = std::find_if_not(elems.begin(), elems.end(), isLookbehind);
    RE * prefix, * suffix;
    std::tie(prefix, suffix) = ParseUniquePrefix(makeSeq(lookbehindEnd, elems.end()));
    if (!isEmptySeq(prefix) && !isEmptySeq(suffix) && !isNullable(suffix)) {
        const auto pfxRange = getLengthRange(prefix, lengthAlphabet);
        if ((pfxRange.first == pfxRange.second) && (pfxRange.first > 0)) {
            a.strategy = StartsStrategy::UniquePrefix;
            a.lookbehinds.assign(elems.begin(), lookbehindEnd);
            a.prefix = prefix;
            a.suffix = suffix;
            return true;
        }
    }
    if (lengthClasses(expandBoundedReps(re, lengthAlphabet), lengthAlphabet, LengthClassLimit, a.classes)) {
        a.strategy = StartsStrategy::LengthClasses;
        return true;
    }
    if ((codeUnitAlphabet == &cc::Unicode) && parseChainForm(re, lengthAlphabet, a.chain)) {
        a.strategy = StartsStrategy::Chain;
        return true;
    }
    return false;
}

// The strategies of the alternatives of an RE as compiled, and the
// alternatives that have none.  An alternative with none is split by promoting
// its variable-length alternations (as for spans, see spanFactoring), if each
// of the alternatives so formed has one.
static void planMatchStarts(RE * re, const cc::Alphabet * lengthAlphabet, const cc::Alphabet * codeUnitAlphabet,
                            std::vector<MatchStartsAlternative> & plan, std::vector<RE *> & unsupported) {
    std::vector<RE *> alts;
    if (Alt * const alt = dyn_cast<Alt>(re)) {
        alts.assign(alt->begin(), alt->end());
    } else {
        alts.push_back(re);
    }
    if (alts.empty()) {
        // Matching nothing: no starts, as for a fixed-length RE.
        MatchStartsAlternative a;
        a.re = re;
        plan.push_back(a);
        return;
    }
    const size_t limit = 32;
    for (RE * r : alts) {
        MatchStartsAlternative a;
        if (startsStrategy(r, lengthAlphabet, codeUnitAlphabet, a)) {
            plan.push_back(std::move(a));
            continue;
        }
        RE * promoted = r;
        for (unsigned pass = 0; pass < 4; ++pass) {
            RE * const next = variableAltPromotion(promoted, lengthAlphabet);
            if ((next == promoted) || (isa<Alt>(next) && (cast<Alt>(next)->size() > limit))) break;
            promoted = next;
        }
        std::vector<MatchStartsAlternative> split;
        bool supported = isa<Alt>(promoted);
        if (supported) {
            for (RE * p : *cast<Alt>(promoted)) {
                MatchStartsAlternative pa;
                if (!startsStrategy(p, lengthAlphabet, codeUnitAlphabet, pa)) {
                    supported = false;
                    break;
                }
                split.push_back(std::move(pa));
            }
        }
        if (supported) {
            for (auto & pa : split) plan.push_back(std::move(pa));
        } else {
            unsupported.push_back(r);
        }
    }
}

void RE_PipelineBuilder::matchStartsPipeline(RE * re, StreamSet * starts) {
    RE_ModeOptions opts = mModeOptions;
    opts.goal = RE_Goal::MatchStarts;
    if (mHaveSourceContext) {
        matchStartsPipeline(::prepareRE(re, opts), starts);
        return;
    }
    // A hand-built RE_CompilerContext: the RE is compiled as given.
    matchStartsPipeline(PreparedRE{re, RE_Mode{nullptr, nullptr}, opts}, starts);
}

static std::vector<PreconditionFailure> modeFailures(const PreparedRE & prepared, const cc::Alphabet * sourceEncoding,
                                                     bool & byReversal);

namespace {
RE * nameForwardLookbehinds(RE * re, std::vector<std::pair<Name *, Assertion *>> & lookbehinds);
}

void RE_PipelineBuilder::matchStartsPipeline(const PreparedRE & prepared, StreamSet * starts) {
    if (prepared.options.goal != RE_Goal::MatchStarts) {
        llvm::report_fatal_error("matchStartsPipeline: the RE must be prepared with the MatchStarts goal");
    }
    usePrepared(prepared);
    RE * re = prepared.re;
    // The starts found by reversal, when no forward strategy applies (decided
    // before the mode passes, which make the kernels of the forward strategies).
    bool byReversal = false;
    if (mHaveSourceContext) modeFailures(prepared, mSourceContext.encoding, byReversal);
    if (byReversal) {
        // The text is extended as the reversed RE needs it (a negative
        // lookbehind of the RE is a negative lookahead of the reversed RE).
        std::vector<std::pair<Name *, Assertion *>> lookbehinds;
        if (hasEndAnchorAnywhere(reverse(nameForwardLookbehinds(re, lookbehinds)))) mExtendText = true;
        ensurePrepared(re);
        reversedStarts(re, starts);
        return;
    }
    ensurePrepared(re);
    mRE = applyModePasses(re, RE_Goal::MatchStarts);
    mRE = processReferences(mRE);
    prepareExternals(mRE);
    compileStarts(mRE, starts);
}

StreamSet * RE_PipelineBuilder::reversalBarriers(RE * re) {
    // The characters that no match of the RE (or assertion within it) can reach.
    UCD::UnicodeSet matchable;
    re::CC * barrierChars = re::makeCC(&cc::Unicode);
    if (matchableCodepoints(re, matchable)) {
        barrierChars = re::makeCC(UCD::UnicodeSet(0, UCD::UNICODE_MAX) - matchable, &cc::Unicode);
    }
    StreamSet * barriers = mPB.CreateStreamSet(1);
    compileClasses(std::vector<re::CC *>{barrierChars}, barriers, mCtxt.mCodeUnitStream);
    if (mCtxt.mMatchFollows) {
        StreamSet * const all = mPB.CreateStreamSet(1);
        OrCombine(mPB, barriers, mCtxt.mMatchFollows, all);
        barriers = all;
    }
    return barriers;
}

namespace {
// The positions of a text, without the position it is extended by (see
// ExtendByOne): a mask of one more position than the source, all but the
// last set.
class TextPositionsMask final : public pablo::PabloKernel {
public:
    TextPositionsMask(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * Mask)
    : PabloKernel(ts, "TextPositionsMask", {Binding{"Source", Source}}, {Binding{"Mask", Mask, FixedRate(), Add1()}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        writeOutputStreamSet("Mask", std::vector<PabloAST *>{pb.createNot(pb.createAtEOF(pb.createOnes()))});
    }
};

// The names of the anchors of a reversed RE (see ReversedAnchorNamer).
const std::string ReversedStartName = "^rev";
const std::string ReversedEndName = "$rev";

// The anchors of a reversed RE, matched in the regions reversed with
// barriers other than the region follows (see RE_PipelineBuilder::
// reversedStarts), named as zero-width externals: where its Start and End
// hold is not at the region starts and follows of the text.
struct ReversedAnchorNamer final : public RE_Transformer {
    ReversedAnchorNamer() : RE_Transformer("ReversedAnchorNamer") {}
    RE * transformStart(Start * s) override {
        mStart = true;
        return makeName(ReversedStartName, s);
    }
    RE * transformEnd(End * e) override {
        mEnd = true;
        return makeName(ReversedEndName, e);
    }
    bool mStart = false;
    bool mEnd = false;
};

// The positions to reverse for the anchors of a reversed RE, given the
// region starts and follows of the text and the barriers of the reversal:
// the last character of each region (before its follow), and the first
// character of each region, where these are not barriers.
class ForwardAnchorMarks final : public pablo::PabloKernel {
public:
    ForwardAnchorMarks(LLVMTypeSystemInterface & ts, StreamSet * Starts, StreamSet * Follows, StreamSet * Barriers,
                       StreamSet * Marks)
    : PabloKernel(ts, "ForwardAnchorMarks",
                  {Binding{"Starts", Starts}, Binding{"Follows", Follows, FixedRate(1), LookAhead(1)},
                   Binding{"Barriers", Barriers}},
                  {Binding{"Marks", Marks}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const notBarrier = pb.createNot(getInputStreamSet("Barriers")[0]);
        PabloAST * const last = pb.createAnd(pb.createLookahead(getInputStreamSet("Follows")[0], 1), notBarrier);
        PabloAST * const first = pb.createAnd(getInputStreamSet("Starts")[0], notBarrier);
        writeOutputStreamSet("Marks", std::vector<PabloAST *>{last, first});
    }
};

// Where the anchors of a reversed RE hold in the reversed regions, given the
// marks of ForwardAnchorMarks reversed (Reversed), and the region starts,
// follows and barriers.  The End of a region of the text, at its follow f,
// is the start of the region reversed that ends at f, which is that of the
// reversal of its last character (or f, if the region reversed is empty: f
// at a barrier, or at the start of the text).  The Start of a region of the
// text, at its start s, is the follow of the region reversed that begins at
// s: one after the reversal of its first character (or s, if s is a barrier).
class ReversedAnchors final : public pablo::PabloKernel {
public:
    ReversedAnchors(LLVMTypeSystemInterface & ts, StreamSet * Reversed, StreamSet * Starts, StreamSet * Follows,
                    StreamSet * Barriers, StreamSet * ReversedStart, StreamSet * ReversedEnd)
    : PabloKernel(ts, "ReversedAnchors",
                  {Binding{"Reversed", Reversed}, Binding{"Starts", Starts}, Binding{"Follows", Follows},
                   Binding{"Barriers", Barriers}},
                  {Binding{"ReversedStart", ReversedStart}, Binding{"ReversedEnd", ReversedEnd}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> reversed = getInputStreamSet("Reversed");
        PabloAST * const barriers = getInputStreamSet("Barriers")[0];
        PabloAST * const afterBarrier = pb.createNot(pb.createAdvance(pb.createNot(barriers), 1));
        PabloAST * const emptyAtFollow = pb.createAnd(getInputStreamSet("Follows")[0], afterBarrier);
        PabloAST * const atStartBarrier = pb.createAnd(getInputStreamSet("Starts")[0], barriers);
        writeOutputStreamSet("ReversedStart", std::vector<PabloAST *>{pb.createOr(reversed[0], emptyAtFollow)});
        writeOutputStreamSet("ReversedEnd", std::vector<PabloAST *>{pb.createOr(pb.createAdvance(reversed[1], 1),
                                                                                 atStartBarrier)});
    }
};
}

void RE_PipelineBuilder::reversedStarts(RE * re, StreamSet * starts) {
    if (mCtxt.mCodeUnitAlphabet != &cc::Unicode) {
        llvm::report_fatal_error("the starts of matches by reversal require full Unicode indexing");
    }
    StreamSet * const barriers = reversalBarriers(re);
    RE_ModeOptions opts;
    opts.goal = RE_Goal::MatchEnds;
    opts.forceUnicodeIndexing = true;
    // Lookbehinds matched forward (see ForwardLookbehindNamer): the ends of
    // their asserted REs, at their last characters, reversed, mark the
    // characters where their lookaheads in the reversed RE hold.
    std::vector<std::pair<Name *, Assertion *>> lookbehinds;
    RE * const named = nameForwardLookbehinds(re, lookbehinds);
    PreparedRE reversed = prepareRE(reverse(named), opts);
    // Without match regions, the reversed text is extended by one position
    // (and made one region) if the reversed RE refers to its end: for a
    // negative lookahead (a negative lookbehind of the RE), which holds at
    // the end, and for the matches whose ends are then marked there (see
    // ensurePrepared).  The externals given to it are extended with it, and
    // the extension is removed from its results.
    const bool extended = (mCtxt.mMatchStarts == nullptr) && hasEndAnchorAnywhere(reversed.re);
    auto extendedExternal = [&](StreamSet * s) {
        if (!extended) return s;
        StreamSet * const x = mPB.CreateStreamSet(s->getNumElements(), 1);
        mPB.CreateKernelCall<ExtendByOne>(s, x);
        return x;
    };
    // The streams reversed, with the same barriers, by one RegionReversal:
    // the code units, the marks of the anchors (if the reversed RE has any)
    // and the ends of the lookbehinds matched forward.
    std::vector<StreamSet *> forwardStreams{mCtxt.mCodeUnitStream};
    // The anchors of the reversed RE: the Start and End of the regions of
    // the text reversed (the barriers that are not region follows divide
    // them), as zero-width externals.  (A text without regions is extended,
    // and given its own region, for the anchors, see matchStartsPipeline.)
    const bool anchored = hasAnchorAnywhere(reversed.re);
    if (anchored) {
        if ((mCtxt.mMatchStarts == nullptr) || (mCtxt.mMatchFollows == nullptr)) {
            llvm::report_fatal_error("the starts of matches by reversal: anchors require match regions");
        }
        StreamSet * const marks = mPB.CreateStreamSet(2);
        mPB.CreateKernelCall<ForwardAnchorMarks>(mCtxt.mMatchStarts, mCtxt.mMatchFollows, barriers, marks);
        forwardStreams.push_back(marks);
    }
    for (const auto & lb : lookbehinds) {
        const PreparedRE prepared = prepareRE(lb.second->getAsserted(), opts);
        RE_PipelineBuilder forward(mPB, RE_context{codeUnitEncoding(), mCtxt.mCodeUnitStream, mCtxt.mMatchStarts,
                                                   mCtxt.mMatchFollows});
        StreamSet * lbEnds = mPB.CreateStreamSet(1);
        forward.matchSearchPipeline(prepared, lbEnds);
        if (const unsigned offset = prepared.grepOffset()) {
            StreamSet * const shifted = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<ShiftBack>(lbEnds, shifted, offset);
            lbEnds = shifted;
        }
        forwardStreams.push_back(lbEnds);
    }
    // Reversed, and taken apart again in the order given.
    std::vector<StreamSet *> reversedStreams;
    if (forwardStreams.size() == 1) {
        StreamSet * const reversedText = mPB.CreateStreamSet(mCtxt.mCodeUnitStream->getNumElements());
        mPB.CreateKernelCall<RegionReversal>(barriers, mCtxt.mCodeUnitStream, reversedText, ReverseRegionLimit);
        reversedStreams.push_back(reversedText);
    } else {
        StreamSet * const all = streamutils::Select(mPB, forwardStreams);
        StreamSet * const reversedAll = mPB.CreateStreamSet(all->getNumElements());
        mPB.CreateKernelCall<RegionReversal>(barriers, all, reversedAll, ReverseRegionLimit);
        uint32_t first = 0;
        for (StreamSet * f : forwardStreams) {
            std::vector<uint32_t> indices;
            for (uint32_t i = 0; i < f->getNumElements(); i++) indices.push_back(first + i);
            reversedStreams.push_back(streamutils::Select(mPB, reversedAll, indices));
            first += f->getNumElements();
        }
    }
    StreamSet * const reversedText = reversedStreams[0];
    RE_PipelineBuilder backward(mPB, RE_context{codeUnitEncoding(), reversedText, mCtxt.mMatchStarts, mCtxt.mMatchFollows});
    unsigned next = 1;
    if (anchored) {
        StreamSet * const reversedMarks = reversedStreams[next++];
        StreamSet * const reversedStart = mPB.CreateStreamSet(1);
        StreamSet * const reversedEnd = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<ReversedAnchors>(reversedMarks, mCtxt.mMatchStarts, mCtxt.mMatchFollows, barriers,
                                              reversedStart, reversedEnd);
        ReversedAnchorNamer anchors;
        reversed.re = anchors.transformRE(reversed.re);
        if (anchors.mStart) {
            backward.addExternal(ReversedStartName, ExternalStream{ExternalStreamKind::ZeroWidth, 1, {0, 0}, reversedStart});
        }
        if (anchors.mEnd) {
            backward.addExternal(ReversedEndName, ExternalStream{ExternalStreamKind::ZeroWidth, 1, {0, 0}, reversedEnd});
        }
    }
    for (const auto & lb : lookbehinds) {
        RE * const asserted = lb.second->getAsserted();
        StreamSet * const marks = extendedExternal(reversedStreams[next++]);
        backward.addExternal(lb.first->getFullName(),
                             ExternalStream{ExternalStreamKind::StartIndexed, 1, getLengthRange(asserted, &cc::Unicode),
                                            marks, lb.second->getSense() == Assertion::Sense::Negative});
    }
    StreamSet * ends = mPB.CreateStreamSet(1);
    backward.matchSearchPipeline(reversed, ends);
    assert(backward.mTextExtended == extended);
    if (const unsigned offset = reversed.grepOffset()) {
        StreamSet * const shifted = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<ShiftBack>(ends, shifted, offset);
        ends = shifted;
    }
    if (extended) {
        // (A match marked at the extension, past the last character, is at
        // that character once shifted back by the offset.)
        StreamSet * const mask = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<TextPositionsMask>(reversedText, mask);
        StreamSet * const inText = mPB.CreateStreamSet(1);
        FilterByMask(mPB, mask, ends, inText);
        ends = inText;
    }
    mPB.CreateKernelCall<RegionReversal>(barriers, ends, starts, ReverseRegionLimit);
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        mPB.captureBitstream("reversalBarriers", barriers);
        mPB.captureBitstream("reversedEnds", ends);
    }
}

void RE_PipelineBuilder::compileStarts(RE * re, StreamSet * starts) {
    std::vector<MatchStartsAlternative> plan;
    std::vector<RE *> unsupported;
    planMatchStarts(re, mCtxt.mLengthAlphabet, mCtxt.mCodeUnitAlphabet, plan, unsupported);
    // (The preconditions have been checked by applyModePasses.)
    assert(unsupported.empty());
    if (plan.size() == 1) {
        alternativeStarts(plan.front(), starts);
        return;
    }
    std::vector<StreamSet *> allStarts;
    for (const MatchStartsAlternative & a : plan) {
        StreamSet * const s = mPB.CreateStreamSet(1);
        alternativeStarts(a, s);
        allStarts.push_back(s);
    }
    mPB.CreateKernelCall<StreamsMerge>(allStarts, starts);
}

void RE_PipelineBuilder::fixedLengthStarts(RE * re, int length, StreamSet * out) {
    // A match of length L marked at its last code unit plus the offset g
    // begins L - 1 + g positions before the mark.
    const int shift = std::max(0, length - 1 + static_cast<int>(grepOffset(re)));
    if (shift == 0) {
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, re, out);
    } else {
        StreamSet * const ends = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, re, ends);
        mPB.CreateKernelCall<ShiftBack>(ends, out, shift);
    }
}

void RE_PipelineBuilder::alternativeStarts(const MatchStartsAlternative & a, StreamSet * out) {
    // Code units are positions: the starts are marked in code units.
    assert(mCtxt.mIndexStream == nullptr);
    if (a.strategy == StartsStrategy::FixedLength) {
        fixedLengthStarts(a.re, getLengthRange(a.re, mCtxt.mLengthAlphabet).first, out);
    } else if (a.strategy == StartsStrategy::LengthClasses) {
        std::vector<StreamSet *> classStarts;
        for (const auto & c : a.classes) {
            StreamSet * const s = (a.classes.size() == 1) ? out : mPB.CreateStreamSet(1);
            fixedLengthStarts(c.second, c.first, s);
            classStarts.push_back(s);
        }
        if (classStarts.size() > 1) {
            mPB.CreateKernelCall<StreamsMerge>(classStarts, out);
        }
    } else if (a.strategy == StartsStrategy::UniquePrefix) {
        // The ends of the matches, shifted back to the preceding match of the
        // prefix (see uniquePrefixEndsBack), which begins them, then back by
        // the length of the prefix (and the offset of its marks).
        Name * const pfxName = makeUniquePrefixName(a.prefix);
        compileExternal(pfxName);
        auto fp = mCtxt.mExternals.find(pfxName->getFullName());
        assert(fp != mCtxt.mExternals.end());
        StreamSet * const pfxStrm = fp->second.extStream;
        const int shift = fp->second.lgthRange.first - 1 + static_cast<int>(fp->second.offset);
        StreamSet * const matchEnd = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, makeSeq({pfxName, a.suffix}), matchEnd);
        StreamSet * const endsBack = uniquePrefixEndsBack(pfxStrm, matchEnd);
        const bool last = (shift <= 0) && a.lookbehinds.empty();
        StreamSet * const atPrefix = last ? out : mPB.CreateStreamSet(1);
        AndCombine(mPB, pfxStrm, endsBack, atPrefix);
        StreamSet * starts = atPrefix;
        if (shift > 0) {
            starts = a.lookbehinds.empty() ? out : mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<ShiftBack>(atPrefix, starts, shift);
        }
        if (!a.lookbehinds.empty()) {
            // The lookbehinds, compiled as a zero-width RE, are marked at the
            // positions where they hold.
            StreamSet * const holds = mPB.CreateStreamSet(1);
            mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, makeSeq(a.lookbehinds.begin(), a.lookbehinds.end()), holds);
            AndCombine(mPB, starts, holds, out);
        }
    } else {
        std::vector<ChainStepStreams> rec;
        std::vector<ChainCoverage::Final> finalSteps;
        chainFormStarts(a.chain, rec, finalSteps, out);
    }
}

namespace {
// Expand each bounded repetition X{m,n} with a small range (m < n) into the
// alternation of X{m}, ..., X{n}, where X{k} is written out as k copies of
// X if X has a variable length (so that its alternations may be promoted).
// For such an X, X{m,} (1 < m <= 4) is written out as m - 1 copies of X
// followed by X+.
class BoundedRepExpander final : public RE_Transformer {
public:
    BoundedRepExpander(const cc::Alphabet * lengthAlphabet) : RE_Transformer("BoundedRepExpander"),
        mAlphabet(lengthAlphabet) {}
protected:
    RE * transformRep(Rep * rep) override {
        RE * const x = transform(rep->getRE());
        const int lb = rep->getLB();
        const int ub = rep->getUB();
        const auto range = getLengthRange(x, mAlphabet);
        if ((ub == Rep::UNBOUNDED_REP) && (lb >= 2) && (lb <= 4) && (range.first != range.second)) {
            // X{lb,} is lb - 1 copies of X followed by X+.
            std::vector<RE *> seq(lb - 1, x);
            seq.push_back(makeRep(x, 1, Rep::UNBOUNDED_REP));
            return makeSeq(seq.begin(), seq.end());
        }
        if ((ub == Rep::UNBOUNDED_REP) || (ub > 4) || ((lb != ub) && (ub - lb > 3))) {
            return (x == rep->getRE()) ? rep : makeRep(x, lb, ub);
        }
        auto copies = [&](int k) -> RE * {
            if (range.first == range.second) return makeRep(x, k, k);
            std::vector<RE *> seq(k, x);
            return makeSeq(seq.begin(), seq.end());
        };
        if (lb == ub) {
            return copies(lb);
        }
        std::vector<RE *> alts;
        for (int k = lb; k <= ub; ++k) alts.push_back(copies(k));
        return makeAlt(alts.begin(), alts.end());
    }
    RE * transformAssertion(Assertion * a) override {
        return a;
    }
private:
    const cc::Alphabet * mAlphabet;
};

RE * expandBoundedReps(RE * re, const cc::Alphabet * lengthAlphabet) {
    return BoundedRepExpander(lengthAlphabet).transformRE(re);
}
}

namespace {
// Replace the names of the given externals by their definitions.
class ExternalDefinitionSubstitution final : public RE_Transformer {
public:
    ExternalDefinitionSubstitution(const ExternalNameMap & externals, ExternalStreamKind kind)
    : RE_Transformer("ExternalDefinitionSubstitution"), mExternals(externals), mKind(kind) {}
protected:
    RE * transformName(Name * n) override {
        auto f = mExternals.find(n->getFullName());
        if ((f != mExternals.end()) && (f->second.kind == mKind) && n->getDefinition()) {
            return transform(n->getDefinition());
        }
        return n;
    }
private:
    const ExternalNameMap & mExternals;
    const ExternalStreamKind mKind;
};
}

RE * RE_PipelineBuilder::spanFactoring(RE * re) {
    // The repetitions of string classes (named by StringClassRepNamer for the
    // matches) are factored as written: their spans are computed by chainSpans.
    re = ExternalDefinitionSubstitution(mCtxt.mExternals, ExternalStreamKind::StringClassRep).transformRE(re);
    // The empty matches are eliminated, and variable-length alternations
    // (including small bounded repetitions) are promoted to the top level,
    // so that the alternatives are fixed-length, have a unique prefix or
    // are chains (see chainSpans), within a limit on their number.
    // (Promotion distributes the alternations directly within sequences; it
    // is repeated for those it exposes.)
    RE * const nonempty = emptyMatchElimination(re);
    const size_t limit = 32;
    auto alternatives = [](RE * r) -> size_t {
        if (const Alt * alt = dyn_cast<Alt>(r)) return alt->size();
        return 1;
    };
    RE * promoted = BoundedRepExpander(mCtxt.mLengthAlphabet).transformRE(nonempty);
    for (unsigned pass = 0; pass < 4; ++pass) {
        RE * const next = variableAltPromotion(promoted, mCtxt.mLengthAlphabet);
        if ((next == promoted) || (alternatives(next) > limit)) break;
        promoted = next;
    }
    promoted = emptyMatchElimination(promoted);
    if (alternatives(promoted) > limit) promoted = nonempty;
    re::FixedSpanNamer FLnamer(mCtxt.mCodeUnitAlphabet);
    RE * xfrmedRE = FLnamer.transformRE(promoted);
    //re::Repeated_CC_Seq_Namer RCCSnamer;
    //xfrmedRE = mRCCSnamer.transformRE(xfrmedRE);
    return xfrmedRE;
}

namespace {
// Why the starts of the matches of an RE (prepared with the MatchStarts goal)
// cannot be found by reversal (see RE_PipelineBuilder::reversedStarts), or ""
// if they can: as the ends of the matches of the reversed RE in the reversed
// regions, compiled for the MatchEnds goal, with full Unicode indexing (each
// position a character).  The RE must not match the empty string (an empty
// match is between positions, which reversal does not map to a start).  Its
// anchors are those of the regions of the text, reversed (see
// ReversedAnchors), which requires match regions: unless extensible (the
// text of the reversal may be extended and given its own region, see
// matchStartsPipeline), the RE must not have a Start anchor (an End anchor
// of the reversed RE, for which the text would not be extended).  (A
// reversed RE that refers to the end of the text otherwise, by a negative
// lookahead, extends the reversed text alone, see reversedStarts.)
std::string reversalFailure(RE * re, const cc::Alphabet * codeUnitAlphabet, bool extensible = true) {
    if (codeUnitAlphabet != &cc::Unicode) return "it requires full Unicode indexing";
    if (getLengthRange(re, &cc::Unicode).first == 0) return "the RE can match the empty string";
    std::vector<std::pair<Name *, Assertion *>> lookbehinds;
    RE * const named = nameForwardLookbehinds(re, lookbehinds);
    if (!extensible && hasEndAnywhere(reverse(named))) {
        return "the reversed RE has an End anchor (a Start anchor of the RE), which requires the text to be "
               "extended, and given its own region";
    }
    RE_ModeOptions opts;
    opts.goal = RE_Goal::MatchEnds;
    opts.forceUnicodeIndexing = true;
    const std::vector<PreconditionFailure> failures = checkPreconditions(prepareRE(reverse(named), opts), &cc::Unicode);
    if (failures.empty()) return "";
    std::string reason = "the reversed RE: ";
    for (const PreconditionFailure & f : failures) reason += f.reason + "; ";
    reason.resize(reason.size() - 2);
    return reason;
}

// The lookaheads of an RE that no forward strategy compiles, and whose starts
// reversal finds (see reversalFailure, without extending the text), named
// ReversedLookaheadPrefix + a hash, with the lookahead as its definition: the
// external is computed by RE_PipelineBuilder::reversedStarts (see
// compileExternal).  Lookaheads within them are left to the reversed RE.
struct ReversedLookaheadNamer final : public NameIntroduction {
    ReversedLookaheadNamer(const cc::Alphabet * lengthAlphabet, const cc::Alphabet * codeUnitAlphabet)
    : NameIntroduction("ReversedLookaheadNamer"), mLengthAlphabet(lengthAlphabet), mCodeUnitAlphabet(codeUnitAlphabet) {}
    RE * transformAssertion(Assertion * a) override;
    const cc::Alphabet * const mLengthAlphabet;
    const cc::Alphabet * const mCodeUnitAlphabet;
    // The lookaheads without a forward strategy that reversal does not
    // match either, with the reasons.
    std::vector<std::pair<Assertion *, std::string>> mNotReversed;
};

// The precondition failures of the forward compilation of an RE (no reversal).
std::vector<PreconditionFailure> forwardFailures(RE * re, const cc::Alphabet * lengthAlphabet,
                                                 const cc::Alphabet * codeUnitAlphabet);

// The lookbehinds of an RE whose starts are found by reversal (see
// RE_PipelineBuilder::reversedStarts) that are matched forward, as externals
// of the backward pass.  In the reversed RE, a lookbehind (?<=B) is the
// lookahead (?=reverse(B)), which holds before the characters (of the
// reversed text) that follow an end of B in the text: the ends of B, marked
// at their last characters and reversed, mark them (a StartIndexed external).
// The lookbehinds named are those whose lookaheads in the reversed RE have no
// forward strategy, where B is not nullable and is compiled forward, without
// extending the text.  The names (ForwardLookbehindPrefix + a hash) are
// defined by the lookaheads of the reversed RE (so that a negative one still
// refers to the end of the text, which the reversed regions need for its
// matches at their ends, see matchStartsPipeline).
struct ForwardLookbehindNamer final : public NameIntroduction {
    ForwardLookbehindNamer() : NameIntroduction("ForwardLookbehindNamer") {}
    RE * transformAssertion(Assertion * a) override;
    std::vector<std::pair<Name *, Assertion *>> mLookbehinds;
};

RE * ForwardLookbehindNamer::transformAssertion(Assertion * a) {
    if (a->getKind() == Assertion::Kind::LookBehind) {
        RE * const asserted = a->getAsserted();
        RE * const lookahead = makeAssertion(reverse(asserted), Assertion::Kind::LookAhead, a->getSense());
        if ((getLengthRange(asserted, &cc::Unicode).first > 0) && !hasEndAnchorAnywhere(asserted) &&
            !forwardFailures(lookahead, &cc::Unicode, &cc::Unicode).empty() &&
            forwardFailures(asserted, &cc::Unicode, &cc::Unicode).empty()) {
            const std::string name = ForwardLookbehindPrefix + kernel::Kernel::getStringHash(Printer_RE::PrintRE(a));
            const bool named = mNameMap.count(name) != 0;
            Name * const n = createName(name, lookahead);
            if (!named) mLookbehinds.emplace_back(n, a);
            return n;
        }
    }
    return RE_Transformer::transformAssertion(a);
}

// The RE with the lookbehinds matched forward named (see
// ForwardLookbehindNamer), and these lookbehinds.
RE * nameForwardLookbehinds(RE * re, std::vector<std::pair<Name *, Assertion *>> & lookbehinds) {
    ForwardLookbehindNamer namer;
    RE * const named = namer.transformRE(re);
    lookbehinds = std::move(namer.mLookbehinds);
    return named;
}

RE * ReversedLookaheadNamer::transformAssertion(Assertion * a) {
    if ((a->getKind() == Assertion::Kind::LookAhead) && !forwardFailures(a, mLengthAlphabet, mCodeUnitAlphabet).empty()) {
        const std::string reason = reversalFailure(a->getAsserted(), mCodeUnitAlphabet, false);
        if (reason.empty()) {
            return createName(ReversedLookaheadPrefix + kernel::Kernel::getStringHash(Printer_RE::PrintRE(a)), a);
        }
        mNotReversed.emplace_back(a, reason);
    }
    return RE_Transformer::transformAssertion(a);
}

// The RE transformations of the mode passes (see
// RE_PipelineBuilder::applyModePasses) for the length and code unit alphabets
// of a mode, apart from the kernels of the names that they introduce, which
// the namers record.
struct ModeTransformation {
    ModeTransformation(const cc::Alphabet * lengthAlphabet, const cc::Alphabet * codeUnitAlphabet)
    : mLengthAlphabet(lengthAlphabet), mCodeUnitAlphabet(codeUnitAlphabet),
      SCnamer(codeUnitAlphabet), LA(*lengthAlphabet) {}

    RE * transform(RE * re, RE_Goal goal);

    // The preconditions of the transformed RE (see checkPreconditions).
    std::vector<PreconditionFailure> failures(RE * transformed, RE_Goal goal);

    const cc::Alphabet * const mLengthAlphabet;
    const cc::Alphabet * const mCodeUnitAlphabet;
    // The RE before the transformations, and whether the starts of its
    // matches (the MatchStarts goal) are found by reversal (see failures).
    RE * mSource = nullptr;
    // Whether lookaheads (and, if mAcceptStartsByReversal, the starts of the
    // matches) may be matched by reversal (see RE_ModeOptions::reversal).
    bool mAllowReversal = true;
    bool mAcceptStartsByReversal = true;
    bool mStartsByReversal = false;
    std::vector<std::pair<Assertion *, std::string>> mLookaheadsNotReversed;
    UCD::PropertyExternalizer PE;
    // (CCs of U+FFFD are named too: they also match ill-formed input.)
    re::VariableLengthCCNamer CCnamer{8, replacedUTF8Errors()};
    re::StringClassRepNamer SCnamer;
    re::LookAheadNamer LA;
};

RE * ModeTransformation::transform(RE * re, RE_Goal goal) {
    // The captures of a search and replace that only the replacement refers
    // to are found from its matches (see re::CaptureWalk): the matches are
    // computed without them.
    mSource = re;
    if (goal == RE_Goal::SearchReplace) re = removeUnneededCaptures(re);
    // Lookaheads without a forward strategy, matched by reversal, are named
    // before the passes, which leave the names as they are.
    if (mAllowReversal) {
        ReversedLookaheadNamer RLA(mLengthAlphabet, mCodeUnitAlphabet);
        re = RLA.transformRE(re);
        mLookaheadsNotReversed = std::move(RLA.mNotReversed);
    }
    RE * xfrmedRE = expandPermutes(re);
    xfrmedRE = regular_expression_passes(xfrmedRE, mLengthAlphabet, goal);
    xfrmedRE = PE.transformRE(xfrmedRE);
    // CCs of characters of more than one UTF-8 length are computed as externals
    // when the code units are UTF-8.  (With Unicode code units, every CC is one
    // code unit.)
    if (mCodeUnitAlphabet == &cc::UTF8) {
        xfrmedRE = CCnamer.transformRE(xfrmedRE);
        xfrmedRE = toUTF8(xfrmedRE);
    }
    // Unbounded repetitions of suitable string classes are matched using
    // the Fill, Starts and Ends of the string class.
    xfrmedRE = SCnamer.transformRE(xfrmedRE);
    // Lookahead lengths are measured in the mode's length alphabet.
    xfrmedRE = LA.transformRE(xfrmedRE);
    return xfrmedRE;
}

// The back references of an RE, including those within the definitions of
// its names, other than the names of the references compiled as externals.
struct ReferenceGatherer final : public RE_Inspector {
    ReferenceGatherer(const std::map<std::string, RE *> & compiled)
    : RE_Inspector(NameProcessingMode::ProcessDefinition), mCompiled(compiled) {}
    void inspectName(Name * n) override {
        if (mCompiled.count(n->getFullName()) == 0) RE_Inspector::inspectName(n);
    }
    void inspectReference(Reference * r) override {
        mReferences.push_back(r);
    }
    const std::map<std::string, RE *> & mCompiled;
    std::vector<Reference *> mReferences;
};

std::vector<PreconditionFailure> ModeTransformation::failures(RE * transformed, RE_Goal goal) {
    // The transformations here only check the RE (processReferences applies them).
    QuietREPrinting quiet;
    std::vector<PreconditionFailure> failures;
    // The failures of the forward strategies for the starts of the matches:
    // for the MatchStarts goal, the starts are otherwise found by reversal,
    // if it applies (see reversalFailure).
    std::vector<PreconditionFailure> forward;
    // Lookaheads named as externals (see RE_PipelineBuilder::compileExternal
    // and matchStartPipeline).
    for (const auto & u : LA.mUnsupported) {
        forward.push_back(PreconditionFailure{goal, u.first, u.second});
    }
    for (const auto & u : mLookaheadsNotReversed) {
        forward.push_back(PreconditionFailure{goal, u.first, "and not by reversal: " + u.second});
    }
    if (mCodeUnitAlphabet != &cc::Unicode) {
        for (const auto & m : LA.mNameMap) {
            if (m.first.compare(0, LookaheadChainPrefix.size(), LookaheadChainPrefix) == 0) {
                failures.push_back(PreconditionFailure{goal, m.second,
                                   "a lookahead chain requires Unicode code units (full Unicode indexing)"});
            }
        }
    }
    // Back references (see RE_PipelineBuilder::processReferences): those to a
    // capture of fixed length at a fixed distance are compiled as externals,
    // for enumerated and codepoint properties; others are not supported.
    re::ReferenceInfo refInfo = re::buildReferenceInfo(transformed);
    re::FixedReferenceTransformer FRT(refInfo, *mLengthAlphabet);
    RE * const fixedRefs = FRT.transformRE(transformed);
    for (const auto & m : FRT.mNameMap) {
        re::Reference * ref = cast<re::Reference>(m.second);
        UCD::PropertyObject * propObj = UCD::getPropertyObject(ref->getReferencedProperty());
        if (!isa<UCD::EnumeratedPropertyObject>(propObj) && !isa<UCD::CodePointPropertyObject>(propObj)) {
            failures.push_back(PreconditionFailure{goal, ref,
                               "a property reference must be to an enumerated or codepoint property"});
        }
    }
    ReferenceGatherer refs(FRT.mNameMap);
    refs.inspectRE(fixedRefs);
    for (Reference * r : refs.mReferences) {
        failures.push_back(PreconditionFailure{goal, r,
                           "a back reference must be to a capture of fixed length at a fixed distance"});
    }
    // The starts of the matches of each alternative (see matchStartsPipeline),
    // of the RE as compiled.
    if ((goal == RE_Goal::MatchStarts) || (goal == RE_Goal::SearchReplace)) {
        std::vector<MatchStartsAlternative> plan;
        std::vector<RE *> unsupported;
        planMatchStarts(fixedRefs, mLengthAlphabet, mCodeUnitAlphabet, plan, unsupported);
        for (RE * u : unsupported) {
            forward.push_back(PreconditionFailure{goal, u,
                              "no strategy for the starts of the matches: not of fixed length, without a unique "
                              "prefix of fixed length followed by a suffix that is not nullable, not bounded with "
                              "at most " + std::to_string(LengthClassLimit) + " lengths, and not a lookahead "
                              "chain (with full Unicode indexing)"});
        }
    }
    if (!forward.empty() && (goal == RE_Goal::MatchStarts) && mAllowReversal && mAcceptStartsByReversal) {
        const std::string reason = reversalFailure(mSource, mCodeUnitAlphabet);
        if (reason.empty()) {
            mStartsByReversal = true;
            forward.clear();
        } else {
            forward.push_back(PreconditionFailure{goal, mSource, "and no starts by reversal: " + reason});
        }
    }
    failures.insert(failures.end(), forward.begin(), forward.end());
    return failures;
}
}

// The preconditions of a prepared RE (see checkPreconditions), and whether
// the starts of its matches are found by reversal (byReversal).
static std::vector<PreconditionFailure> modeFailures(const PreparedRE & prepared, const cc::Alphabet * sourceEncoding,
                                                     bool & byReversal) {
    // (The codes of a multiplexed alphabet are characters of its source.)
    if (const auto * mpx = dyn_cast_or_null<cc::MultiplexedAlphabet>(sourceEncoding)) {
        sourceEncoding = mpx->getSourceAlphabet();
    }
    // The alphabets of the RE as compiled (see RE_PipelineBuilder::ensurePrepared).
    const cc::Alphabet * lengthAlphabet = &cc::Unicode;
    const cc::Alphabet * codeUnitAlphabet = &cc::Unicode;
    if ((sourceEncoding != &cc::Unicode) && (prepared.mode.indexAlphabet == &cc::UTF8)) {
        lengthAlphabet = prepared.mode.lengthAlphabet;
        codeUnitAlphabet = &cc::UTF8;
    }
    // The passes are run to check the RE, not to compile it.
    QuietREPrinting quiet;
    ModeTransformation M(lengthAlphabet, codeUnitAlphabet);
    M.mAllowReversal = prepared.options.reversal;
    RE * const transformed = M.transform(prepared.re, prepared.options.goal);
    std::vector<PreconditionFailure> failures = M.failures(transformed, prepared.options.goal);
    byReversal = M.mStartsByReversal;
    return failures;
}

namespace {
std::vector<PreconditionFailure> forwardFailures(RE * re, const cc::Alphabet * lengthAlphabet,
                                                 const cc::Alphabet * codeUnitAlphabet) {
    ModeTransformation M(lengthAlphabet, codeUnitAlphabet);
    M.mAllowReversal = false;
    return M.failures(M.transform(re, RE_Goal::MatchEnds), RE_Goal::MatchEnds);
}
}

std::vector<PreconditionFailure> checkPreconditions(const PreparedRE & prepared, const cc::Alphabet * sourceEncoding) {
    bool byReversal = false;
    return modeFailures(prepared, sourceEncoding, byReversal);
}

std::string preconditionReport(const std::vector<PreconditionFailure> & failures) {
    std::string report;
    for (const PreconditionFailure & f : failures) {
        report += std::string(goalName(f.goal)) + ": " + f.reason + ": " + Printer_RE::PrintRE(f.construct) + "\n";
    }
    return report;
}

//
// Search and replace.
//
namespace {

// The matches selected from the left without overlap, given the starts and
// the ends (last positions) of the matches, where no match ends within
// another (re::matchEndsUnique): overlapping matches then end at the same
// position, and a start is selected unless it is within the match of an
// earlier start.  Selected: the selected starts; Covered: the positions of the
// selected matches; Keep: the positions not covered, less those of Drop.
class LeftmostMatchSelection final : public pablo::PabloKernel {
public:
    LeftmostMatchSelection(LLVMTypeSystemInterface & ts, StreamSet * Starts, StreamSet * Ends, StreamSet * Drop,
                           StreamSet * Selected, StreamSet * Covered, StreamSet * Keep)
    : PabloKernel(ts, std::string("LeftmostMatchSelection") + (Drop ? "_drop" : ""),
                  [&] {
                      Bindings inputs{Binding{"Starts", Starts}, Binding{"Ends", Ends}};
                      if (Drop) inputs.emplace_back("Drop", Drop);
                      return inputs;
                  }(),
                  {Binding{"Selected", Selected}, Binding{"Covered", Covered}, Binding{"Keep", Keep}}),
      mHasDrop(Drop != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const S = getInputStreamSet("Starts")[0];
        PabloAST * const E = getInputStreamSet("Ends")[0];
        PabloAST * const notE = pb.createNot(E);
        // The positions from marks M through the next end: the next end of each
        // mark is the end of its match.  (MatchStar over the positions other
        // than ends stops at the next end, which it includes.)
        auto through = [&](PabloAST * M) {
            return pb.createOr(pb.createAnd(M, E), pb.createMatchStar(pb.createAnd(M, notE), notE));
        };
        PabloAST * const inside = through(pb.createAdvance(pb.createAnd(S, notE), 1));
        PabloAST * const selected = pb.createAnd(S, pb.createNot(inside), "selected");
        PabloAST * const covered = through(selected);
        PabloAST * keep = pb.createNot(covered);
        if (mHasDrop) keep = pb.createAnd(keep, pb.createNot(getInputStreamSet("Drop")[0]));
        writeOutputStreamSet("Selected", std::vector<PabloAST *>{selected});
        writeOutputStreamSet("Covered", std::vector<PabloAST *>{covered});
        writeOutputStreamSet("Keep", std::vector<PabloAST *>{pb.createInFile(keep)});
    }
private:
    const bool mHasDrop;
};

// The bixnum of a constant count at the marked positions (zero elsewhere).
class ConstantCount final : public pablo::PabloKernel {
public:
    ConstantCount(LLVMTypeSystemInterface & ts, StreamSet * Marks, unsigned count, StreamSet * Counts)
    : PabloKernel(ts, "ConstantCount" + std::to_string(count) + "_" + std::to_string(Counts->getNumElements()),
                  {Binding{"Marks", Marks}}, {Binding{"Counts", Counts}}),
      mCount(count), mBits(Counts->getNumElements()) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const marks = getInputStreamSet("Marks")[0];
        std::vector<PabloAST *> counts;
        for (unsigned j = 0; j < mBits; j++) {
            counts.push_back(((mCount >> j) & 1) ? marks : pb.createZeroes());
        }
        writeOutputStreamSet("Counts", counts);
    }
private:
    const unsigned mCount;
    const unsigned mBits;
};

std::string walkStepsSignature(const std::vector<re::CaptureWalk::Step> & steps) {
    std::string s;
    for (const re::CaptureWalk::Step & step : steps) {
        if (step.run) {
            s += "r[";
            for (const auto & r : *step.run) s += std::to_string(r.first) + "-" + std::to_string(r.second) + ",";
            s += "]";
        } else {
            s += "a" + std::to_string(step.length);
        }
    }
    return s;
}

std::string captureWalkSignature(const re::CaptureWalk & walk) {
    std::string s = walkStepsSignature(walk.steps);
    auto boundary = [&](const re::CaptureWalk::Boundary & b) {
        s += (b.fromEnd ? "b" : b.backward ? "w" : "f") + std::to_string(b.at);
    };
    for (const re::CaptureWalk::CaptureBoundaries & c : walk.captures) {
        s += "|";
        boundary(c.start);
        boundary(c.follow);
    }
    return s;
}

unsigned captureWalkLookahead(const re::CaptureWalk & walk) {
    unsigned la = 1;
    for (const re::CaptureWalk::CaptureBoundaries & c : walk.captures) {
        if (c.start.fromEnd) la = std::max(la, c.start.at);
        if (c.follow.fromEnd) la = std::max(la, c.follow.at);
    }
    return la;
}

// The streams of the walk back from the ends of the selected matches (see
// re::CaptureWalk::backSteps), to be reversed (see RE_PipelineBuilder::
// captureBoundaries), over Unicode code units (Basis): the last characters
// of the selected matches, the positions covered by them, and the class of
// each run of the walk.
class BackwardWalkStreams final : public pablo::PabloKernel {
public:
    BackwardWalkStreams(LLVMTypeSystemInterface & ts, const re::CaptureWalk & walk, StreamSet * Basis,
                        StreamSet * Ends, StreamSet * Covered, StreamSet * Streams)
    : PabloKernel(ts, "BackwardWalkStreams_" + std::to_string(Basis->getNumElements()) + "_" +
                      getStringHash(walkStepsSignature(walk.backSteps)),
                  {Binding{"Basis", Basis}, Binding{"Ends", Ends}, Binding{"Covered", Covered}},
                  {Binding{"Streams", Streams}}),
      mSteps(walk.backSteps) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        cc::Parabix_CC_Compiler_Builder ccc(getInputStreamSet("Basis"));
        PabloAST * const covered = getInputStreamSet("Covered")[0];
        std::vector<PabloAST *> streams{pb.createAnd(getInputStreamSet("Ends")[0], covered), covered};
        for (const re::CaptureWalk::Step & step : mSteps) {
            if (step.run) streams.push_back(ccc.compileCC(step.run, pb));
        }
        writeOutputStreamSet("Streams", streams);
    }
private:
    const std::vector<re::CaptureWalk::Step> mSteps;
};

// The walk back from the ends of the selected matches, as a forward walk in
// the reversed regions (Reversed: the streams of BackwardWalkStreams,
// reversed), from their last characters: the cursor after each prefix of
// the steps (Cursors, one stream more than the steps).  After k characters,
// the cursor is at the reversed position of the character before the
// boundary k characters back from the follow of the match.
class BackwardWalk final : public pablo::PabloKernel {
public:
    BackwardWalk(LLVMTypeSystemInterface & ts, const re::CaptureWalk & walk, StreamSet * Reversed, StreamSet * Cursors)
    : PabloKernel(ts, "BackwardWalk_" + getStringHash(walkStepsSignature(walk.backSteps)),
                  {Binding{"Reversed", Reversed}}, {Binding{"Cursors", Cursors}}),
      mSteps(walk.backSteps) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> reversed = getInputStreamSet("Reversed");
        PabloAST * const covered = reversed[1];
        std::vector<PabloAST *> at{reversed[0]};
        unsigned run = 2;
        for (const re::CaptureWalk::Step & step : mSteps) {
            if (step.run) {
                at.push_back(pb.createScanThru(at.back(), pb.createAnd(reversed[run++], covered)));
            } else {
                at.push_back(pb.createAdvance(at.back(), step.length));
            }
        }
        writeOutputStreamSet("Cursors", at);
    }
private:
    const std::vector<re::CaptureWalk::Step> mSteps;
};

// The boundaries of captures in the selected matches (see re::CaptureWalk),
// over Unicode code units (Basis): for each capture, its start and its follow
// (Marks, two streams per capture).  Starts: the selected starts; Ends: the
// ends of the matches, of which those of the selected ones are Covered;
// Backward: the cursors of the walk back (see BackwardWalk) reversed back, at
// the characters before the boundaries, if the walk has backward boundaries.
class CaptureBoundaries final : public pablo::PabloKernel {
public:
    CaptureBoundaries(LLVMTypeSystemInterface & ts, const re::CaptureWalk & walk, StreamSet * Basis,
                      StreamSet * Starts, StreamSet * Ends, StreamSet * Covered, StreamSet * Backward,
                      StreamSet * Marks)
    : PabloKernel(ts, "CaptureBoundaries_" + std::to_string(Basis->getNumElements()) + "_" +
                      getStringHash(captureWalkSignature(walk)),
                  [&] {
                      Bindings inputs{Binding{"Basis", Basis}, Binding{"Starts", Starts},
                                      Binding{"Ends", Ends, FixedRate(1), LookAhead(captureWalkLookahead(walk))},
                                      Binding{"Covered", Covered, FixedRate(1), LookAhead(captureWalkLookahead(walk))}};
                      if (Backward) inputs.emplace_back("Backward", Backward);
                      return inputs;
                  }(),
                  {Binding{"Marks", Marks}}),
      mWalk(walk), mHasBackward(Backward != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        cc::Parabix_CC_Compiler_Builder ccc(getInputStreamSet("Basis"));
        PabloAST * const ends = getInputStreamSet("Ends")[0];
        PabloAST * const covered = getInputStreamSet("Covered")[0];
        // The cursor after each step of the walk from the starts.
        std::vector<PabloAST *> at{getInputStreamSet("Starts")[0]};
        for (const re::CaptureWalk::Step & step : mWalk.steps) {
            if (step.run) {
                at.push_back(pb.createScanThru(at.back(), pb.createAnd(ccc.compileCC(step.run, pb), covered)));
            } else {
                at.push_back(pb.createAdvance(at.back(), step.length));
            }
        }
        // Back from the follows of the selected matches (the positions after
        // their ends).
        std::vector<PabloAST *> backward;
        if (mHasBackward) backward = getInputStreamSet("Backward");
        auto boundary = [&](const re::CaptureWalk::Boundary & b) -> PabloAST * {
            if (b.backward) return pb.createAdvance(backward[b.at], 1);
            if (!b.fromEnd) return at[b.at];
            if (b.at == 0) return pb.createAdvance(pb.createAnd(ends, covered), 1);
            if (b.at == 1) return pb.createAnd(ends, covered);
            return pb.createAnd(pb.createLookahead(ends, b.at - 1), pb.createLookahead(covered, b.at - 1));
        };
        std::vector<PabloAST *> marks;
        for (const re::CaptureWalk::CaptureBoundaries & c : mWalk.captures) {
            marks.push_back(boundary(c.start));
            marks.push_back(boundary(c.follow));
        }
        writeOutputStreamSet("Marks", marks);
    }
private:
    const re::CaptureWalk mWalk;
    const bool mHasBackward;
};

// The positions from the start S of a capture to the position before its
// follow F, given the starts and the follows of the capture in all the
// selected matches.  A position marked by both is either an empty capture or
// the follow of the capture of a match and the start of the capture of the
// next (adjacent) match, which is then within a capture: from a start that is
// not a follow, the captures extend through the positions other than the
// follows that are not starts.  (Both cases at one position would make the
// search pattern nullable: an empty capture at the start of a match, after a
// capture ending at the follow of the previous match.)
PabloAST * captureCoverage(PabloBuilder & pb, PabloAST * S, PabloAST * F) {
    PabloAST * const open = pb.createAnd(S, pb.createNot(F));
    PabloAST * const notClose = pb.createNot(pb.createAnd(F, pb.createNot(S)));
    return pb.createAnd(pb.createMatchStar(open, notClose), notClose);
}

// The starts, coverage and follows of captures, given their starts and
// follows (CaptureBoundaries).
class CaptureSpans final : public pablo::PabloKernel {
public:
    CaptureSpans(LLVMTypeSystemInterface & ts, StreamSet * Marks, StreamSet * Spans)
    : PabloKernel(ts, "CaptureSpans" + std::to_string(Marks->getNumElements()),
                  {Binding{"Marks", Marks}}, {Binding{"Spans", Spans}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> marks = getInputStreamSet("Marks");
        std::vector<PabloAST *> spans;
        for (unsigned j = 0; j + 1 < marks.size(); j += 2) {
            spans.push_back(marks[j]);
            spans.push_back(captureCoverage(pb, marks[j], marks[j + 1]));
            spans.push_back(marks[j + 1]);
        }
        writeOutputStreamSet("Spans", spans);
    }
};

// A literal piece of a replacement, inserted before an anchor of a match: its
// start, its follow (the position after its last code unit), or the start of
// a capture, possibly only where that is (or is not) the follow of the match.
struct ReplacementSlot {
    enum class Anchor {Start, Follow, CaptureStart};
    enum class When {Always, AtFollow, NotAtFollow};
    Anchor anchor;
    unsigned capture;
    When when;
    std::vector<unsigned> units;
};

// The positions kept when the matches are replaced (Kept: those of Keep and
// the captures referenced, given by their starts and follows, Marks), and
// the anchors of the slots of the replacement (Flags, one per slot, and Any).
class ReplacementAnchors final : public pablo::PabloKernel {
public:
    ReplacementAnchors(LLVMTypeSystemInterface & ts, const std::vector<ReplacementSlot> & slots,
                       StreamSet * Selected, StreamSet * Ends, StreamSet * Covered, StreamSet * Keep,
                       StreamSet * Marks, StreamSet * Kept, StreamSet * Flags, StreamSet * Any)
    : PabloKernel(ts, "ReplacementAnchors" + std::to_string(Marks ? Marks->getNumElements() : 0) + "_" + [&] {
                      std::string sig;
                      for (const ReplacementSlot & s : slots) {
                          sig += std::to_string(static_cast<int>(s.anchor)) + std::to_string(s.capture) +
                                 std::to_string(static_cast<int>(s.when)) + ",";
                      }
                      return sig;
                  }(),
                  [&] {
                      Bindings inputs{Binding{"Selected", Selected}, Binding{"Ends", Ends},
                                      Binding{"Covered", Covered}, Binding{"Keep", Keep}};
                      if (Marks) inputs.emplace_back("Marks", Marks);
                      return inputs;
                  }(),
                  [&] {
                      Bindings outputs{Binding{"Kept", Kept}};
                      if (Flags) {
                          outputs.emplace_back("Flags", Flags);
                          outputs.emplace_back("Any", Any);
                      }
                      return outputs;
                  }()),
      mSlots(slots), mHasMarks(Marks != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const selected = getInputStreamSet("Selected")[0];
        PabloAST * const covered = getInputStreamSet("Covered")[0];
        PabloAST * const follow = pb.createAdvance(pb.createAnd(getInputStreamSet("Ends")[0], covered), 1);
        std::vector<PabloAST *> marks;
        if (mHasMarks) marks = getInputStreamSet("Marks");
        PabloAST * kept = getInputStreamSet("Keep")[0];
        for (unsigned j = 0; j + 1 < marks.size(); j += 2) {
            kept = pb.createOr(kept, captureCoverage(pb, marks[j], marks[j + 1]));
        }
        writeOutputStreamSet("Kept", std::vector<PabloAST *>{kept});
        if (mSlots.empty()) return;
        std::vector<PabloAST *> flags;
        PabloAST * any = pb.createZeroes();
        for (const ReplacementSlot & s : mSlots) {
            PabloAST * f = (s.anchor == ReplacementSlot::Anchor::Start) ? selected
                         : (s.anchor == ReplacementSlot::Anchor::Follow) ? follow : marks[2 * s.capture];
            if (s.when == ReplacementSlot::When::AtFollow) {
                f = pb.createAnd(f, follow);
            } else if (s.when == ReplacementSlot::When::NotAtFollow) {
                f = pb.createAnd(f, pb.createNot(follow));
            }
            flags.push_back(f);
            any = pb.createOr(any, f);
        }
        writeOutputStreamSet("Flags", flags);
        writeOutputStreamSet("Any", std::vector<PabloAST *>{any});
    }
private:
    const std::vector<ReplacementSlot> mSlots;
    const bool mHasMarks;
};

// The code units of the slots written into the positions inserted before
// their anchors: before each position with a flag, a block of all the slots
// (Size code units) was inserted, of which those flagged there are written
// and kept (Kept), the others deleted.
class FillSlots final : public pablo::PabloKernel {
public:
    FillSlots(LLVMTypeSystemInterface & ts, const std::vector<ReplacementSlot> & slots, unsigned size,
              StreamSet * Text, StreamSet * Keep, StreamSet * Flags, StreamSet * Filled, StreamSet * Kept)
    : PabloKernel(ts, "FillSlots" + std::to_string(Text->getNumElements()) + "_" + [&] {
                      std::string sig;
                      for (const ReplacementSlot & s : slots) {
                          for (unsigned u : s.units) sig += std::to_string(u) + ",";
                          sig += "|";
                      }
                      return getStringHash(sig);
                  }(),
                  {Binding{"Text", Text}, Binding{"Keep", Keep},
                   Binding{"Flags", Flags, FixedRate(1), LookAhead(size)}},
                  {Binding{"Filled", Filled}, Binding{"Kept", Kept}}),
      mSlots(slots), mSize(size) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> text = getInputStreamSet("Text");
        const std::vector<PabloAST *> flags = getInputStreamSet("Flags");
        // Code unit k of a slot at offset off of the block is at the
        // position mSize - off - k before the flag.
        std::vector<std::pair<PabloAST *, unsigned>> written;
        unsigned off = 0;
        for (unsigned s = 0; s < mSlots.size(); s++) {
            for (unsigned k = 0; k < mSlots[s].units.size(); k++) {
                written.emplace_back(pb.createLookahead(flags[s], mSize - off - k), mSlots[s].units[k]);
            }
            off += mSlots[s].units.size();
        }
        std::vector<PabloAST *> filled(text);
        PabloAST * kept = getInputStreamSet("Keep")[0];
        for (const auto & w : written) {
            kept = pb.createOr(kept, w.first);
            for (unsigned b = 0; b < filled.size(); b++) {
                if ((w.second >> b) & 1) filled[b] = pb.createOr(filled[b], w.first);
            }
        }
        writeOutputStreamSet("Filled", filled);
        writeOutputStreamSet("Kept", std::vector<PabloAST *>{kept});
    }
private:
    const std::vector<ReplacementSlot> mSlots;
    const unsigned mSize;
};

// The replacement as literal pieces around the references that are not
// folded (n references, between n + 1 pieces), or false if it is not a
// sequence of literal characters and references.
bool replacementPieces(const PreparedSearchReplace & sr, std::vector<std::u32string> & pieces,
                       std::vector<std::string> & references) {
    std::vector<re::ReplacementItem> items;
    if (!foldReplacement(sr.replacement, sr.search.re, items)) return false;
    pieces.assign(1, std::u32string());
    references.clear();
    for (const re::ReplacementItem & item : items) {
        if (item.reference) {
            references.push_back(item.reference->getName());
            pieces.emplace_back();
        } else {
            pieces.back() += item.literal;
        }
    }
    return true;
}

// The preconditions of a search and replace other than those of the search
// pattern as compiled (see ModeTransformation::failures).
std::vector<PreconditionFailure> searchReplaceFailures(const PreparedSearchReplace & sr) {
    const RE_Goal goal = RE_Goal::SearchReplace;
    std::vector<PreconditionFailure> failures;
    RE * const search = sr.search.re;
    if (sr.replacement == nullptr) {
        failures.push_back(PreconditionFailure{goal, search, "no replacement"});
        return failures;
    }
    if (getLengthRange(search, &cc::Unicode).first == 0) {
        failures.push_back(PreconditionFailure{goal, search, "the search pattern can match the empty string"});
    }
    if (!matchEndsUnique(search)) {
        failures.push_back(PreconditionFailure{goal, search,
                           "a match may end within another match (other than at its end): which matches are "
                           "replaced may then depend on the matching conventions (greedy or lazy, longest or "
                           "first), or need a sequential selection of overlapping matches (not yet supported)"});
    }
    const std::vector<Reference *> unresolved = unresolvedReferences(sr.replacement, search);
    for (Reference * r : unresolved) {
        failures.push_back(PreconditionFailure{goal, r, "a reference to a capture that the search pattern does not have"});
    }
    std::vector<std::u32string> pieces;
    std::vector<std::string> references;
    if (!replacementPieces(sr, pieces, references)) {
        failures.push_back(PreconditionFailure{goal, sr.replacement,
                           "the replacement is not a sequence of literal characters and references"});
    } else if (!references.empty() && unresolved.empty()) {
        re::CaptureWalk walk;
        RE * construct = nullptr;
        std::string reason;
        if (!planCaptureWalk(search, references, walk, construct, reason, sr.search.options.reversal)) {
            failures.push_back(PreconditionFailure{goal, construct, reason});
        } else {
            for (size_t j = 0; j + 1 < walk.captures.size(); j++) {
                if (walk.captures[j].close >= walk.captures[j + 1].open) {
                    failures.push_back(PreconditionFailure{goal, sr.replacement,
                                       "the references must be to captures in their order in the search pattern, "
                                       "each referred to once and not within another: moving or copying captured "
                                       "text is not yet supported"});
                    break;
                }
            }
        }
        if ((sr.search.mode.lengthAlphabet != &cc::Unicode) || (sr.search.mode.indexAlphabet != &cc::Unicode)) {
            failures.push_back(PreconditionFailure{goal, sr.replacement,
                               "references to captures require full Unicode indexing (see prepareSearchReplace)"});
        }
    }
    return failures;
}

void reportFailures(const std::vector<PreconditionFailure> & failures) {
    if (!failures.empty()) {
        llvm::report_fatal_error(llvm::StringRef("Regular expression engine preconditions not met:\n") +
                                 preconditionReport(failures));
    }
}

}

std::vector<PreconditionFailure> checkPreconditions(const PreparedSearchReplace & sr, const cc::Alphabet * sourceEncoding) {
    std::vector<PreconditionFailure> failures = checkPreconditions(sr.search, sourceEncoding);
    for (PreconditionFailure & f : searchReplaceFailures(sr)) failures.push_back(std::move(f));
    return failures;
}

RE_PipelineBuilder::SelectedMatches RE_PipelineBuilder::selectMatches(const PreparedSearchReplace & sr, bool extendText) {
    usePrepared(sr.search);
    mExtendText = extendText;
    RE * re = sr.search.re;
    ensurePrepared(re);
    mRE = applyModePasses(re, RE_Goal::SearchReplace);
    mRE = processReferences(mRE);
    prepareExternals(mRE);
    // The starts of the matches, and their ends (last positions).
    StreamSet * const starts = mPB.CreateStreamSet(1);
    compileStarts(mRE, starts);
    StreamSet * ends = mPB.CreateStreamSet(1);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, mRE, ends);
    if (const unsigned offset = grepOffset(mRE)) {
        StreamSet * const shifted = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<ShiftBack>(ends, shifted, offset);
        ends = shifted;
    }
    SelectedMatches m;
    m.ends = ends;
    m.selected = mPB.CreateStreamSet(1);
    m.covered = mPB.CreateStreamSet(1);
    m.keep = mPB.CreateStreamSet(1);
    StreamSet * const drop = mTextExtended ? mCtxt.mMatchFollows : nullptr;
    mPB.CreateKernelCall<LeftmostMatchSelection>(starts, ends, drop, m.selected, m.covered, m.keep);
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        mPB.captureBitstream("srStarts", starts);
        mPB.captureBitstream("srEnds", ends);
        mPB.captureBitstream("srSelected", m.selected);
        mPB.captureBitstream("srCovered", m.covered);
        mPB.captureBitstream("srKeep", m.keep);
    }
    return m;
}

StreamSet * RE_PipelineBuilder::captureBoundaries(const re::CaptureWalk & walk, const SelectedMatches & m, RE * search) {
    if (mCtxt.mCodeUnitAlphabet != &cc::Unicode) {
        llvm::report_fatal_error("the boundaries of captures require full Unicode indexing (see prepareSearchReplace)");
    }
    // The classes of the runs of the walk, as compiled over the code units.
    re::CaptureWalk codeUnitWalk = walk;
    for (re::CaptureWalk::Step & step : codeUnitWalk.steps) if (step.run) step.run = codeUnitCC(step.run);
    for (re::CaptureWalk::Step & step : codeUnitWalk.backSteps) if (step.run) step.run = codeUnitCC(step.run);
    // The boundaries found by the walk back from the ends of the matches:
    // a forward walk in the regions reversed in place (whose barriers no
    // match crosses), from the last characters of the selected matches.
    // The cursor at the reversed position of the character before each
    // boundary (a character of the match, as the items before a backward
    // boundary are not nullable) is reversed back to that character.
    StreamSet * backward = nullptr;
    if (walk.hasBackward()) {
        unsigned runs = 0;
        for (const re::CaptureWalk::Step & step : walk.backSteps) if (step.run) runs++;
        StreamSet * const barriers = reversalBarriers(search);
        StreamSet * const streams = mPB.CreateStreamSet(2 + runs);
        mPB.CreateKernelCall<BackwardWalkStreams>(codeUnitWalk, mCtxt.mCodeUnitStream, m.ends, m.covered, streams);
        StreamSet * const reversed = mPB.CreateStreamSet(2 + runs);
        mPB.CreateKernelCall<RegionReversal>(barriers, streams, reversed, ReverseRegionLimit);
        StreamSet * const cursors = mPB.CreateStreamSet(walk.backSteps.size() + 1);
        mPB.CreateKernelCall<BackwardWalk>(walk, reversed, cursors);
        backward = mPB.CreateStreamSet(walk.backSteps.size() + 1);
        mPB.CreateKernelCall<RegionReversal>(barriers, cursors, backward, ReverseRegionLimit);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBixNum("srBackwardCursors", backward);
        }
    }
    StreamSet * const marks = mPB.CreateStreamSet(2 * walk.captures.size());
    mPB.CreateKernelCall<CaptureBoundaries>(codeUnitWalk, mCtxt.mCodeUnitStream, m.selected, m.ends, m.covered, backward,
                                            marks);
    return marks;
}

void RE_PipelineBuilder::captureSpansPipeline(const PreparedSearchReplace & sr, const std::vector<std::string> & captures,
                                              StreamSet * spans) {
    if (sr.search.options.goal != RE_Goal::SearchReplace) {
        llvm::report_fatal_error("captureSpansPipeline: the search pattern must be prepared with prepareSearchReplace");
    }
    reportFailures(searchReplaceFailures(sr));
    re::CaptureWalk walk;
    RE * construct = nullptr;
    std::string reason;
    if (!planCaptureWalk(sr.search.re, captures, walk, construct, reason, sr.search.options.reversal)) {
        reportFailures({PreconditionFailure{RE_Goal::SearchReplace, construct, reason}});
    }
    const SelectedMatches m = selectMatches(sr, true);
    StreamSet * const marks = captureBoundaries(walk, m, sr.search.re);
    mPB.CreateKernelCall<CaptureSpans>(marks, spans);
}

RE_PipelineBuilder::ReplacedText RE_PipelineBuilder::replaceMatches(const PreparedSearchReplace & sr, bool follows,
                                                                    bool replaced, StreamSet * result) {
    if (sr.search.options.goal != RE_Goal::SearchReplace) {
        llvm::report_fatal_error("search and replace: the search pattern must be prepared with prepareSearchReplace");
    }
    reportFailures(searchReplaceFailures(sr));
    // The replacement: literal pieces around references to captures, in the
    // order of the captures (see the preconditions).
    std::vector<std::u32string> pieces;
    std::vector<std::string> references;
    replacementPieces(sr, pieces, references);
    const unsigned n = references.size();
    re::CaptureWalk walk;
    if (n > 0) {
        RE * construct = nullptr;
        std::string reason;
        planCaptureWalk(sr.search.re, references, walk, construct, reason, sr.search.options.reversal);
    }
    // With references, a piece may be inserted before the follow of a
    // match, which must be a position of the text: without match regions,
    // the text is extended by one.
    mNeedsCodepoints = true;
    const SelectedMatches m = selectMatches(sr, n > 0);
    if ((follows || replaced) && (mCtxt.mMatchFollows == nullptr)) {
        llvm::report_fatal_error("searchReplacePipeline: the follows of the result require match regions");
    }
    StreamSet * const marks = (n > 0) ? captureBoundaries(walk, m, sr.search.re) : nullptr;
    // Each match is deleted but for the captures referenced, which stay in
    // place, and each literal piece is inserted before an anchor: piece j
    // before the start of the capture of reference j, and the last piece
    // before the follow of the match (or before its start, without
    // references).  The pieces inserted before one position are those of the
    // match that it follows, then those of the match that it is in: they are
    // given slots in that order, and a block of all the slots is inserted
    // before each anchor, of which only the pieces anchored there are kept.
    // The start of a capture whose prefix in the search pattern is not
    // nullable is never the start of a match: if it is the follow of a match,
    // it is that of its own match.  (The start of a capture whose prefix is
    // nullable may be the start of its match, and the follow of the previous
    // match: its pieces are inserted after those of the previous match.)
    const bool utf8 = (mCtxt.mCodeUnitAlphabet == &cc::UTF8);
    auto codeUnits = [&](const std::u32string & s) {
        std::vector<unsigned> units;
        if (utf8) {
            for (const char c : toUTF8String(s)) units.push_back(static_cast<unsigned char>(c));
        } else {
            units.assign(s.begin(), s.end());
        }
        return units;
    };
    using Anchor = ReplacementSlot::Anchor;
    using When = ReplacementSlot::When;
    std::vector<ReplacementSlot> followSlots, inMatchSlots;
    for (unsigned j = 0; j <= n; j++) {
        if (pieces[j].empty()) continue;
        const std::vector<unsigned> units = codeUnits(pieces[j]);
        if (j < n) {
            if (walk.captures[j].prefixMinLength > 0) {
                followSlots.push_back(ReplacementSlot{Anchor::CaptureStart, j, When::AtFollow, units});
                inMatchSlots.push_back(ReplacementSlot{Anchor::CaptureStart, j, When::NotAtFollow, units});
            } else {
                inMatchSlots.push_back(ReplacementSlot{Anchor::CaptureStart, j, When::Always, units});
            }
        } else if (n == 0) {
            inMatchSlots.push_back(ReplacementSlot{Anchor::Start, 0, When::Always, units});
        } else {
            followSlots.push_back(ReplacementSlot{Anchor::Follow, 0, When::Always, units});
        }
    }
    std::vector<ReplacementSlot> slots(followSlots);
    slots.insert(slots.end(), inMatchSlots.begin(), inMatchSlots.end());
    unsigned size = 0;
    for (const ReplacementSlot & s : slots) size += s.units.size();
    StreamSet * keep = m.keep;
    StreamSet * flags = nullptr;
    StreamSet * any = nullptr;
    if (marks || !slots.empty()) {
        keep = mPB.CreateStreamSet(1);
        if (!slots.empty()) {
            flags = mPB.CreateStreamSet(slots.size());
            any = mPB.CreateStreamSet(1);
        }
        mPB.CreateKernelCall<ReplacementAnchors>(slots, m.selected, m.ends, m.covered, m.keep, marks, keep, flags, any);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            if (marks) mPB.captureBixNum("srCaptureMarks", marks);
            if (flags) mPB.captureBixNum("srSlotFlags", flags);
        }
    }
    StreamSet * matchedFollows = nullptr;
    if (replaced) {
        matchedFollows = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<MatchedLinesKernel>(m.selected, mCtxt.mMatchFollows, matchedFollows);
    }
    if (mCtxt.mMultiplexed) {
        llvm::report_fatal_error("RE_PipelineBuilder: the text of a replacement needs the codepoints, not a multiplexed basis");
    }
    StreamSet * text = utf8 ? codeUnitBasis() : mCtxt.mCodeUnitStream;
    StreamSet * resultFollows = follows ? mCtxt.mMatchFollows : nullptr;
    StreamSet * resultReplaced = matchedFollows;
    // The blocks of slots inserted and filled.
    if (size > 0) {
        unsigned bits = 0;
        while ((size >> bits) != 0) bits++;
        StreamSet * const counts = mPB.CreateStreamSet(bits);
        mPB.CreateKernelCall<ConstantCount>(any, size, counts);
        StreamSet * const spreadMask = mPB.CreateStreamSet(1);
        InsertionSpreadMask(mPB, counts, spreadMask, kernel::InsertPosition::Before);
        auto spread = [&](StreamSet * s) {
            StreamSet * const x = mPB.CreateStreamSet(s->getNumElements());
            SpreadByMask(mPB, spreadMask, s, x);
            return x;
        };
        text = spread(text);
        keep = spread(keep);
        flags = spread(flags);
        if (resultFollows) resultFollows = spread(resultFollows);
        if (resultReplaced) resultReplaced = spread(resultReplaced);
        StreamSet * const filled = mPB.CreateStreamSet(text->getNumElements());
        StreamSet * const kept = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<FillSlots>(slots, size, text, keep, flags, filled, kept);
        text = filled;
        keep = kept;
    }
    auto filtered = [&](StreamSet * s, StreamSet * f) {
        if (f == nullptr) f = mPB.CreateStreamSet(s->getNumElements());
        FilterByMask(mPB, keep, s, f);
        return f;
    };
    if (result && (result->getNumElements() != text->getNumElements())) {
        llvm::report_fatal_error("searchReplaceCodeUnitsPipeline: the result must have the basis bits of the code units "
                                 "of the source (8 for UTF-8, 21 for Unicode)");
    }
    ReplacedText r;
    r.text = filtered(text, result);
    if (resultFollows) r.follows = filtered(resultFollows, nullptr);
    if (resultReplaced) r.replaced = filtered(resultReplaced, nullptr);
    return r;
}

void RE_PipelineBuilder::searchReplacePipeline(const PreparedSearchReplace & sr, StreamSet * output,
                                               StreamSet * follows, StreamSet * replaced) {
    ReplacedText r = replaceMatches(sr, follows != nullptr, replaced != nullptr, nullptr);
    // The result in UTF-8 code units, and its follows at their positions.
    StreamSet * u8basis = r.text;
    if (mCtxt.mCodeUnitAlphabet != &cc::UTF8) {
        u8basis = mPB.CreateStreamSet(8);
        U21_to_UTF8(mPB, r.text, u8basis);
        if (follows || replaced) {
            StreamSet * const u8index = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<UTF8_index>(u8basis, u8index);
            if (follows) {
                StreamSet * const s = mPB.CreateStreamSet(1);
                SpreadByMask(mPB, u8index, r.follows, s);
                r.follows = s;
            }
            if (replaced) {
                StreamSet * const s = mPB.CreateStreamSet(1);
                SpreadByMask(mPB, u8index, r.replaced, s);
                r.replaced = s;
            }
        }
    }
    mPB.CreateKernelCall<P2SKernel>(u8basis, output);
    if (follows) mPB.CreateKernelCall<StreamSelect>(follows, Select(r.follows, {0}));
    if (replaced) mPB.CreateKernelCall<StreamSelect>(replaced, Select(r.replaced, {0}));
}

void RE_PipelineBuilder::searchReplaceCodeUnitsPipeline(const PreparedSearchReplace & sr, StreamSet * codeUnits) {
    replaceMatches(sr, false, false, codeUnits);
}

RE * RE_PipelineBuilder::applyModePasses(RE * re, RE_Goal goal) {
    ModeTransformation M(mCtxt.mLengthAlphabet, mCtxt.mCodeUnitAlphabet);
    // (Compiled forward: the starts by reversal are chosen before, see
    // matchStartsPipeline.)
    M.mAllowReversal = mModeOptions.reversal;
    M.mAcceptStartsByReversal = false;
    RE * const xfrmedRE = M.transform(re, goal);
    const std::vector<PreconditionFailure> failures = M.failures(xfrmedRE, goal);
    if (!failures.empty()) {
        llvm::report_fatal_error(llvm::StringRef("Regular expression engine preconditions not met:\n") +
                                 preconditionReport(failures));
    }

    for (auto m : M.PE.mNameMap) {
        if (re::PropertyExpression * pe = dyn_cast<re::PropertyExpression>(m.second)) {
            compileProperty(pe);
        }
    }

    for (auto m : M.CCnamer.mNameMap) {
        std::vector<re::CC *> ccs = {cast<re::CC>(m.second)};
        StreamSet * ccStrm = mPB.CreateStreamSet(1);
        compileClasses(ccs, ccStrm, mCtxt.mCodeUnitStream);
        if ((mCtxt.mCodeUnitAlphabet == &cc::UTF8) && replacedUTF8Errors() && ccs[0]->contains(0xFFFD)) {
            // The maximal ill-formed subparts of the input, at their final
            // code units, are characters U+FFFD (see codegen::UTF8Errors).
            StreamSet * const errors = mPB.CreateStreamSet(1);
            mPB.CreateKernelCall<UTF8_ErrorMarks>(mCtxt.mCodeUnitStream, errors, pablo::BitMovementMode::Advance);
            StreamSet * const withErrors = mPB.CreateStreamSet(1);
            OrCombine(mPB, ccStrm, errors, withErrors);
            ccStrm = withErrors;
        }
        addExternal(m.first, ExternalStream{ExternalStreamKind::FixedLength, 0, {1, 1}, ccStrm});
    }

    for (auto & m : M.SCnamer.mStrings) {
        StreamSet * fillStartsEnds = mPB.CreateStreamSet(3);
        mPB.CreateKernelCall<StringClassKernel>(codeUnitStrings(m.second), codeUnitBasis(), fillStartsEnds);
        addExternal(m.first, ExternalStream{ExternalStreamKind::StringClassRep, 0, {0, 0}, fillStartsEnds});
    }

    return xfrmedRE;
}

RE * RE_PipelineBuilder::processReferences(RE * re) {
    re::ReferenceInfo mRefInfo = re::buildReferenceInfo(re);
    if (!mRefInfo.twixtREs.empty()) {
        if (mCtxt.mMultiplexed) {
            llvm::report_fatal_error("RE_PipelineBuilder: references compare codepoints, not with a multiplexed basis");
        }
        re::FixedReferenceTransformer FRT(mRefInfo, *mCtxt.mLengthAlphabet);
        RE * xfrmed = FRT.transformRE(re);
        for (auto m : FRT.mNameMap) {
            auto name = m.first;
            re::Reference * ref = cast<re::Reference>(m.second);
            UCD::property_t p = ref->getReferencedProperty();
            std::string instanceName = ref->getInstanceName();
            unsigned captureLen = static_cast<unsigned>(getLengthRange(ref->getCapture(), mCtxt.mLengthAlphabet).first);
            auto mapping = mRefInfo.twixtREs.find(instanceName);
            auto twixtLen = getLengthRange(mapping->second, mCtxt.mLengthAlphabet).first;
            auto dist = captureLen + twixtLen;
            UCD::PropertyObject * propObj = UCD::getPropertyObject(p);
            if (auto * obj = dyn_cast<UCD::EnumeratedPropertyObject>(propObj)) {
                std::string extName = UCD::getPropertyFullName(p) + "_basis";
                std::vector<UCD::UnicodeSet> & bases = obj->GetEnumerationBasisSets();
                std::vector<re::CC *> ccs;
                for (auto & b : bases) ccs.push_back(makeCC(b, &cc::Unicode));
                StreamSet * propertyBasis = mPB.CreateStreamSet(ccs.size());
                mPB.CreateKernelFamilyCall<CharClassesKernel>(ccs, mCtxt.mCodeUnitStream, propertyBasis);
                StreamSet * distStrm = mPB.CreateStreamSet(1);
                mPB.CreateKernelCall<FixedDistanceMatchesKernel>(dist, captureLen, propertyBasis, distStrm);
                addExternal(name, ExternalStream{ExternalStreamKind::FixedLength, 0u, {1, 1}, distStrm});
            } else if (isa<UCD::CodePointPropertyObject>(propObj)) {
                // Identity or other codepoint properties
                StreamSet * distStrm = mPB.CreateStreamSet(1);
                mPB.CreateKernelCall<CodePointMatchKernel>(p, dist, captureLen, mCtxt.mCodeUnitStream, distStrm);
                addExternal(name, ExternalStream{ExternalStreamKind::FixedLength, 0u, {captureLen, captureLen}, distStrm});
            } else {
                llvm::report_fatal_error("Property reference must be an enumerated or codepoint property.");
            }
        }
        return xfrmed;
    }
    return re;
}


void UnicodePropertyLogic(PipelineBuilder & P, PropertyExpression * pe,
                          StreamSet * BasisBits, StreamSet * PropertyStream) {
    UnicodePropertyLogic(P, pe, BasisBits, nullptr, PropertyStream);
}

void UnicodePropertyLogic(PipelineBuilder & P, re::PropertyExpression * pe,
                          StreamSet * BasisBits, StreamSet * IndexStream, StreamSet * PropertyStream) {
    //pe = cast<PropertyExpression>(UCD::linkAndResolve(pe, grep::lineNumGrep));
    std::string propName = pe->getFullName();
    if (pe->getKind() == re::PropertyExpression::Kind::Codepoint) {
        P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(pe, BasisBits, PropertyStream);
    } else { //PropertyExpression::Kind::Boundary
        if (pe->getOperator() == re::PropertyExpression::Operator::NEq) {
            // Boundary kernels compute positive boundaries only.
            llvm::report_fatal_error("negated boundary " + llvm::StringRef(propName) +
                                     " must be compiled as a zero-width complement");
        }
        if (BasisBits->getNumElements() < 21) {
            if (IndexStream == nullptr) {
                llvm::report_fatal_error("index stream required for boundary properties without full Unicode basis");
            }
        }
        UCD::property_t prop = static_cast<UCD::property_t>(pe->getPropertyCode());
        UCD::PropertyObject * propObj = UCD::getPropertyObject(prop);
        if (UCD::BoundaryPropertyObject * bObj = dyn_cast<UCD::BoundaryPropertyObject>(propObj)) {
            // Grapheme Cluster and level 2 word boundaries \b{g}, \b{w} 
            re::RE * bRE = bObj->GetBoundaryExpression();
            const auto b_Sets = re::collectCCs(bRE, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
            auto b_mpx = cc::makeMultiplexedAlphabet("b_mpx", b_Sets);
            bRE = transformCCs(b_mpx, bRE, re::NameTransformationMode::TransformDefinition);
            auto b_basis = b_mpx->getMultiplexedCCs();
            StreamSet * b_Classes = P.CreateStreamSet(b_basis.size());
            P.CreateKernelFamilyCall<CharClassesKernel>(b_basis, BasisBits, b_Classes);
            re::LookAheadNamer LA;
            bRE = LA.transformRE(bRE);
            //
            RE_CompilerContext ctxt;

            if (IndexStream && (prop == UCD::w)) {
                // Must switch to Unicode indexing for word boundaries.
                StreamSet * b_Classes_Uindexed = P.CreateStreamSet(b_basis.size());
                FilterByMask(P, IndexStream, b_Classes, b_Classes_Uindexed);
                ctxt.setCodeUnitContext(b_mpx, b_Classes_Uindexed);
                StreamSet * U_property = P.CreateStreamSet(1);
                RE_PipelineBuilder RE_PB(P, ctxt);
                RE_PB.matchSearchPipeline(bRE, U_property);
                SpreadByMask(P, IndexStream, U_property, PropertyStream);
            } else {
                if (BasisBits->getNumElements() < 21) {
                    ctxt.setCodeUnitContext(&cc::UTF8, BasisBits);
                } else {
                    ctxt.setCodeUnitContext(&cc::Unicode, BasisBits);
                }
                ctxt.addAlphabet(b_mpx, b_Classes);
                if (IndexStream) {
                    ctxt.setIndexingContext(&cc::Unicode, IndexStream);
                }
                RE_PipelineBuilder RE_PB(P, ctxt);
                RE_PB.matchSearchPipeline(bRE, PropertyStream);
            }
        } else if (auto * obj = dyn_cast<UCD::EnumeratedPropertyObject>(propObj)) {
            std::vector<UCD::UnicodeSet> & bases = obj->GetEnumerationBasisSets();
            std::vector<re::CC *> ccs;
            for (auto & b : bases) ccs.push_back(makeCC(b, &cc::Unicode));
            StreamSet * enumBasis = P.CreateStreamSet(ccs.size());
            P.CreateKernelFamilyCall<CharClassesKernel>(ccs, BasisBits, enumBasis);
            P.CreateKernelCall<BoundaryKernel>(enumBasis, IndexStream, PropertyStream);
        } else if (auto * obj = dyn_cast<UCD::BinaryPropertyObject>(propObj)) {
            std::vector<re::CC *> ccs = {makeCC(obj->GetCodepointSet("Y"), &cc::Unicode)};
            StreamSet * pStrm = P.CreateStreamSet(1);
            P.CreateKernelFamilyCall<CharClassesKernel>(ccs, BasisBits, pStrm);
            P.CreateKernelCall<BoundaryKernel>(pStrm, IndexStream, PropertyStream);
        } else {
            llvm::report_fatal_error("Unsupported property for boundary expression");
        }
    }
}

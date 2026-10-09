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
#include <re/analysis/re_name_gather.h>
#include <re/analysis/capture-ref.h>
#include <re/printer/re_printer.h>
#include <re/toolchain/toolchain.h>
#include <re/unicode/regex_passes.h>
#include <re/transforms/to_utf8.h>
#include <re/transforms/re_multiplex.h>
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
    mMatchStarts(nullptr), mMatchFollows(nullptr), mLengthAlphabet(nullptr), mIndexStream(nullptr),
    mCombiningType(RE_CombiningType::None), mCombiningStream(nullptr) {}

void RE_CompilerContext::setCodeUnitContext(const cc::Alphabet * a, StreamSet * basis) {
    mCodeUnitAlphabet = a;
    mLengthAlphabet = a; // default
    mCodeUnitStream = basis;
    addAlphabet(a, basis);
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


RE_Kernel::RE_Kernel(LLVMTypeSystemInterface & ts, RE_CompilerContext & ctxt, RE * re, StreamSet * results)
: PabloKernel(ts, makeSignature(ctxt, re), makeInputBindings(ctxt, re), makeOutputBindings(re, results), {}, {}),
mContext(ctxt), mRE(re) {
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

Bindings RE_Kernel::makeOutputBindings(RE * re, StreamSet * results) {
    if (grepOffset(re) == 0) {
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
    RE_Compiler re_compiler(getEntryScope(), matchStarts, matchFollows, mContext.mCodeUnitAlphabet);
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

static RE_Mode determineREMode(RE * re, const RE_ModeOptions & opts) {
    RE_Mode mode;
    // Determine the unit of length for the RE.  If the RE involves
    // fixed length UTF-8 sequences only, then UTF-8 can be used
    // for most efficient processing.   Otherwise we must use full
    // Unicode length calculations.
    bool useFixedUTF8 = !opts.unicodeIndexingOverride && validateFixedUTF8(re) && !hasPropertyReference(re);
    useFixedUTF8 = useFixedUTF8 && !opts.forceUnicodeIndexing;
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
    if (useFixedUTF8) {
        mode.lengthAlphabet = &cc::UTF8;
        mode.indexAlphabet = &cc::UTF8;
    } else {
        mode.lengthAlphabet = &cc::Unicode;
        // Determine whether UTF8-indexed or Unicode-indexed streams will
        // be used for regular expression processing.
        bool useIndexedUTF8 = !opts.unicodeBasisOverride
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

PreparedRE prepareRE(RE * re, const RE_ModeOptions & opts) {
    PreparedRE prepared;
    // Property values given by regular expressions are resolved by a search
    // of the lines of property value names.
    re = resolveModesAndExternalSymbols(re, matchingLineNumbers);
    prepared.re = regular_expression_passes(re);
    prepared.mode = determineREMode(prepared.re, opts);
    prepared.options = opts;
    return prepared;
}

std::pair<int, int> PreparedRE::lengthRange() const {
    return getLengthRange(re, mode.lengthAlphabet);
}

bool PreparedRE::endAnchored() const {
    return hasEndAnchor(re);
}

unsigned PreparedRE::grepOffset() const {
    return re::grepOffset(re);
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

    if (mSourceContext.encoding == &cc::Unicode) {
        // Already fully Unicode-encoded by the caller (e.g. csvgrep); use as-is.
        mCtxt.setCodeUnitContext(&cc::Unicode, source);
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

    // Kept disabled (and dead) for now -- to be re-enabled in a future step.
    bool useMultiplexedUnicode = false; //!mModeOptions.unicodeBasisOverride && !hasCodepointReference(re);
    if (useMultiplexedUnicode) {
        const auto UnicodeSets = re::collectCCs(re, *mode.indexAlphabet);
        if (!UnicodeSets.empty()) {
            auto mpx = cc::makeMultiplexedAlphabet("mpx", UnicodeSets);
            re = transformCCs(mpx, re, re::NameTransformationMode::None);
            auto mpx_basis = mpx->getMultiplexedCCs();
            StreamSet * const u8CharClasses = mPB.CreateStreamSet(mpx_basis.size());
            mPB.CreateKernelFamilyCall<CharClassesKernel>(mpx_basis, source, u8CharClasses);
            StreamSet * const unicodeCCs = mPB.CreateStreamSet(mpx_basis.size());
            FilterByMask(mPB, mU8Index, u8CharClasses, unicodeCCs);
            mCtxt.setCodeUnitContext(mpx, unicodeCCs);
        }
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
    unsigned offset = grepOffset(defn);
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
                mPB.CreateKernelCall<StringClassKernel>(seg->strings, codeUnitBasis(), fillStartsEnds, mCtxt.mMatchFollows);
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
    mRE = applyModePasses(re);
    mRE = processReferences(mRE);
    prepareExternals(mRE);
    mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, mRE, results);
}

void RE_PipelineBuilder::matchSearchPipeline(const PreparedRE & prepared, StreamSet * results) {
    usePrepared(prepared);
    RE * re = prepared.re;
    ensurePrepared(re);
    mRE = applyModePasses(re);
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
    mRE = applyModePasses(re);
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
    } else if (!uniquePrefixSpans(re, spans)) {
        StreamSet * matchEnd = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, re, matchEnd);
        auto minlgth = getLengthRange(re, mCtxt.mLengthAlphabet).first;
        auto offset = grepOffset(re);
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
        if (mCodeUnitBasis->getNumElements() == 1) {
            mCodeUnitBasis = mPB.CreateStreamSet(8, 1);
            Selected_S2P(mPB, mCtxt.mCodeUnitStream, mCodeUnitBasis);
        }
    }
    return mCodeUnitBasis;
}

StreamSet * RE_PipelineBuilder::classStream(RE * re, CC * cc) {
    // The class may already be an external (e.g. a property), whose kernel
    // must then be the one used.
    if (Name * const n = dyn_cast<Name>(re)) {
        auto f = mCtxt.mExternals.find(n->getFullName());
        if (f != mCtxt.mExternals.end()) return f->second.extStream;
    }
    StreamSet * const strm = mPB.CreateStreamSet(1);
    mPB.CreateKernelFamilyCall<CharClassesKernel>(std::vector<re::CC *>{cc}, codeUnitBasis(), strm);
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
    mPB.CreateKernelCall<LongestSpan>(pfxLgth + pfxOffset - 1, grepOffset(split), pfxStrm, endBack, matchEnd, spans);
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

// Is r a lookbehind assertion (see definedAssertion)?
bool isLookbehind(RE * r) {
    Assertion * const a = definedAssertion(r);
    return a && (a->getKind() == Assertion::Kind::LookBehind);
}
}

bool RE_PipelineBuilder::chainSpans(RE * re, StreamSet * spans) {
    // Chains are compiled with one position per code unit.
    if (mCtxt.mIndexStream != nullptr) return false;
    std::vector<RE *> elems;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        elems.assign(seq->begin(), seq->end());
    } else {
        elems.push_back(re);
    }
    // Lookbehinds at the start hold at the starts of the matches: they are
    // compiled on their own and restrict the starts of the rest.
    const auto lookbehindEnd = std::find_if_not(elems.begin(), elems.end(), isLookbehind);
    const std::vector<RE *> lookbehinds(elems.begin(), lookbehindEnd);
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
    // Final stars, each of a class or of a string class: a final X{lb,} of a
    // class with lb > 0 is X{lb} followed by X*, where X{lb} ends the chain;
    // a string class with lb = 1 must be the first final star, and the
    // chain then ends where an occurrence begins.  (Kernels are created only
    // once the form is known.)
    struct Final {
        char kind;
        RE * re;
        CC * cc;
        std::vector<std::vector<CC *>> strings;
    };
    std::vector<Final> finals;   // from the right
    bool occurrenceRequired = false;
    while (!elems.empty()) {
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
    std::vector<LookaheadSegment> segments;
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
        if (!parseLookaheadChain(makeSeq(elems.begin(), elems.end()), mCtxt.mLengthAlphabet, segments, false)) {
            return false;
        }
        if (occurrenceRequired) segments.pop_back();
        if (segments.empty()) return false;
    }
    std::vector<ChainCoverage::Final> finalSteps;
    for (auto f = finals.rbegin(); f != finals.rend(); ++f) {
        StreamSet * strm = nullptr;
        if (f->kind == 's') {
            strm = classStream(f->re, f->cc);
        } else {
            strm = mPB.CreateStreamSet(3);
            mPB.CreateKernelCall<StringClassKernel>(f->strings, codeUnitBasis(), strm, mCtxt.mMatchFollows);
        }
        finalSteps.push_back(ChainCoverage::Final{f->kind, strm});
    }
    // The starts of the occurrences of the first final star, if one is required.
    StreamSet * Hend = nullptr;
    if (occurrenceRequired) {
        Hend = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<StreamSelect>(Hend, Select(finalSteps.front().strm, {1}));
    }
    std::vector<ChainStepStreams> rec;
    StreamSet * starts = segments.empty() ? Hend : chainMatchStarts(segments, &rec, Hend);
    if (!lookbehinds.empty()) {
        StreamSet * const holds = mPB.CreateStreamSet(1);
        mPB.CreateKernelFamilyCall<RE_Kernel>(mCtxt, makeSeq(lookbehinds.begin(), lookbehinds.end()), holds);
        StreamSet * const restricted = mPB.CreateStreamSet(1);
        AndCombine(mPB, starts, holds, restricted);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            mPB.captureBitstream("chainStarts", starts);
            mPB.captureBitstream("lookbehindHolds", holds);
        }
        starts = restricted;
    }
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

RE * RE_PipelineBuilder::applyModePasses(RE * re) {
    const cc::Alphabet * lengthAlphabet = mCtxt.mLengthAlphabet;
    RE * xfrmedRE = expandPermutes(re);
    xfrmedRE = regular_expression_passes(xfrmedRE, lengthAlphabet);

    UCD::PropertyExternalizer PE;
    xfrmedRE = PE.transformRE(xfrmedRE);
    for (auto m : PE.mNameMap) {
        if (re::PropertyExpression * pe = dyn_cast<re::PropertyExpression>(m.second)) {
            compileProperty(pe);
        }
    }

    // CCs of characters of more than one UTF-8 length are computed as externals
    // when the code units are UTF-8.  (With Unicode code units, every CC is one
    // code unit.)
    if (mCtxt.mCodeUnitAlphabet == &cc::UTF8) {
        re::VariableLengthCCNamer CCnamer;
        xfrmedRE = CCnamer.transformRE(xfrmedRE);
        for (auto m : CCnamer.mNameMap) {
            std::vector<re::CC *> ccs = {cast<re::CC>(m.second)};
            StreamSet * ccStrm = mPB.CreateStreamSet(1);
            mPB.CreateKernelFamilyCall<CharClassesKernel>(ccs, mCtxt.mCodeUnitStream, ccStrm);
            addExternal(m.first, ExternalStream{ExternalStreamKind::FixedLength, 0, {1, 1}, ccStrm});
        }
    }

    if (mCtxt.mCodeUnitAlphabet == &cc::UTF8) {
        xfrmedRE = toUTF8(xfrmedRE);
    }

    // Unbounded repetitions of suitable string classes are matched using
    // the Fill, Starts and Ends of the string class.
    {
        re::StringClassRepNamer SCnamer(mCtxt.mCodeUnitAlphabet);
        xfrmedRE = SCnamer.transformRE(xfrmedRE);
        for (auto & m : SCnamer.mStrings) {
            StreamSet * fillStartsEnds = mPB.CreateStreamSet(3);
            mPB.CreateKernelCall<StringClassKernel>(m.second, codeUnitBasis(), fillStartsEnds);
            addExternal(m.first, ExternalStream{ExternalStreamKind::StringClassRep, 0, {0, 0}, fillStartsEnds});
        }
    }

    // Lookahead lengths are measured in the mode's length alphabet.
    re::LookAheadNamer LA(*lengthAlphabet);
    xfrmedRE = LA.transformRE(xfrmedRE);

    return xfrmedRE;
}

RE * RE_PipelineBuilder::processReferences(RE * re) {
    re::ReferenceInfo mRefInfo = re::buildReferenceInfo(re);
    if (!mRefInfo.twixtREs.empty()) {
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

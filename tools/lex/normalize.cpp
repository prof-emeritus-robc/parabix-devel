/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "normalize.h"
#include "pretokenizer.h"

#include <kernel/unicode/utf8gen.h>
#include <re/adt/adt.h>
#include <re/parse/parser.h>
#include <re/transforms/re_simplifier.h>
#include <re/unicode/resolve_properties.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/sentinel.h>
#include <kernel/streamutils/stream_shift.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <llvm/Support/Casting.h>
#include <kernel/unicode/utf8_decoder.h>
#include <unicode/utf/transchar.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>


using namespace kernel;
using namespace pablo;

// InvertMaskKernel — NOT of a single-bit stream.

class InvertMaskKernel : public PabloKernel {
public:
    InvertMaskKernel(LLVMTypeSystemInterface & ts,
                     StreamSet * input,
                     StreamSet * output)
    : PabloKernel(ts, "InvertMask",
                  {Binding{"input",  input}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * in = getInputStreamSet("input")[0];
        writeOutputStreamSet("output", std::vector<PabloAST*>{pb.createNot(in)});
    }
};

// StripLeadingMaskKernel
//
// Outputs a delete mask for leading whitespace.
// leading[k] = 1 iff k < firstNonWS  =  NOT MatchStar(nonWS, ones)
//
// When the entire stream is whitespace, nonWS = 0 everywhere, MatchStar = 0,
// NOT = all-ones — correctly marks the whole stream for deletion.

class StripLeadingMaskKernel : public PabloKernel {
public:
    StripLeadingMaskKernel(LLVMTypeSystemInterface & ts,
                           StreamSet * nonWS,
                           StreamSet * output)
    : PabloKernel(ts, "StripLeadingMask",
                  {Binding{"nonWS", nonWS}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * nonWs = getInputStreamSet("nonWS")[0];
        PabloAST * ones  = pb.createNot(pb.createZeroes());
        writeOutputStreamSet("output",
            std::vector<PabloAST*>{pb.createNot(pb.createMatchStar(nonWs, ones))});
    }
};

// StripTrailingMaskKernel
//
// Outputs a delete mask for trailing whitespace.
// lastNWS: a single 1 at the last non-whitespace position K (from IndexedShiftBack).
// trailing[k] = 1 iff k > K  =  MatchStar(Advance(lastNWS, 1), ones) AND wsSpans
//
// Edge case: if the input is all whitespace, lastNWS is all-zero, so trailing = 0
// (nothing deleted). In practice NormStrip (both sides) still works because
// StripLeadingMaskKernel covers the all-WS case for the combined mode.

class StripTrailingMaskKernel : public PabloKernel {
public:
    StripTrailingMaskKernel(LLVMTypeSystemInterface & ts,
                            StreamSet * lastNWS,
                            StreamSet * wsSpans,
                            StreamSet * output)
    : PabloKernel(ts, "StripTrailingMask",
                  {Binding{"lastNWS", lastNWS},
                   Binding{"wsSpans", wsSpans}},
                  {Binding{"output",  output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * last = getInputStreamSet("lastNWS")[0];
        PabloAST * ws   = getInputStreamSet("wsSpans")[0];
        PabloAST * ones = pb.createNot(pb.createZeroes());
        // Advance(last, 1)[k] = last[k-1] = 1 iff k = K+1
        // MatchStar extends that mark forward through all positions (ones as carry set)
        PabloAST * afterK = pb.createMatchStar(pb.createAdvance(last, 1), ones);
        writeOutputStreamSet("output",
            std::vector<PabloAST*>{pb.createAnd(afterK, ws)});
    }
};

// OrMaskKernel,  bitwise OR of two single-bit streams.
// Used to combine leading and trailing delete masks.

class OrMaskKernel : public PabloKernel {
public:
    OrMaskKernel(LLVMTypeSystemInterface & ts,
                 StreamSet * a, StreamSet * b, StreamSet * output)
    : PabloKernel(ts, "OrMask",
                  {Binding{"a", a}, Binding{"b", b}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * a = getInputStreamSet("a")[0];
        PabloAST * b = getInputStreamSet("b")[0];
        writeOutputStreamSet("output", std::vector<PabloAST*>{pb.createOr(a, b)});
    }
};

// applyStripAccents
//
// Removes all Unicode Mn (Mark, Nonspacing) characters from the byte stream.
// Should be called after NFD normalization so that combining accents are
// isolated codepoints.
//
// Pipeline:
//   BasisBits
//     → UTF8_index          → u8index   (last byte of each UTF-8 char)
//     → UnicodePropertyKernelBuilder(\p{Mn})
//                           → MnMask    (last byte of each Mn char)
//     → U8Spans             → MnSpans   (all bytes of each Mn char)
//     → InvertMaskKernel    → KeepMask  (1 = non-Mn byte, keep)
//     → FilterByMask        → filtered BasisBits (Mn chars removed)

static StreamSet * applyStripAccents(PipelineBuilder & P, StreamSet * BasisBits) {

    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    re::RE * mnRE = re::simplifyRE(re::RE_Parser::parse("\\p{Mn}"));
    mnRE = UCD::linkAndResolve(mnRE);
    mnRE = UCD::externalizeProperties(mnRE);
    re::Name * mnName = llvm::cast<re::Name>(mnRE);

    StreamSet * MnMask = P.CreateStreamSet(1, 1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(mnName, BasisBits, MnMask);

    StreamSet * MnSpans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(MnMask, u8index, MnSpans);

    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(MnSpans, KeepMask);

    StreamSet * FilteredBasis = P.CreateStreamSet(8, 1);
    FilterByMask(P, KeepMask, BasisBits, FilteredBasis);

    return FilteredBasis;
}

// applyStrip
//
// Removes Unicode White_Space characters from the left edge, right edge, or both.
//
// Pipeline:
//   BasisBits
//     → UTF8_index                               → u8index
//     → UnicodePropertyKernelBuilder(\p{White_Space})
//                                                → WS_Mask  (last byte of each ws char)
//     → U8Spans                                  → WS_Spans (all bytes of each ws char)
//     → InvertMaskKernel                         → nonWS
//
//   Leading path (stripLeading):
//     → StripLeadingMaskKernel                   → leadDelete
//
//   Trailing path (stripTrailing):
//     nonWS → AddSentinel                        → nonWS_s  (N+1, sentinel at N)
//     nonWS → EOFbit                             → eofMark  (N+1, 1 only at N)
//     nonWS_s + eofMark → IndexedShiftBack       → lastNWS  (1 at last nonWS pos K)
//     lastNWS + WS_Spans → StripTrailingMaskKernel → trailDelete
//
//   Combine → InvertMaskKernel → KeepMask → FilterByMask → filtered BasisBits

static StreamSet * applyStrip(PipelineBuilder & P, StreamSet * BasisBits,
                               bool stripLeading, bool stripTrailing) {

    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    re::RE * wsRE = re::simplifyRE(re::RE_Parser::parse("\\p{White_Space}"));
    wsRE = UCD::linkAndResolve(wsRE);
    wsRE = UCD::externalizeProperties(wsRE);
    re::Name * wsName = llvm::cast<re::Name>(wsRE);

    StreamSet * WS_Mask = P.CreateStreamSet(1, 1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(wsName, BasisBits, WS_Mask);

    StreamSet * WS_Spans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(WS_Mask, u8index, WS_Spans);

    StreamSet * nonWS = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(WS_Spans, nonWS);

    StreamSet * deleteMask = nullptr;

    if (stripLeading) {
        StreamSet * leadDelete = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<StripLeadingMaskKernel>(nonWS, leadDelete);
        deleteMask = leadDelete;
    }

    if (stripTrailing) {
        // Sentinel approach: shift the EOF marker back through the nonWS index
        // to land at the last nonWS position K.
        StreamSet * nonWS_s = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<AddSentinel>(nonWS, nonWS_s);

        StreamSet * eofMark = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<EOFbit>(nonWS, eofMark);

        StreamSet * lastNWS = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<IndexedShiftBack>(nonWS_s, eofMark, lastNWS);

        StreamSet * trailDelete = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<StripTrailingMaskKernel>(lastNWS, WS_Spans, trailDelete);

        if (deleteMask != nullptr) {
            StreamSet * combined = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<OrMaskKernel>(deleteMask, trailDelete, combined);
            deleteMask = combined;
        } else {
            deleteMask = trailDelete;
        }
    }

    if (deleteMask == nullptr) return BasisBits;

    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(deleteMask, KeepMask);

    StreamSet * FilteredBasis = P.CreateStreamSet(8, 1);
    FilterByMask(P, KeepMask, BasisBits, FilteredBasis);

    return FilteredBasis;
}

// applyByteLevel — GPT-2 byte alphabet normalization.
//
// Maps every input byte to a unique printable Unicode codepoint using the GPT-2
// byte alphabet (same mapping as the bytelevel pre-tokenizer), then re-encodes
// the result as UTF-8.  The output byte stream is a valid UTF-8 string where
// every original byte is represented by exactly one printable character.

static StreamSet * applyByteLevel(PipelineBuilder & P, StreamSet * BasisBits) {
    StreamSet * codepoints = applyByteLevelEncoding(P, BasisBits);
    StreamSet * OutputBasis = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, codepoints, OutputBasis);
    return OutputBasis;
}

// applyLowercase — map all uppercase codepoints to lowercase using SLC (1-to-1).
//
// Pipeline:
//   BasisBits
//     → UTF8_Decoder   → U21          (21-bit Unicode basis, u8final-indexed)
//     → UTF8_index     → u8index      (1 at the last byte of each UTF-8 sequence)
//     → FilterByMask   → U21_focus    (character-indexed: one position per codepoint)
//     → LC_Translation → LC_U21       (XOR masks applied to flip uppercase bits)
//     → U21_to_UTF8    → Output       (re-encoded UTF-8)

struct Lowercase_BixData {
    Lowercase_BixData() {
        auto * slc_obj = llvm::cast<UCD::CodePointPropertyObject>(getPropertyObject(UCD::slc));
        mLC1_Sets = slc_obj->GetBitTransformSets();
    }
    unicode::BitTranslationSets LC_1st_BitXorCCs() {
        return mLC1_Sets;
    }
    unicode::BitTranslationSets mLC1_Sets;
};

class LC_Translation : public PabloKernel {
public:
    LC_Translation(LLVMTypeSystemInterface & ts, Lowercase_BixData data,
                   StreamSet * Basis, StreamSet * Output)
    : PabloKernel(ts, "LC_Translation" + std::to_string(Basis->getNumElements()) + "x1",
                  {Binding{"basis", Basis}},
                  {Binding{"Output", Output}}), mData(std::move(data)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        UTF::UTF_Compiler unicodeCompiler(getInput(0), pb);
        unicode::BitTranslationSets LC1 = mData.LC_1st_BitXorCCs();   // UnicodeSet
        std::vector<Var *> LC1_Vars(LC1.size());    //  mask bitstream
        std::vector<Var *> all_targets(LC1.size()); 
        std::vector<UCD::UnicodeSet> all_CCs(LC1.size());
        for (unsigned i = 0; i < LC1.size(); i++) {  
            Var * v = pb.createVar("LC1_bit" + std::to_string(i), pb.createZeroes());
            LC1_Vars[i] = v;
            all_targets[i] = v;  
            all_CCs[i] = LC1[i];   // UnicodeSet - which characters need bit i flipped?
        }
        unicodeCompiler.compile(all_targets, all_CCs);
        std::vector<PabloAST *> basis = getInputStreamSet("basis");
        Var * outputVar = getOutputStreamVar("Output");

        // loop over all 21 bit positions and decides what to write to the output:
        for (unsigned i = 0; i < basis.size(); i++) { 
            PabloAST * out = (i < LC1.size())
                ? pb.createXor(basis[i], LC1_Vars[i])
                : basis[i];  .// pass-through
            pb.createAssign(pb.createExtract(outputVar, pb.getInteger(i)), out);
        }
    }
    Lowercase_BixData mData;
};

static StreamSet * applyLowercase(PipelineBuilder & P, StreamSet * BasisBits) {
    // decode the raw UTF-8 bytes into 21-bit Unicode codepoint values
    StreamSet * U21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21);

    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    // Remove the multi-byte padding, there's exactly one position per codepoint.
    StreamSet * U21_focus = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21, U21_focus);

    Lowercase_BixData lc_data; //  loads the XOR masks
    StreamSet * LC_U21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<LC_Translation>(lc_data, U21_focus, LC_U21);

    StreamSet * Output = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, LC_U21, Output);
    return Output;
}

// applyNFD — convert UTF-8 input to NFD form.

static StreamSet * applyNFD(PipelineBuilder & P, StreamSet * BasisBits) {
    NFD_PipelineBuilder nfd(P);
    StreamSet * TransformedBasis = P.CreateStreamSet(8, 1);
    nfd.NFD_U8_Pipeline(BasisBits, TransformedBasis);
    return TransformedBasis;
}

// applyNormalization — public dispatch function
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               NormalizationMode mode) {

    if (mode == NormByteLevel)    return applyByteLevel(P, BasisBits);
    if (mode == NormStripAccents) return applyStripAccents(P, applyNFD(P, BasisBits));
    if (mode == NormStripLeft)    return applyStrip(P, BasisBits, true,  false);
    if (mode == NormStripRight)   return applyStrip(P, BasisBits, false, true);
    if (mode == NormStrip)        return applyStrip(P, BasisBits, true,  true);

    if (mode == NormNFD) return applyNFD(P, BasisBits);
    if (mode == NormLowercase) return applyLowercase(P, BasisBits);
    //if (mode == NormNFC) return applyNFC(P, BasisBits);

    return BasisBits;  // NormNone — pass-through
}

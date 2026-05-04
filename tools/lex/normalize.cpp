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
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/char_replacement.h>

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

// NmtReplaceKernel21 — at ReplaceMask positions, force the 21-bit codepoint to 0x20 (space).
// 0x20 in binary: only bit 5 is set, all other bits are 0.
//   - At replace positions: bit 5 = 1, others = 0   (= the codepoint 0x20)
//   - At non-replace positions: each bit = original  (codepoint unchanged)
class NmtReplaceKernel21 : public PabloKernel {
public:
    NmtReplaceKernel21(LLVMTypeSystemInterface & ts,
                       StreamSet * U21Basis,
                       StreamSet * ReplaceMask,
                       StreamSet * Output)
    : PabloKernel(ts, "NmtReplace21",
                  {Binding{"basis",   U21Basis},
                   Binding{"replace", ReplaceMask}},
                  {Binding{"output",  Output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST*> basis = getInputStreamSet("basis");
        PabloAST * replace = getInputStreamSet("replace")[0];
        Var * out = getOutputStreamVar("output");

        for (unsigned i = 0; i < 21; i++) {
            PabloAST * v = (i == 5)
                ? pb.createOr(basis[i], replace)              // bit 5: replace=1 → 1, else original
                : pb.createAnd(basis[i], pb.createNot(replace)); // others: replace=1 → 0, else original
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), v);
        }
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

// applyLowercase — map all uppercase codepoints to lowercase
// using full Unicode Lower_Case (UCD::lc) via U21_StringOverridePipeline.
static StreamSet * applyLowercase(PipelineBuilder & P, StreamSet * BasisBits) {
    // decode the raw UTF-8 bytes into 21-bit Unicode codepoint values
    StreamSet * U21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21);

    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    // Remove the multi-byte padding, there's exactly one position per codepoint.
    StreamSet * U21_focus = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21, U21_focus);

    // Full Unicode Lower_Case — handles 1→N expansions like İ → i\u0307.
    StreamSet * LC_U21 = U21_StringOverridePipeline(P, UCD::lc, U21_focus);

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

// applyDeleteAndSpaceReplace — generic per-codepoint cleanup pipeline.
// Deletes codepoints matching delCC, replaces codepoints matching repCC with
// U+0020 (space), keeps everything else unchanged.  Reused by applyNmt and
// applyBertCleanText since both have this exact shape, just with different CCs.
static StreamSet * applyDeleteAndSpaceReplace(PipelineBuilder & P,
                                              StreamSet * BasisBits,
                                              re::CC * delCC,
                                              re::CC * repCC) {
    // Decode bytes → U21 codepoints (with multi-byte padding).
    StreamSet * U21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21);

    // u8index = 1 at the LAST byte of each UTF-8 char.
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    // U21_focus: one position per codepoint (no multi-byte padding).
    StreamSet * U21_focus = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21, U21_focus);

    StreamSet * DelMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(
        std::vector<re::CC*>{delCC}, U21_focus, DelMask);

    StreamSet * RepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(
        std::vector<re::CC*>{repCC}, U21_focus, RepMask);

    // At RepMask positions, force codepoint to 0x20 (space).
    StreamSet * ReplacedU21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<NmtReplaceKernel21>(U21_focus, RepMask, ReplacedU21);

    // Filter out delete positions: KeepMask = NOT DelMask
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(DelMask, KeepMask);

    StreamSet * FilteredU21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, ReplacedU21, FilteredU21);

    // Re-encode codepoints → UTF-8 bytes.
    StreamSet * Output = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, FilteredU21, Output);
    return Output;
}

// applyNmt — Google NMT preprocessing.
// Deletes a fixed set of control characters and replaces a fixed set of
// whitespace/special characters with U+0020 (space).
static StreamSet * applyNmt(PipelineBuilder & P, StreamSet * BasisBits) {
    // DELETE: U+0001-U+0008, U+000B, U+000E-U+001F, U+007F
    re::CC * delCC = re::makeCC(0x01, 0x08);
    delCC = re::makeCC(delCC, re::makeCC(0x0B));
    delCC = re::makeCC(delCC, re::makeCC(0x0E, 0x1F));
    delCC = re::makeCC(delCC, re::makeCC(0x7F));

    // REPLACE: tab, LF, FF, CR + Unicode whitespace/special chars
    re::CC * repCC = re::makeCC(0x09, 0x0A);
    repCC = re::makeCC(repCC, re::makeCC(0x0C, 0x0D));
    repCC = re::makeCC(repCC, re::makeCC(0x1680));
    repCC = re::makeCC(repCC, re::makeCC(0x200B, 0x200F));
    repCC = re::makeCC(repCC, re::makeCC(0x2028, 0x2029));
    repCC = re::makeCC(repCC, re::makeCC(0x2581));
    repCC = re::makeCC(repCC, re::makeCC(0xFEFF));
    repCC = re::makeCC(repCC, re::makeCC(0xFFFD));

    return applyDeleteAndSpaceReplace(P, BasisBits, delCC, repCC);
}

// applyBertCleanText — BERT BasicTokenizer's _clean_text.
// Deletes \p{C} codepoints (control/format/surrogate/private/unassigned) plus
// U+FFFD, EXCEPT \t \n \r which are kept-but-replaced.  Replaces \p{Zs} (space
// separator) plus \t \n \r with U+0020.  Note: does NOT collapse runs.
static StreamSet * applyBertCleanText(PipelineBuilder & P, StreamSet * BasisBits) {
    auto * gcObj = llvm::cast<UCD::EnumeratedPropertyObject>(
        UCD::getPropertyObject(UCD::gc));

    // Delete = \p{C} - {\t, \n, \r} + {U+FFFD}.
    // Build \p{C} as the union of its five subcategories (Cc, Cf, Cs, Co, Cn).
    UCD::UnicodeSet delSet;
    delSet.insert(gcObj->GetCodepointSet("Cc"));
    delSet.insert(gcObj->GetCodepointSet("Cf"));
    delSet.insert(gcObj->GetCodepointSet("Cs"));
    delSet.insert(gcObj->GetCodepointSet("Co"));
    delSet.insert(gcObj->GetCodepointSet("Cn"));
    delSet = delSet - UCD::UnicodeSet(0x09)   // \t
                    - UCD::UnicodeSet(0x0A)   // \n
                    - UCD::UnicodeSet(0x0D);  // \r
    delSet.insert(0xFFFD);
    re::CC * delCC = re::makeCC(delSet, &cc::Unicode);

    // Replace = \p{Zs} + {\t, \n, \r} - with space.
    UCD::UnicodeSet repSet = gcObj->GetCodepointSet("Zs");
    repSet.insert(0x09);
    repSet.insert(0x0A);
    repSet.insert(0x0D);
    re::CC * repCC = re::makeCC(repSet, &cc::Unicode);

    return applyDeleteAndSpaceReplace(P, BasisBits, delCC, repCC);
}

// applyOneNormalization — dispatch a single mode to the right kernel.
StreamSet * applyOneNormalization(PipelineBuilder & P,
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
     if (mode == NormNmt) return applyNmt(P, BasisBits);
    if (mode == NormBertCleanText) return applyBertCleanText(P, BasisBits);

    return BasisBits;  // NormNone — pass-through
}

// applyNormalization — apply a sequence of normalizations in order.
// normalizers.Sequence([N1, N2, ...]).
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               const std::vector<NormalizationMode> & modes) {
    for (NormalizationMode m : modes) {
        BasisBits = applyOneNormalization(P, BasisBits, m);
    }
    return BasisBits;
} 

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
#include <ucd/utf/transchar.h>
#include <ucd/utf/utf_compiler.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/char_replacement.h>
#include <kernel/streamutils/pdep_kernel.h>

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


// ---------------------------------------------------------------------------
// Shared helpers — decode once at the top, encode once at the bottom.
// ---------------------------------------------------------------------------

// decodeToU21: UTF-8 bytes → one U21 codepoint per stream position.
static StreamSet * decodeToU21(PipelineBuilder & P, StreamSet * BasisBits) {
    StreamSet * U21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21);
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);
    StreamSet * U21_focus = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21, U21_focus);
    return U21_focus;
}

// encodeFromU21: U21 codepoints → UTF-8 bytes.
static StreamSet * encodeFromU21(PipelineBuilder & P, StreamSet * U21_focus) {
    StreamSet * Output = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, U21_focus, Output);
    return Output;
}

// applyStripAccents — remove all accent/combining marks (Unicode category Mn).
//
// Now works entirely in U21 space (one slot = one codepoint), so we never
// need to think about how many bytes a character takes.
//
// Pipeline:
//   U21_focus
//     → NFD_U21_Pipeline   → NFD_U21   decompose é → e + ◌́  so each accent
//                                       is now isolated in its own slot
//     → CharClassesKernel  → MnMask    1 at every slot that holds an Mn
//                                       (Mark, Nonspacing) codepoint
//     → InvertMaskKernel   → KeepMask  flip: 1 = "keep this slot"
//     → FilterByMask       → result    squeeze out the Mn slots; only the
//                                       base characters remain
static StreamSet * applyStripAccents(PipelineBuilder & P, StreamSet * U21_focus) {

    // Step 1 — Canonical decomposition (NFD) in U21 space.
    // NFD_U21_Pipeline expands precomposed characters so every combining mark
    // gets its own slot.  e.g. U+00E9 (é) → U+0065 (e) + U+0301 (combining acute).
    NFD_PipelineBuilder nfd(P);
    StreamSet * NFD_U21 = nfd.NFD_U21_Pipeline(U21_focus);

    // Step 2 — Build the Mn character class from the Unicode General Category table.
    // We look up the "Mn" (Mark, Nonspacing) set — all combining diacritics, accents,
    // dots, etc. — directly from the UCD property object so the list is always correct
    // for the Unicode version Parabix was built with.
    auto * gcObj = llvm::cast<UCD::EnumeratedPropertyObject>(
        UCD::getPropertyObject(UCD::gc));
    re::CC * mnCC = re::makeCC(gcObj->GetCodepointSet("Mn"), &cc::Unicode);

    // Step 3 — Mark every slot that holds an Mn codepoint.
    // CharClassesKernel takes our NFD stream (one codepoint per slot) and outputs
    // a 1-bit stream: 1 where the codepoint is in mnCC, 0 everywhere else.
    StreamSet * MnMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC*>{mnCC}, NFD_U21, MnMask);

    // Step 4 — Invert: we want to KEEP non-Mn slots, not delete them.
    // InvertMaskKernel flips every bit: the 0s (non-Mn) become 1s (keep).
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(MnMask, KeepMask);

    // Step 5 — Filter: remove all slots where KeepMask = 0 (the accent slots).
    // FilterByMask compresses the stream, leaving only the kept codepoints.
    StreamSet * result = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, NFD_U21, result);
    return result;
}

// applyStrip — remove Unicode White_Space from the left, right, or both edges.
//
// In U21 space every slot is one codepoint, so we just ask "is this slot
// a whitespace character?" with CharClassesKernel — no UTF8_index or U8Spans
// needed.  The leading/trailing logic (MatchStar, IndexedShiftBack) is
// unchanged — it only cares about a 1-bit yes/no stream, not about bytes.
//
// Pipeline:
//   U21_focus
//     → CharClassesKernel(wsCC)  → WS_Mask   1 at every whitespace codepoint
//     → InvertMaskKernel         → nonWS     1 at every non-whitespace codepoint
//
//   Leading path (stripLeading):
//     nonWS → StripLeadingMaskKernel          → leadDelete
//
//   Trailing path (stripTrailing):
//     nonWS → AddSentinel                     → nonWS_s
//     nonWS → EOFbit                          → eofMark
//     nonWS_s + eofMark → IndexedShiftBack    → lastNWS  (1 at the last non-ws slot)
//     lastNWS + WS_Mask → StripTrailingMask   → trailDelete
//
//   Combine → InvertMaskKernel → KeepMask → FilterByMask → result (U21)
static StreamSet * applyStrip(PipelineBuilder & P, StreamSet * U21_focus,
                               bool stripLeading, bool stripTrailing) {

    // Step 1 — Build the Unicode White_Space character class.
    // These are the exact codepoints in the Unicode White_Space property
    // (stable since Unicode 6.3).  We hardcode them here because
    // CharClassesKernel works on U21 codepoints and needs a re::CC, not
    // a property name string.
    re::CC * wsCC = re::makeCC(0x09, 0x0D);              // HT LF VT FF CR
    wsCC = re::makeCC(wsCC, re::makeCC(0x20));            // SPACE
    wsCC = re::makeCC(wsCC, re::makeCC(0x85));            // NEL
    wsCC = re::makeCC(wsCC, re::makeCC(0xA0));            // NBSP
    wsCC = re::makeCC(wsCC, re::makeCC(0x1680));          // OGHAM SPACE MARK
    wsCC = re::makeCC(wsCC, re::makeCC(0x2000, 0x200A));  // EN QUAD … HAIR SPACE
    wsCC = re::makeCC(wsCC, re::makeCC(0x2028, 0x2029));  // LINE SEP / PARA SEP
    wsCC = re::makeCC(wsCC, re::makeCC(0x202F));          // NARROW NO-BREAK SPACE
    wsCC = re::makeCC(wsCC, re::makeCC(0x205F));          // MEDIUM MATH SPACE
    wsCC = re::makeCC(wsCC, re::makeCC(0x3000));          // IDEOGRAPHIC SPACE

    // Step 2 — Mark which slots are whitespace.
    // CharClassesKernel outputs a 1-bit stream: 1 = whitespace, 0 = not.
    // Because we're in U21 space, each slot is already one codepoint —
    // no byte-span extension needed.
    StreamSet * WS_Mask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC*>{wsCC}, U21_focus, WS_Mask);

    // Step 3 — Flip the mask to get non-whitespace positions.
    // The leading/trailing kernels below need to know WHERE the non-ws chars are.
    StreamSet * nonWS = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(WS_Mask, nonWS);

    StreamSet * deleteMask = nullptr;

    // Step 4a — Leading strip.
    // StripLeadingMaskKernel uses MatchStar to mark every slot before the
    // first non-whitespace slot as "delete".
    if (stripLeading) {
        StreamSet * leadDelete = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<StripLeadingMaskKernel>(nonWS, leadDelete);
        deleteMask = leadDelete;
    }

    // Step 4b — Trailing strip.
    // We find the LAST non-whitespace slot using a sentinel + IndexedShiftBack,
    // then mark everything after it that is whitespace as "delete".
    if (stripTrailing) {
        // AddSentinel appends a sentinel bit so IndexedShiftBack has a
        // guaranteed stopping point at the end of the stream.
        StreamSet * nonWS_s = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<AddSentinel>(nonWS, nonWS_s);

        // EOFbit produces a single 1 at the very last position of the stream.
        StreamSet * eofMark = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<EOFbit>(nonWS, eofMark);

        // IndexedShiftBack slides the EOF marker backwards through nonWS_s
        // until it lands on the last non-whitespace position K.
        StreamSet * lastNWS = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<IndexedShiftBack>(nonWS_s, eofMark, lastNWS);

        // StripTrailingMaskKernel marks every slot after K that is whitespace.
        // Note: we pass WS_Mask directly — in U21 space it already has
        // exactly one 1 per whitespace codepoint, no U8Spans needed.
        StreamSet * trailDelete = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<StripTrailingMaskKernel>(lastNWS, WS_Mask, trailDelete);

        if (deleteMask != nullptr) {
            // Both leading and trailing: OR the two delete masks together.
            StreamSet * combined = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<OrMaskKernel>(deleteMask, trailDelete, combined);
            deleteMask = combined;
        } else {
            deleteMask = trailDelete;
        }
    }

    // If neither flag was set, nothing to do — return the input unchanged.
    if (deleteMask == nullptr) return U21_focus;

    // Step 5 — Keep everything that is NOT marked for deletion.
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(deleteMask, KeepMask);

    // Step 6 — Compress: remove the deleted slots from the U21 stream.
    StreamSet * result = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, U21_focus, result);
    return result;
}
// applyByteLevel — GPT-2 byte alphabet normalization.
//
// This is the ONE exception in our U21 pipeline: it must work on raw bytes
// because it maps each individual byte (0x00-0xFF) to a unique printable
// Unicode codepoint.  If we stayed in U21, a character like 中 (one codepoint)
// would lose its three individual bytes — so we must go back to bytes first.
//
// Key insight: applyByteLevelEncoding already RETURNS U21 (one output codepoint
// per input byte), so we stay in U21 after it — no second encode needed.
// The old version wrongly called U21_to_UTF8 after it, which applyNormalization
// would then immediately decode back to U21 — a wasted round-trip.
static StreamSet * applyByteLevel(PipelineBuilder & P, StreamSet * U21_focus) {
    // Step 1 — Encode our current U21 stream back to UTF-8 bytes.
    // We need raw bytes because the mapping is per-byte, not per-codepoint.
    StreamSet * bytes = encodeFromU21(P, U21_focus);

    // Step 2 — Apply the GPT-2 byte alphabet mapping.
    // Each byte becomes one printable Unicode codepoint.
    // applyByteLevelEncoding returns a U21 stream directly — we're back in
    // U21 space and can continue chaining other normalizers.
    return applyByteLevelEncoding(P, bytes);
}
// applyLowercase — map all uppercase codepoints to lowercase
// using full Unicode Lower_Case (UCD::lc) via U21_StringOverridePipeline.
// Takes U21_focus directly — the old 6-line decode/encode wrapper is gone.
// U21_StringOverridePipeline handles 1-to-N expansions (some characters
// lowercase into two codepoints in certain languages).
static StreamSet * applyLowercase(PipelineBuilder & P, StreamSet * U21_focus) {

    // Full Unicode Lower_Case — handles 1→N expansions like İ → i\u0307.
    return U21_StringOverridePipeline(P, UCD::lc, U21_focus);
}
// applyNFD — canonical decomposition in U21 space.
// NFD_U21_Pipeline already exists in NFD_PipelineBuilder and returns U21
// directly, so we no longer need an intermediate byte streamset.
// e.g. U+00E9 (e-acute, one slot) becomes U+0065 + U+0301 (two slots).
static StreamSet * applyNFD(PipelineBuilder & P, StreamSet * U21_focus) {
    NFD_PipelineBuilder nfd(P);
    // Takes U21_focus (one codepoint per slot), returns the decomposed U21.
    return nfd.NFD_U21_Pipeline(U21_focus);
}
// applyNFKD — compatibility decomposition in U21 space.
// Same pattern as applyNFD but uses NFKD_U21_Pipeline, which applies the
// more aggressive compatibility decomposition on top of canonical decomposition.
// e.g. U+FB01 (fi ligature, one slot) becomes U+0066 + U+0069 (two slots).
static StreamSet * applyNFKD(PipelineBuilder & P, StreamSet * U21_focus) {
    NFD_PipelineBuilder nfd(P);
    // Takes U21_focus (one codepoint per slot), returns the decomposed U21.
    return nfd.NFKD_U21_Pipeline(U21_focus);
}
// applyDeleteAndSpaceReplace — shared engine for applyNmt and applyBertCleanText.
// Deletes every codepoint in delCC, replaces every codepoint in repCC with
// U+0020 (space), and passes everything else through unchanged.
//
// Now takes U21_focus directly — the old 7-line decode block at the top
// and 2-line encode at the bottom are gone.  The actual logic is unchanged.
static StreamSet * applyDeleteAndSpaceReplace(PipelineBuilder & P,
                                              StreamSet * U21_focus,
                                              re::CC * delCC,
                                              re::CC * repCC) {
    // Step 1 — Mark which codepoints to DELETE.
    // CharClassesKernel scans every slot in U21_focus and outputs 1 wherever
    // the codepoint falls inside delCC.
    StreamSet * DelMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(
        std::vector<re::CC*>{delCC}, U21_focus, DelMask);

    // Step 2 — Mark which codepoints to REPLACE with space.
    StreamSet * RepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(
        std::vector<re::CC*>{repCC}, U21_focus, RepMask);

    // Step 3 — Write U+0020 at every RepMask position.
    // NmtReplaceKernel21 forces bit 5 = 1 and all other bits = 0 (= 0x20)
    // at those positions, leaving all other slots untouched.
    StreamSet * ReplacedU21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<NmtReplaceKernel21>(U21_focus, RepMask, ReplacedU21);

    // Step 4 — Build a keep-mask: 1 = keep this slot, 0 = delete it.
    // We invert DelMask so that non-deleted slots are marked 1.
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(DelMask, KeepMask);

    // Step 5 — Compress: squeeze out the deleted slots.
    // FilterByMask removes every slot where KeepMask = 0.
    StreamSet * FilteredU21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, ReplacedU21, FilteredU21);
    return FilteredU21;
}

// applyNmt
// Deletes a fixed set of control characters and replaces a fixed set of
// whitespace/special characters with U+0020 (space).
// applyNmt — Google NMT preprocessing.
// Just builds the delete/replace character sets and hands them to the engine.
// Only change from before: parameter is now U21_focus instead of BasisBits.
static StreamSet * applyNmt(PipelineBuilder & P, StreamSet * U21_focus) {
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

    return applyDeleteAndSpaceReplace(P, U21_focus, delCC, repCC);
}

// applyBertCleanText — BERT BasicTokenizer's _clean_text.
// Deletes \p{C} codepoints (control/format/surrogate/private/unassigned) plus
// U+FFFD, EXCEPT \t \n \r which are kept-but-replaced.  Replaces \p{Zs} (space
// separator) plus \t \n \r with U+0020. 
// applyBertCleanText — BERT BasicTokenizer's _clean_text.
// Just builds the delete/replace character sets and hands them to the engine.
// Only change from before: parameter is now U21_focus instead of BasisBits.
static StreamSet * applyBertCleanText(PipelineBuilder & P, StreamSet * U21_focus) {
    auto * gcObj = llvm::cast<UCD::EnumeratedPropertyObject>(
        UCD::getPropertyObject(UCD::gc));

    // DELETE: all of \p{C} (five subcategories) minus \t \n \r, plus U+FFFD.
    // \p{C} = control/format/surrogate/private-use/unassigned codepoints.
    UCD::UnicodeSet delSet;
    delSet.insert(gcObj->GetCodepointSet("Cc"));
    delSet.insert(gcObj->GetCodepointSet("Cf"));
    delSet.insert(gcObj->GetCodepointSet("Cs"));
    delSet.insert(gcObj->GetCodepointSet("Co"));
    delSet.insert(gcObj->GetCodepointSet("Cn"));
    delSet = delSet - UCD::UnicodeSet(0x09)   // \t — kept but replaced
                    - UCD::UnicodeSet(0x0A)   // \n — kept but replaced
                    - UCD::UnicodeSet(0x0D);  // \r — kept but replaced
    delSet.insert(0xFFFD);
    re::CC * delCC = re::makeCC(delSet, &cc::Unicode);

    // REPLACE with space: \p{Zs} (space separators) plus \t \n \r.
    UCD::UnicodeSet repSet = gcObj->GetCodepointSet("Zs");
    repSet.insert(0x09);
    repSet.insert(0x0A);
    repSet.insert(0x0D);
    re::CC * repCC = re::makeCC(repSet, &cc::Unicode);

    return applyDeleteAndSpaceReplace(P, U21_focus, delCC, repCC);
}

// applyBertChineseChars — BERT handle_chinese_chars=True.
// Surrounds every CJK codepoint C with spaces: C → " C ".
// Two passes are required so that consecutive CJK chars each get their own
// surrounding spaces (e.g. 中文 → " 中  文 ", not " 中 文 ").
//
// Pass 1 — insert one space BEFORE each CJK:
//   U21_focus  →  UnitInsertionSpreadMask(CJK, Before)  →  preMask
//              →  SpreadByMask                          →  preSpread (0 at inserted slots)
//              →  NmtReplaceKernel21(NOT preMask)       →  preResult (space at inserted slots)
//
// Pass 2 — insert one space AFTER each CJK (tracking CJK through Pass 1 via spread):
//   CJK_in_pre →  UnitInsertionSpreadMask(CJK_in_pre, After)  →  postMask
//   preResult  →  SpreadByMask                                →  postSpread
//              →  NmtReplaceKernel21(NOT postMask)            →  postResult
//
//   postResult  →  returned as U21 (applyNormalization encodes at the end)
// Takes U21_focus directly — the 5-line decode at the top and the 2-line
// encode at the bottom are gone. Every step inside was already U21.
static StreamSet * applyBertChineseChars(PipelineBuilder & P, StreamSet * U21_focus) {
    // CJK ranges — identical to HuggingFace BertTokenizer._is_chinese_char
    re::CC * cjkCC = re::makeCC(0x3400,  0x4DBF);   // Extension A
    cjkCC = re::makeCC(cjkCC, re::makeCC(0x4E00,  0x9FFF));   // Unified Ideographs
    cjkCC = re::makeCC(cjkCC, re::makeCC(0xF900,  0xFAFF));   // Compatibility Ideographs
    cjkCC = re::makeCC(cjkCC, re::makeCC(0x20000, 0x2A6DF));  // Extension B
    cjkCC = re::makeCC(cjkCC, re::makeCC(0x2A700, 0x2B73F));  // Extension C
    cjkCC = re::makeCC(cjkCC, re::makeCC(0x2B740, 0x2B81F));  // Extension D
    cjkCC = re::makeCC(cjkCC, re::makeCC(0x2B820, 0x2CEAF));  // Extension E
    cjkCC = re::makeCC(cjkCC, re::makeCC(0x2F800, 0x2FA1F));  // Compatibility Supplement

    StreamSet * CJK_Mask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC*>{cjkCC}, U21_focus, CJK_Mask);

    // Pass 1: insert a space BEFORE each CJK character
    StreamSet * preMask = P.CreateStreamSet(1, 1);
    UnitInsertionSpreadMask(P, CJK_Mask, preMask, InsertPosition::Before);

    StreamSet * preSpread = P.CreateStreamSet(21, 1);
    SpreadByMask(P, preMask, U21_focus, preSpread);

    // Spread CJK_Mask into the expanded stream so Pass 2 knows where CJK chars landed
    StreamSet * CJK_in_pre = P.CreateStreamSet(1, 1);
    SpreadByMask(P, preMask, CJK_Mask, CJK_in_pre);

    // Inserted slots have 0 in preSpread; OR bit 5 at those positions → U+0020
    StreamSet * insertMark1 = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(preMask, insertMark1);
    StreamSet * preResult = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<NmtReplaceKernel21>(preSpread, insertMark1, preResult);

    // Pass 2: insert a space AFTER each CJK character
    StreamSet * postMask = P.CreateStreamSet(1, 1);
    UnitInsertionSpreadMask(P, CJK_in_pre, postMask, InsertPosition::After);

    StreamSet * postSpread = P.CreateStreamSet(21, 1);
    SpreadByMask(P, postMask, preResult, postSpread);

    StreamSet * insertMark2 = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(postMask, insertMark2);
    StreamSet * postResult = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<NmtReplaceKernel21>(postSpread, insertMark2, postResult);

    // Return U21 directly — no encode needed, applyNormalization handles that.
    return postResult;
}

// applyOneNormalization — dispatch a single mode in U21 space.
// Takes U21_focus (one codepoint per slot) and returns a transformed U21 stream.
// Every normalizer above now speaks U21, so this is a pure U21-to-U21 dispatch.
static StreamSet * applyOneNormalization(PipelineBuilder & P,
                                         StreamSet * U21_focus,
                                         NormalizationMode mode) {
    // NormStripAccents: NFD decompose first so accents are isolated, then delete Mn.
    // Both applyNFD and applyStripAccents now work in U21, so we chain them directly.
    if (mode == NormStripAccents)     return applyStripAccents(P, applyNFD(P, U21_focus));

    if (mode == NormByteLevel)        return applyByteLevel(P, U21_focus);
    if (mode == NormStripLeft)        return applyStrip(P, U21_focus, true,  false);
    if (mode == NormStripRight)       return applyStrip(P, U21_focus, false, true);
    if (mode == NormStrip)            return applyStrip(P, U21_focus, true,  true);
    if (mode == NormNFD)              return applyNFD(P, U21_focus);
    if (mode == NormNFKD)             return applyNFKD(P, U21_focus);
    //if (mode == NormNFC)            return applyNFC(P, U21_focus);
    if (mode == NormLowercase)        return applyLowercase(P, U21_focus);
    if (mode == NormNmt)              return applyNmt(P, U21_focus);
    if (mode == NormBertCleanText)    return applyBertCleanText(P, U21_focus);
    if (mode == NormBertChineseChars) return applyBertChineseChars(P, U21_focus);

    return U21_focus;  // NormNone — pass-through
}

// applyNormalization — THE PAYOFF.
// Decode UTF-8 → U21 exactly once at the start, run every normalization step
// in U21 space (one codepoint per slot, no byte gymnastics), then encode back
// to UTF-8 exactly once at the end.
//
// Before this refactor, a 3-step chain like bertcleantext,bertchinesechars,lowercase
// decoded and re-encoded 3 times (6 round-trips total).  Now it's 1 decode + 1 encode
// no matter how many steps are in the chain.
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               const std::vector<NormalizationMode> & modes) {
    // Step 1 — Decode UTF-8 bytes to U21 codepoints, one per stream slot.
    StreamSet * U21_focus = decodeToU21(P, BasisBits);

    // Step 2 — Apply each normalizer in order, fully in U21 space.
    // Each step receives the output of the previous one as its input.
    for (NormalizationMode m : modes) {
        U21_focus = applyOneNormalization(P, U21_focus, m);
    }

    // Step 3 — Encode the final U21 stream back to UTF-8 bytes.
    return encodeFromU21(P, U21_focus);
}

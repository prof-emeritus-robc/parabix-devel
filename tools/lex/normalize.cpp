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
#include <kernel/streamutils/sorting.h>
#include <kernel/bitwise/bixnum_kernel.h>
#include <kernel/bitwise/bixlogic.h>

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
// Outputs a delete mask for leading whitespace.
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
// Outputs a delete mask for trailing whitespace.
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
// decode once at the top, encode once at the bottom.
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

    // Canonical decomposition (NFD) in U21 space.
    // NFD_U21_Pipeline expands precomposed characters so every combining mark
    NFD_PipelineBuilder nfd(P);
    StreamSet * NFD_U21 = nfd.NFD_U21_Pipeline(U21_focus);

    // Build the Mn character class from the Unicode General Category table.
    auto * gcObj = llvm::cast<UCD::EnumeratedPropertyObject>(
        UCD::getPropertyObject(UCD::gc));
    re::CC * mnCC = re::makeCC(gcObj->GetCodepointSet("Mn"), &cc::Unicode);

    // Mark every slot that holds an Mn codepoint.
    // CharClassesKernel takes our NFD stream (one codepoint per slot) and outputs
    // a 1-bit stream: 1 where the codepoint is in mnCC, 0 everywhere else.
    StreamSet * MnMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC*>{mnCC}, NFD_U21, MnMask);

    // Invert: we want to KEEP non-Mn slots, not delete them.
    // InvertMaskKernel flips every bit: the 0s (non-Mn) become 1s (keep).
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(MnMask, KeepMask);

    // Filter: remove all slots where KeepMask = 0 (the accent slots).
    // FilterByMask compresses the stream, leaving only the kept codepoints.
    StreamSet * result = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, NFD_U21, result);
    return result;
}

// applyStrip — remove Unicode White_Space from the left, right, or both edges.
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

    // Build the Unicode White_Space character class.
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

    // Mark which slots are whitespace.
    StreamSet * WS_Mask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC*>{wsCC}, U21_focus, WS_Mask);

    // Flip the mask to get non-whitespace positions.
    StreamSet * nonWS = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(WS_Mask, nonWS);

    StreamSet * deleteMask = nullptr;

    // StripLeadingMaskKernel uses MatchStar to mark every slot before the
    // first non-whitespace slot as "delete".
    if (stripLeading) {
        StreamSet * leadDelete = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<StripLeadingMaskKernel>(nonWS, leadDelete);
        deleteMask = leadDelete;
    }

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

    // Keep everything that is NOT marked for deletion.
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(deleteMask, KeepMask);

    // Compress: remove the deleted slots from the U21 stream.
    StreamSet * result = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, U21_focus, result);
    return result;
}
// applyByteLevel — GPT-2 byte alphabet normalization. It work on raw bytes
// and maps each individual byte (0x00-0xFF) to a unique printable Unicode codepoint. 
static StreamSet * applyByteLevel(PipelineBuilder & P, StreamSet * U21_focus) {
    // Encode our current U21 stream back to UTF-8 bytes.
    // We need raw bytes because the mapping is per-byte, not per-codepoint.
    StreamSet * bytes = encodeFromU21(P, U21_focus);

    // Each byte becomes one printable Unicode codepoint.
    // applyByteLevelEncoding returns a U21 stream directly
    return applyByteLevelEncoding(P, bytes);
}
// applyLowercase — map all uppercase codepoints to lowercase
static StreamSet * applyLowercase(PipelineBuilder & P, StreamSet * U21_focus) {

    // Full Unicode Lower_Case — handles 1→N expansions like İ → i\u0307.
    return U21_StringOverridePipeline(P, UCD::lc, U21_focus);
}
// applyNFD — canonical decomposition in U21 space.
// NFD_U21_Pipeline already exists in NFD_PipelineBuilder and returns U21
// directly, so we no longer need an intermediate byte streamset.
static StreamSet * applyNFD(PipelineBuilder & P, StreamSet * U21_focus) {
    NFD_PipelineBuilder nfd(P);
    // Takes U21_focus (one codepoint per slot), returns the decomposed U21.
    return nfd.NFD_U21_Pipeline(U21_focus);
}
// applyNFKD — compatibility decomposition in U21 space, uses NFKD_U21_Pipeline.
static StreamSet * applyNFKD(PipelineBuilder & P, StreamSet * U21_focus) {
    NFD_PipelineBuilder nfd(P);
    // Takes U21_focus (one codepoint per slot), returns the decomposed U21.
    return nfd.NFKD_U21_Pipeline(U21_focus);
}
// applyDeleteAndSpaceReplace — shared engine for applyNmt and applyBertCleanText.
// Deletes every codepoint in delCC, replaces every codepoint in repCC with
// U+0020 (space), and passes everything else through unchanged.
static StreamSet * applyDeleteAndSpaceReplace(PipelineBuilder & P,
                                              StreamSet * U21_focus,
                                              re::CC * delCC,
                                              re::CC * repCC) {

    // CharClassesKernel scans every slot in U21_focus and outputs 1 wherever
    // the codepoint falls inside delCC.
    StreamSet * DelMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(
        std::vector<re::CC*>{delCC}, U21_focus, DelMask);

    // Mark which codepoints to REPLACE with space.
    StreamSet * RepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(
        std::vector<re::CC*>{repCC}, U21_focus, RepMask);

    // Write U+0020 at every RepMask position.
    // NmtReplaceKernel21 forces bit 5 = 1 and all other bits = 0 (= 0x20)
    StreamSet * ReplacedU21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<NmtReplaceKernel21>(U21_focus, RepMask, ReplacedU21);

    // Build a keep-mask: 1 = keep this slot, 0 = delete it.
    // We invert DelMask so that non-deleted slots are marked 1.
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(DelMask, KeepMask);

    // Compress: squeeze out the deleted slots.
    // FilterByMask removes every slot where KeepMask = 0.
    StreamSet * FilteredU21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, KeepMask, ReplacedU21, FilteredU21);
    return FilteredU21;
}

// applyNmt
// Deletes a fixed set of control characters and replaces a fixed set of
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
static StreamSet * applyBertCleanText(PipelineBuilder & P, StreamSet * U21_focus) {
    auto * gcObj = llvm::cast<UCD::EnumeratedPropertyObject>(
        UCD::getPropertyObject(UCD::gc));

    // DELETE: all of \p{C} (five subcategories) minus \t \n \r, plus U+FFFD.
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
// postResult  →  returned as U21 (applyNormalization encodes at the end)
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

// applyNFC — canonical composition.
//
// NFC = NFD decomposition followed by canonical composition.
// All NFC composition kernels work on UTF-8 bytes (not U21)
static StreamSet * applyNFC(PipelineBuilder & P, StreamSet * U21_focus) {

    // Encode to UTF-8 bytes.
    StreamSet * BasisBits = encodeFromU21(P, U21_focus);

    // how many extra byte slots each position needs.
    // NFC_Initial_Insertion outputs a BixNum (multi-bit count) saying
    // "insert N extra slots after position X".
    StreamSet * InsertBixNum = P.CreateStreamSet(4, 1);
    P.CreateKernelCall<NFC_Initial_Insertion>(BasisBits, InsertBixNum);

    // Build the expansion mask from those insertion counts.
    // InsertionSpreadMask turns the BixNum into a 1-bit mask where:
    //   1 = this position holds an original byte
    //   0 = this is a newly inserted empty slot
    StreamSet * ExpansionMask = P.CreateStreamSet(1, 1);
    InsertionSpreadMask(P, InsertBixNum, ExpansionMask, InsertPosition::After);

    // Spread the input bytes into the expanded stream.
    // SpreadByMask copies original bytes to the 1-positions and
    // leaves the 0-positions (inserted slots) as zero.
    StreamSet * ExpandedBasis = P.CreateStreamSet(8, 1);
    SpreadByMask(P, ExpansionMask, BasisBits, ExpandedBasis);

    // Track which zero bytes came from the original source
    // vs which are inserted empty slots.
    StreamSet * NullStream = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::EQ_immediate>(ExpandedBasis, 0, NullStream);
    StreamSet * SourceNull = P.CreateStreamSet(1, 1);
    AndCombine(P, NullStream, ExpansionMask, SourceNull);

    // ExcludedCompositeStage.
    // Some precomposed characters are "excluded composites"
    // they must always be left in decomposed form.
    StreamSet * EC_Basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<ExcludedCompositeStage>(ExpandedBasis, EC_Basis);

    // Find non-reorderable (CCC = NR) positions.
    // CCC = "Canonical Combining Class".  NR means the class is 0 —
    // these are "starter" characters (base letters, not combining marks).
    // LongComposablePipeline needs to know where starters are so it can
    // figure out which combining marks belong to which base character.
    // We also AND with ExpansionMask to ignore inserted empty slots.
    re::PropertyExpression * CCC0_Prop = re::makePropertyExpression("CCC", "NR");
    CCC0_Prop = llvm::cast<re::PropertyExpression>(UCD::linkAndResolve(CCC0_Prop));
    StreamSet * ccc_NR0 = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(CCC0_Prop, EC_Basis, ccc_NR0,
                                                     BitMovementMode::LookAhead);
    StreamSet * ccc_NR = P.CreateStreamSet(1, 1);
    AndCombine(P, ccc_NR0, ExpansionMask, ccc_NR);

    // Singleton canonicalization.
    // Some codepoints have a "singleton decomposition" — they map to
    // a single different codepoint. 
    StreamSet * CanonBasis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<SingletonCanonicalization>(EC_Basis, CanonBasis);

    // Short-range composition.
    // Composes pairs of adjacent composable characters — a base letter
    // immediately followed by a combining mark that can merge with it.
    StreamSet * ShortBasis = P.CreateStreamSet(8, 1);
    ShortComposablePipeline(P, CanonBasis, ShortBasis);

    // Long-range composition.
    // Composes sequences where combining marks may appear between the
    // two characters being composed.
    StreamSet * FinalBasis = P.CreateStreamSet(8, 1);
    LongComposablePipeline(P, ShortBasis, ccc_NR, FinalBasis);

    // Hangul syllable composition.
    // Korean L + V + T sequences (or LV + T) are composed into
    // precomposed syllable codepoints using a special algorithm
    StreamSet * L_V_T = P.CreateStreamSet(Hangul_Composables::HC_Kind::Count);
    P.CreateKernelCall<Hangul_Composables>(FinalBasis, L_V_T,
                                           BitMovementMode::LookAhead);
    StreamSet * TranslatedBasis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<Hangul_Composition>(FinalBasis, L_V_T, TranslatedBasis);

    // Decide which positions to keep.
    // After composition, some slots are zero because:
    //   - They were inserted empty workspace slots 
    //   - They were zeroed out by Hangul (redundant L and T slots)
    // We keep a position if it is either:
    //   (a) non-zero (a real composed character), OR
    //   (b) a genuine null byte from the original source (SourceNull).
    StreamSet * NonZeroResults = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(TranslatedBasis, 0, NonZeroResults);
    StreamSet * FinalSelectionMask = P.CreateStreamSet(1, 1);
    OrCombine(P, NonZeroResults, SourceNull, FinalSelectionMask);

    // Filter: compress out the dead zero-slots.
    StreamSet * ComposedBasis = P.CreateStreamSet(8, 1);
    FilterByMask(P, FinalSelectionMask, TranslatedBasis, ComposedBasis);

    // CCC sort: reorder any residual combining marks.
    // Even after composition some combining marks may still be in the
    // wrong order.  We compute the Canonical Combining Class (CCC) for
    // every character
    // UnicodePropertyBasis gives us the CCC as a multi-bit stream.
    // U8Spans extends CCC values from the last byte to the full span
    // of each UTF-8 character.
    // BitonicSortRuns sorts runs of non-zero CCC positions.
    UCD::EnumeratedPropertyObject * enumObj =
        llvm::cast<UCD::EnumeratedPropertyObject>(
            UCD::getPropertyObject(UCD::ccc));
    StreamSet * CCC_Basis = P.CreateStreamSet(
        enumObj->GetEnumerationBasisSets().size(), 1);
    P.CreateKernelCall<UnicodePropertyBasis>(enumObj, ComposedBasis, CCC_Basis);

    StreamSet * u8idx = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(ComposedBasis, u8idx);

    StreamSet * CCC_Spans = P.CreateStreamSet(
        enumObj->GetEnumerationBasisSets().size(), 1);
    P.CreateKernelCall<U8Spans>(CCC_Basis, u8idx, CCC_Spans);

    StreamSet * CCC_NonZero = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(CCC_Spans, 0, CCC_NonZero);

    StreamSets ToSort = {CCC_Spans, ComposedBasis};
    StreamSets SortResult = BitonicSortRuns(P, 32, CCC_NonZero, ToSort);
    // SortResult[1] is the CCC-sorted BasisBits (8×1) — same format
    // as ComposedBasis.

    // Decode back to U21.
    return decodeToU21(P, SortResult[1]);
}

// applyNFKC — compatibility decomposition + canonical composition.
// Unicode defines NFKC(x) = NFC(NFKD(x))
static StreamSet * applyNFKC(PipelineBuilder & P, StreamSet * U21_focus) {
    return applyNFC(P, applyNFKD(P, U21_focus));
}

// applyOneNormalization — dispatch a single mode in U21 space.
// Takes U21_focus (one codepoint per slot) and returns a transformed U21 stream.
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
    if (mode == NormNFC)              return applyNFC(P, U21_focus);
    if (mode == NormNFKD)             return applyNFKD(P, U21_focus);
    if (mode == NormNFKC)             return applyNFKC(P, U21_focus);
    if (mode == NormLowercase)        return applyLowercase(P, U21_focus);
    if (mode == NormNmt)              return applyNmt(P, U21_focus);
    if (mode == NormBertCleanText)    return applyBertCleanText(P, U21_focus);
    if (mode == NormBertChineseChars) return applyBertChineseChars(P, U21_focus);

    return U21_focus;  // NormNone — pass-through
}

// Decodes UTF-8 once, runs every mode in U21 space, returns U21_focus.
StreamSet * applyNormalizationU21(PipelineBuilder & P,
                                   StreamSet * BasisBits,
                                   const std::vector<NormalizationMode> & modes) {
    // Decode UTF-8 bytes to U21 codepoints, one per stream slot.
    StreamSet * U21_focus = decodeToU21(P, BasisBits);

    // Apply each normalizer in order, fully in U21 space.
    // Each step receives the output of the previous one as its input.
    for (NormalizationMode m : modes) {
        U21_focus = applyOneNormalization(P, U21_focus, m);
    }

    return U21_focus;
}

// applyNormalization — normalize and return UTF-8 BasisBits.
// Calls applyNormalizationU21 then encodes the result back to bytes.
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               const std::vector<NormalizationMode> & modes) {
    return encodeFromU21(P, applyNormalizationU21(P, BasisBits, modes));
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "pretokenizer.h"

#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/adt.h>
#include <re/parse/parser.h>
#include <re/unicode/resolve_properties.h>
#include <re/cc/cc_kernel.h>
#include <re/unicode/regex_passes.h>
#include <re/unicode/boundaries.h>
#include <re/analysis/collect_ccs.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/re_multiplex.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/re/regexp_kernel.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/boundary_kernels.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pe_ones.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <vector>
#include <string>

using namespace llvm;
using namespace codegen;
using namespace kernel;
using namespace pablo;
using namespace re;

//  Debug macros 
#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name)  if (codegen::EnableIllustrator) P.captureByteData(#name, name)

//  Helper: split-code string for kernel naming 
static std::string SplitCode(SplitBehaviorMode s) {
    if (s == isolated)          return "i";
    if (s == contiguous)        return "c";
    if (s == mergedwithprevious) return "p";
    if (s == mergedwithnext)    return "n";
    if (s == removed)           return "r";
    return "d";
}

//  Kernel classes 

// Remove first position mark from insertion mask
class RemoveFirstMarkKernel : public PabloKernel {
public:
    RemoveFirstMarkKernel(LLVMTypeSystemInterface & ts,
                          StreamSet * inputMask,
                          StreamSet * outputMask)
    : PabloKernel(ts, "removeFirstMarkKernel",
                  {Binding{"inputMask", inputMask}},
                  {Binding{"outputMask", outputMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * mask = getInputStreamSet("inputMask")[0];
        PabloAST * notAtFirst = pb.createAdvance(pb.createOnes(), 1);
        PabloAST * result = pb.createAnd(mask, notAtFirst);
        writeOutputStreamSet("outputMask", std::vector<PabloAST*>{result});
    }
};

// Unicode line separator insertion kernel: writes LF (0x0A) at token boundaries
class AddUnicodeLineSeparators : public PabloKernel {
public:
    AddUnicodeLineSeparators(LLVMTypeSystemInterface & ts,
                             StreamSet * insertMask,
                             StreamSet * spreadBasis,
                             StreamSet * finalBasis)
    : PabloKernel(ts, "addUnicodeLineSeparators",
                  {Binding{"insertMask", insertMask}, Binding{"spreadBasis", spreadBasis}},
                  {Binding{"finalBasis", finalBasis}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * insert = getInputStreamSet("insertMask")[0];
        std::vector<PabloAST *> basis = getInputStreamSet("spreadBasis");

        std::vector<PabloAST *> out(basis.size());
        for (unsigned i = 0; i < basis.size(); ++i) out[i] = basis[i];

        PabloAST * insertMark = pb.createNot(insert);
        if (out.size() >= 4) {
            out[1] = pb.createOr(out[1], insertMark);
            out[3] = pb.createOr(out[3], insertMark);
        }
        writeOutputStreamSet("finalBasis", out);
    }
};

// AND kernel
class AndKernel : public PabloKernel {
public:
    AndKernel(LLVMTypeSystemInterface & ts,
              StreamSet * input1,
              StreamSet * input2,
              StreamSet * output)
    : PabloKernel(ts, "andKernel",
                  {Binding{"input1", input1}, Binding{"input2", input2}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream1 = getInputStreamSet("input1")[0];
        PabloAST * u8index = getInputStreamSet("input2")[0];
        PabloAST * result = pb.createAnd(stream1, u8index);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// NOT kernel
class NotKernel : public PabloKernel {
public:
    NotKernel(LLVMTypeSystemInterface & ts,
              StreamSet * input,
              StreamSet * output)
    : PabloKernel(ts, "notKernel",
                  {Binding{"input", input}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream = getInputStreamSet("input")[0];
        PabloAST * result = pb.createNot(stream);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// OR kernel
class OrKernel : public PabloKernel {
public:
    OrKernel(LLVMTypeSystemInterface & ts,
             StreamSet * input1,
             StreamSet * input2,
             StreamSet * output)
    : PabloKernel(ts, "orKernel",
                  {Binding{"input1", input1}, Binding{"input2", input2}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream1 = getInputStreamSet("input1")[0];
        PabloAST * stream2 = getInputStreamSet("input2")[0];
        PabloAST * result = pb.createOr(stream1, stream2);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// Unicode Alphanumeric Detection kernel (Letter OR Number)
class UnicodeAlphanumericDetector : public PabloKernel {
public:
    UnicodeAlphanumericDetector(LLVMTypeSystemInterface & ts,
                                StreamSet * BasisBits,
                                StreamSet * AlphanumericMask,
                                StreamSet * LetterStream,
                                StreamSet * NumberStream)
    : PabloKernel(ts, "unicodeAlphanumericDetector",
                  {Binding{"LetterStream", LetterStream},
                   Binding{"NumberStream", NumberStream}},
                  {Binding{"AlphanumericMask", AlphanumericMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * letter = getInputStreamSet("LetterStream")[0];
        PabloAST * number = getInputStreamSet("NumberStream")[0];
        PabloAST * alphanumeric = pb.createOr(letter, number);
        writeOutputStreamSet("AlphanumericMask", std::vector<PabloAST*>{alphanumeric});
    }
};

// ByteLevel transform: OR bit 8 for codepoints in [0x00, 0x20]
class ByteLevelTransformKernel : public PabloKernel {
public:
    ByteLevelTransformKernel(LLVMTypeSystemInterface & ts,
                              StreamSet * U21codepoints,
                              StreamSet * InvisibleMask,
                              StreamSet * U21transformed)
    : PabloKernel(ts, "byteLevelTransform",
                  {Binding{"codepoints", U21codepoints}, Binding{"invisible", InvisibleMask}},
                  {Binding{"transformed", U21transformed}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> u21 = getInputStreamSet("codepoints");
        PabloAST * invisible = getInputStreamSet("invisible")[0];

        std::vector<PabloAST *> out(21);
        for (unsigned i = 0; i < 21; i++) {
            out[i] = (i == 8) ? pb.createOr(u21[i], invisible) : u21[i];
        }
        writeOutputStreamSet("transformed", out);
    }
};

// ByteLevel GPT-2 byte encoding kernel
class ByteLevelGPT2Kernel : public PabloKernel {
public:
    ByteLevelGPT2Kernel(LLVMTypeSystemInterface & ts,
                        StreamSet * BasisBits,
                        StreamSet * GPT2Codepoints,
                        StreamSet * FirstByteMask)
    : PabloKernel(ts, "byteLevelGPT2",
                  {Binding{"basis", BasisBits}},
                  {Binding{"codepoints", GPT2Codepoints},
                   Binding{"firstbyte",  FirstByteMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> b = getInputStreamSet("basis");

        cc::Parabix_CC_Compiler_Builder ccc(b);
        PabloAST * isZone1 = ccc.compileCC(makeByte(0x00, 0x20), pb);
        PabloAST * isZone2 = ccc.compileCC(makeByte(0x7F),        pb);
        PabloAST * isZone3 = ccc.compileCC(makeByte(0x80, 0xA0),  pb);
        PabloAST * isZone4 = ccc.compileCC(makeByte(0xAD),        pb);

        PabloAST * notPass = pb.createOr(isZone2, pb.createOr(isZone3, isZone4));
        PabloAST * isPass  = pb.createNot(notPass);

        PabloAST * c2 = pb.createAnd(b[2], b[1]);
        PabloAST * c3 = pb.createAnd(b[3], c2);
        PabloAST * c4 = pb.createAnd(b[4], c3);
        PabloAST * z3[8];
        z3[0] = b[0];
        z3[1] = pb.createNot(b[1]);
        z3[2] = pb.createXor(b[2], b[1]);
        z3[3] = pb.createXor(b[3], c2);
        z3[4] = pb.createXor(b[4], c3);
        z3[5] = pb.createXor(pb.createNot(b[5]), c4);
        z3[6] = pb.createOr(b[5], c4);
        z3[7] = pb.createZeroes();

        std::vector<PabloAST *> out(21);
        out[8] = pb.createOr(isZone1, pb.createOr(isZone2, pb.createOr(isZone3, isZone4)));
        for (unsigned i = 9; i < 21; i++) out[i] = pb.createZeroes();

        constexpr uint32_t zone2_low = 33;
        constexpr uint32_t zone4_low = 67;
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * pass = pb.createAnd(isPass, b[i]);
            PabloAST * z3c  = pb.createAnd(isZone3, z3[i]);
            PabloAST * z2c  = ((zone2_low >> i) & 1)
                              ? static_cast<PabloAST *>(isZone2)
                              : static_cast<PabloAST *>(pb.createZeroes());
            PabloAST * z4c  = ((zone4_low >> i) & 1)
                              ? static_cast<PabloAST *>(isZone4)
                              : static_cast<PabloAST *>(pb.createZeroes());
            out[i] = pb.createOr(pass, pb.createOr(z3c, pb.createOr(z2c, z4c)));
        }
        writeOutputStreamSet("codepoints", out);

        PabloAST * isCont     = pb.createAnd(b[7], pb.createNot(b[6]));
        PabloAST * isFirstByte = pb.createNot(isCont);
        writeOutputStreamSet("firstbyte", std::vector<PabloAST *>{isFirstByte});
    }
};

// Detect ASCII space (0x20) positions
class WhitespaceDetector : public PabloKernel {
public:
    WhitespaceDetector(LLVMTypeSystemInterface & ts,
                       StreamSet * BasisBits,
                       StreamSet * WhitespaceMask)
    : PabloKernel(ts, "whitespaceDetector" + BasisBits->shapeString(),
                  {Binding{"BasisBits", BasisBits}},
                  {Binding{"WhitespaceMask", WhitespaceMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("BasisBits");
        PabloAST * isSpace = basis[5];
        for (unsigned i = 0; i < basis.size(); ++i) {
            if (i == 5) continue;
            isSpace = pb.createAnd(isSpace, pb.createNot(basis[i]));
        }
        writeOutputStreamSet("WhitespaceMask", std::vector<PabloAST*>{isSpace});
    }
};

// Convert split marks to token boundaries according to behavior mode
class SplitMarksToTokens : public PabloKernel {
public:
    SplitMarksToTokens(LLVMTypeSystemInterface & ts,
                       SplitBehaviorMode b,
                       StreamSet * SplitMarks,
                       StreamSet * ResultBoundaries)
    : PabloKernel(ts, "SplitMarksToTokens:" + SplitCode(b),
                  {Binding{"SplitMarks", SplitMarks}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}), mBehavior(b) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * SplitMarks = getInputStreamSet("SplitMarks")[0];
        PabloAST * SplitRun1 = pb.createAnd(pb.createAdvance(pb.createNot(SplitMarks), 1), SplitMarks);
        PabloAST * SplitRunFollow = pb.createAnd(pb.createAdvance(SplitMarks, 1), pb.createNot(SplitMarks));
        PabloAST * result = nullptr;
        if (mBehavior == contiguous)        result = pb.createOr(SplitRun1, SplitRunFollow);
        else if (mBehavior == mergedwithprevious) result = SplitRunFollow;
        else if (mBehavior == removed)      result = SplitRunFollow;
        else if (mBehavior == mergedwithnext) result = SplitRun1;
        else /* isolated */                 result = pb.createOr(SplitMarks, SplitRunFollow);
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
private:
    SplitBehaviorMode mBehavior;
};

// Isolated behavior: token boundaries + space-start boundaries
class IsolatedBehavior : public PabloKernel {
public:
    IsolatedBehavior(LLVMTypeSystemInterface & ts,
                     StreamSet * TokenBoundaries,
                     StreamSet * WhitespaceMask,
                     StreamSet * ResultBoundaries)
    : PabloKernel(ts, "isolatedBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        PabloAST * spaceStart = pb.createAnd(whitespace, pb.createNot(pb.createAdvance(whitespace, 1)));
        PabloAST * result = pb.createOr(boundaries, spaceStart);
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// MergedWithPrevious behavior: attach whitespace to previous word
class MergedWithPreviousBehavior : public PabloKernel {
public:
    MergedWithPreviousBehavior(LLVMTypeSystemInterface & ts,
                               StreamSet * TokenBoundaries,
                               StreamSet * WhitespaceMask,
                               StreamSet * ResultBoundaries)
    : PabloKernel(ts, "mergedWithPreviousBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(whitespace));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// MergedWithNext behavior: attach whitespace to next word
class MergedWithNextBehavior : public PabloKernel {
public:
    MergedWithNextBehavior(LLVMTypeSystemInterface & ts,
                           StreamSet * TokenBoundaries,
                           StreamSet * WhitespaceMask,
                           StreamSet * ResultBoundaries)
    : PabloKernel(ts, "mergedWithNextBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(pb.createAdvance(whitespace, 1)));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Contiguous behavior: keep punctuation with words, separate spaces
class ContiguousBehavior : public PabloKernel {
public:
    ContiguousBehavior(LLVMTypeSystemInterface & ts,
                       StreamSet * TokenBoundaries,
                       StreamSet * WhitespaceMask,
                       StreamSet * AlphanumericMask,
                       StreamSet * PunctuationStream,
                       StreamSet * ResultBoundaries)
    : PabloKernel(ts, "contiguousBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries},
                   Binding{"WhitespaceMask", WhitespaceMask},
                   Binding{"AlphanumericMask", AlphanumericMask, FixedRate(), LookAhead(1)},
                   Binding{"PunctuationStream", PunctuationStream}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries  = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * alphanumeric = getInputStreamSet("AlphanumericMask")[0];
        PabloAST * punctuation  = getInputStreamSet("PunctuationStream")[0];
        PabloAST * previousIsAlpha = pb.createAdvance(alphanumeric, 1);
        PabloAST * punctAfterAlpha = pb.createAnd(punctuation, previousIsAlpha);
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(punctAfterAlpha));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Detect positions of a specific delimiter codepoint
class CharDelimiterKernel : public PabloKernel {
public:
    CharDelimiterKernel(LLVMTypeSystemInterface & ts,
                        StreamSet * InputStreams,
                        StreamSet * DelimMask,
                        uint32_t delimCodepoint)
    : PabloKernel(ts, "charDelimiter_" + std::to_string(delimCodepoint)
                       + "_w" + std::to_string(InputStreams->getNumElements()),
                  {Binding{"InputStreams", InputStreams}},
                  {Binding{"DelimMask", DelimMask}}),
      mDelimCodepoint(delimCodepoint) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> b = getInputStreamSet("InputStreams");
        PabloAST * result = pb.createOnes();
        for (unsigned i = 0; i < b.size(); i++) {
            bool bit_set = (mDelimCodepoint >> i) & 1;
            result = pb.createAnd(result, bit_set ? b[i] : pb.createNot(b[i]));
        }
        writeOutputStreamSet("DelimMask", std::vector<PabloAST*>{result});
    }
private:
    uint32_t mDelimCodepoint;
};

// InFileNonSpaceKernel — nonspace = InFile(NOT space).
//
// GPT2PretokenBoundaryKernel must distinguish "the next codepoint IS a non-space"
// from "there IS no next codepoint". Pablo LookAhead past end-of-input yields 0, so
// NOT(LookAhead(space,1)) is TRUE at the final position — which wrongly splits a
// TRAILING whitespace run, because HF's "\s+(?!\S)" succeeds at EOF and keeps the
// whole run as one pretoken ("a  " → HF "a","ĠĠ"; the buggy form gave "a","Ġ","Ġ").
// InFile() masks the padding past EOF to 0 (it ANDs with NOT EOFmask), so
// LookAhead(nonspace, 1) is 0 at the last codepoint and the run stays intact.
class InFileNonSpaceKernel : public PabloKernel {
public:
    InFileNonSpaceKernel(LLVMTypeSystemInterface & ts,
                         StreamSet * space, StreamSet * nonspace)
    : PabloKernel(ts, "InFileNonSpace",
                  {Binding{"space", space}}, {Binding{"nonspace", nonspace}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * sp = getInputStreamSet("space")[0];
        pb.createAssign(pb.createExtract(getOutputStreamVar("nonspace"), pb.getInteger(0)),
                        pb.createInFile(pb.createNot(sp)));
    }
};

// GPT2PretokenBoundaryKernel — GPT-2 pretoken STARTS via CHARACTER-CLASS transitions
// (parallel leftmost-longest), replacing the RE_Kernel route which over-marks (it
// matches all positions in parallel, so overlapping arms like " '" vs "'t" and
// " star" vs "star" produce spurious boundaries). A GPT-2 pretoken is: an optional
// SINGLE leading space + a maximal run of ONE class (Letter/Number/Other), or a
// whitespace run, plus the 'contraction glue. For disjoint class-runs, leftmost-longest
// == "boundary at each class transition", computable in one parallel pass.
// Inputs (codepoint domain, 1-bit): space (LookAhead 1 to peek the next codepoint),
//   letter, number, apostrophe. Output: boundary (1 at each pretoken's first codepoint).
class GPT2PretokenBoundaryKernel : public PabloKernel {
public:
    // space    = full \p{White_Space} (the HF \s class: 0x20, tab/newline, NBSP U+00A0,
    //            and the other Unicode White_Space codepoints) — drives run-splitting.
    // spaceLit = the literal 0x20 ONLY (HF's " ?" optional-leading-space is 0x20, not \s)
    //            — the only whitespace that attaches FORWARD to the following word.
    // sdmt/cl/cvr/ce: single-codepoint classes for the contraction suffixes — [sdmt]
    // (single), l, [vr], e (for ll / ve / re). LookAhead(2) so the apostrophe can peek
    // its next 1–2 codepoints. HF's '(?:[sdmt]|ll|ve|re) is lowercase / case-sensitive.
    // nonspace = InFile(NOT space), supplied by InFileNonSpaceKernel. Used INSTEAD of
    // NOT(LookAhead(space,1)) so that "no next codepoint" (EOF) is distinguishable
    // from "next codepoint is a non-space" — see InFileNonSpaceKernel above.
    GPT2PretokenBoundaryKernel(LLVMTypeSystemInterface & ts,
                               StreamSet * space, StreamSet * nonspace,
                               StreamSet * spaceLit,
                               StreamSet * letter, StreamSet * number,
                               StreamSet * apostrophe,
                               StreamSet * sdmt, StreamSet * cl,
                               StreamSet * cvr, StreamSet * ce,
                               StreamSet * boundary)
    : PabloKernel(ts, "GPT2PretokenBoundary",
                  {Binding{"space", space, FixedRate(), LookAhead(1)},
                   Binding{"nonspace", nonspace, FixedRate(), LookAhead(1)},
                   Binding{"spaceLit", spaceLit},
                   Binding{"letter", letter}, Binding{"number", number},
                   Binding{"apostrophe", apostrophe},
                   Binding{"sdmt", sdmt, FixedRate(), LookAhead(2)},
                   Binding{"cl",   cl,   FixedRate(), LookAhead(2)},
                   Binding{"cvr",  cvr,  FixedRate(), LookAhead(2)},
                   Binding{"ce",   ce,   FixedRate(), LookAhead(2)}},
                  {Binding{"boundary", boundary}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * space    = getInputStreamSet("space")[0];      // full \p{White_Space}
        PabloAST * spaceLit = getInputStreamSet("spaceLit")[0];   // literal 0x20 only
        PabloAST * letter   = getInputStreamSet("letter")[0];
        PabloAST * number   = getInputStreamSet("number")[0];
        PabloAST * apos     = getInputStreamSet("apostrophe")[0];
        PabloAST * sdmt     = getInputStreamSet("sdmt")[0];   // [sdmt]  (single-letter suffix)
        PabloAST * cl       = getInputStreamSet("cl")[0];     // 'l'     (ll)
        PabloAST * cvr      = getInputStreamSet("cvr")[0];    // [vr]    (ve / re first letter)
        PabloAST * ce       = getInputStreamSet("ce")[0];     // 'e'     (ve / re second letter)

        PabloAST * nonspace = pb.createNot(space);
        PabloAST * other    = pb.createAnd(nonspace, pb.createNot(pb.createOr(letter, number)));

        // previous-codepoint values (Advance = delay by 1)
        PabloAST * Lp     = pb.createAdvance(letter,   1);
        PabloAST * Np     = pb.createAdvance(number,   1);
        PabloAST * Op     = pb.createAdvance(other,    1);
        PabloAST * Wp     = pb.createAdvance(space,    1);   // prev codepoint is whitespace
        PabloAST * SpLitP = pb.createAdvance(spaceLit, 1);   // prev codepoint is a literal 0x20

        // same non-space class as the previous codepoint (L→L / N→N / O→O = no boundary)
        PabloAST * sameClass = pb.createOr(pb.createAnd(letter, Lp),
                               pb.createOr(pb.createAnd(number, Np),
                                           pb.createAnd(other,  Op)));

        // nonspace[i+1], and FALSE when there is no i+1 at all. The earlier form
        // NOT(LookAhead(space,1)) also fired at end-of-input (LookAhead past EOF is 0),
        // splitting trailing whitespace runs that HF keeps whole via "\s+(?!\S)".
        PabloAST * nonspaceIn = getInputStreamSet("nonspace")[0];
        PabloAST * nsAhead = pb.createLookahead(nonspaceIn, 1);

        // A) LAST whitespace char of a run before a nonspace ALWAYS opens a token. A
        //    literal 0x20 attaches FORWARD into the word (HF " ?\p{L/N}+" / " ?[^\s..]+");
        //    a non-0x20 whitespace (e.g. NBSP U+00A0) CANNOT attach (" ?" is 0x20 only),
        //    so it is a lone "\s+" token. Both cases start a new token at this position.
        PabloAST * wsBeforeNS  = pb.createAnd(space, nsAhead);
        // B) whitespace-run START (prev not whitespace) — opens the run's leading token.
        PabloAST * spaceStart  = pb.createAnd(space, pb.createNot(Wp));
        // C) non-space run start: a class transition, UNLESS the prev codepoint is an
        //    attaching literal 0x20 (then the word joins that space's " ?word" token).
        //    Prev being a non-0x20 whitespace (NBSP) or a different nonspace class → the
        //    word starts fresh (NBSP does not attach), so wordStart fires there.
        PabloAST * wordStart   = pb.createAnd(nonspace,
                                 pb.createAnd(pb.createNot(sameClass), pb.createNot(SpLitP)));

        PabloAST * boundary = pb.createOr(wsBeforeNS, pb.createOr(spaceStart, wordStart));

        // Contraction 's|'t|'d|'m|'ll|'ve|'re — HF's '(?:[sdmt]|ll|ve|re), LOWERCASE and
        // CASE-SENSITIVE, exactly 1 or 2 suffix letters. Because the contraction arm is
        // FIRST in HF's alternation, it wins whenever the scanner RESUMES at the
        // apostrophe. It resumes there unless the previous codepoint swallowed it, which
        // happens in exactly two cases:
        //   prev == literal 0x20 → " ?[^\s\p{L}\p{N}]+" takes " '" (e.g. " 'there")
        //   prev is Other        → that arm's maximal run already absorbed it ("x?'s" → "?'","s")
        // Everything else resumes at the apostrophe and forms a contraction:
        //   prev L / N            → \p{L}+ / \p{N}+ stop before it ("don't", "3's")
        //   prev non-0x20 \s      → " ?" is 0x20-ONLY so it cannot attach; that whitespace
        //                           is a lone "\s+" token ("a\n'default" → "'d","efault";
        //                           same for tab and NBSP)
        //   BOF                   → nothing precedes ("'default" → "'d","efault")
        // Stating it as a NEGATIVE covers BOF for free: at position 0 every Advance is 0,
        // so neither exclusion fires. (The earlier form required prev L|N, which silently
        // dropped the newline / tab / NBSP / BOF cases — one such site was the ONLY
        // divergence in 5.4M tokens over 24 MB of openwebtext: Django "'default':".)
        PabloAST * aposSwallowed = pb.createOr(SpLitP, Op);   // prev 0x20, or prev Other
        PabloAST * wordApos = pb.createAnd(apos, pb.createNot(aposSwallowed));
        // Peek the 1–2 codepoints after the apostrophe (LookAhead on the suffix classes).
        PabloAST * single = pb.createAnd(wordApos, pb.createLookahead(sdmt, 1));   // 's 't 'd 'm  (span = 2 cp)
        PabloAST * dbl    = pb.createAnd(wordApos,                                  // 'll 've 're  (span = 3 cp)
                            pb.createOr(pb.createAnd(pb.createLookahead(cl,  1), pb.createLookahead(cl, 2)),
                                        pb.createAnd(pb.createLookahead(cvr, 1), pb.createLookahead(ce, 2))));
        // Suppress the boundary INSIDE the contraction span (its suffix letters join the
        // apostrophe token): +1 for single/double, +2 for the 2nd letter of a double.
        PabloAST * cSuppress = pb.createOr(pb.createAdvance(pb.createOr(single, dbl), 1),
                                           pb.createAdvance(dbl, 2));
        // FORCE a boundary right AFTER the span — the next letter starts a fresh word
        // ("Ne've"+"r", "word's"+"tuff") which sameClass would otherwise glue. Past-EOF
        // Advance yields 0, so a contraction at end of input forces nothing.
        PabloAST * cForce = pb.createOr(pb.createAdvance(single, 2),
                                        pb.createAdvance(dbl, 3));
        boundary = pb.createOr(pb.createAnd(boundary, pb.createNot(cSuppress)), cForce);

        writeOutputStreamSet("boundary", std::vector<PabloAST*>{boundary});
    }
};

//  Helper functions

static void whiteSpaceLogic(PipelineBuilder & P,
                             StreamSet * U21Basis,
                             StreamSet * WSmask,
                             StreamSet * results)
{
    re::RE * rule1 = re::generateRE_TokenizerRule(re::WhitespaceBoundary);
    RE_CompilerContext ctxt;
    ctxt.setCodeUnitContext(&cc::Unicode, U21Basis);
    RE_PipelineBuilder RE_PB(P, ctxt);
    StreamSet * wsb = P.CreateStreamSet(1);
    RE_PB.matchSearchPipeline(rule1, wsb);
    StreamSet * wsFollows = P.CreateStreamSet(1);
    P.CreateKernelCall<SplitMarksToTokens>(removed, WSmask, wsFollows);
    P.CreateKernelCall<OrKernel>(wsFollows, wsb, results);
    SHOW_STREAM(results);
}

static void applySplitBehaviorTransformation(
    PipelineBuilder & P,
    SplitBehaviorMode effectiveBehavior,
    StreamSet * spreadBasis,
    StreamSet * spreadWhitespaceMask,
    StreamSet * spreadTokenBoundaries,
    StreamSet * spreadAlphanumericMask,
    StreamSet * spreadPunctuationStream,
    StreamSet *& finalU21codepoints,
    StreamSet *& TransformedBoundaries)
{
    // Currently a pass-through; future behavior transformations applied here.
    finalU21codepoints = spreadBasis;
    TransformedBoundaries = spreadTokenBoundaries;
    SHOW_STREAM(TransformedBoundaries);
}

static StreamSet * buildREBasedTokenizer(
    PipelineBuilder & P,
    const std::string & prefixName,
    re::RE * rule,
    StreamSet * U21Basis)
{
    if (!rule) {
        llvm::errs() << "Error: null RE rule for " << prefixName << "\n";
        return nullptr;
    }
    StreamSet * WordBoundaries = P.CreateStreamSet(1, 1);
    RE_CompilerContext ctxt;
    ctxt.setCodeUnitContext(&cc::Unicode, U21Basis);
    RE_PipelineBuilder RE_PB(P, ctxt);
    RE_PB.matchSearchPipeline(rule, WordBoundaries);
    SHOW_STREAM(WordBoundaries);
    return WordBoundaries;
}

struct TokenizerConfig {
    re::RE_TokenizerKind kind;
    std::string prefix;
};

const static std::map<PreTokenizerMode, TokenizerConfig> TokenizerConfigs = {
    {whitespace,   {re::WhitespaceBoundary,    "WS"}},
    {whitespacesplit, {re::WhitespaceSplitBoundary, "WSS"}},
    {punctuation,  {re::PunctuationBoundary,   "PC"}},
    {bytelevel,    {re::ByteLevelBoundary,      "BL"}},
    {bert,         {re::BertPreTokenizer,       "BERT"}}
};

//  Stage 2a: build token boundaries 

PreTokenizerResult buildPreTokenizerBoundaries(
    PipelineBuilder & P,
    StreamSet * BasisBits,
    StreamSet * U21codepoints,
    PreTokenizerMode preTokenizer,
    SplitBehaviorMode splitBehavior,
    const std::string & delimiterString)
{
    // Number property stream (always needed; may be recomputed for bytelevel)
    auto numberProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Number");
    numberProp = cast<re::PropertyExpression>(UCD::linkAndResolve(numberProp));
    StreamSet * NumberStream = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(numberProp, U21codepoints, NumberStream);
    SHOW_STREAM(NumberStream);

    // Whitespace mask (may be replaced for bytelevel)
    StreamSet * WhitespaceMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<WhitespaceDetector>(U21codepoints, WhitespaceMask);
    SHOW_STREAM(WhitespaceMask);

    StreamSet * WordBoundaries = nullptr;

    if (preTokenizer == simpleWordBoundaries) {
        auto wb = re::makePropertyExpression(PropertyExpression::Kind::Boundary, "word");
        wb = cast<re::PropertyExpression>(UCD::linkAndResolve(wb));
        WordBoundaries = P.CreateStreamSet(1, 1);
        UnicodePropertyLogic(P, wb, U21codepoints, WordBoundaries);
    }
    else if (preTokenizer == whitespace) {
        WordBoundaries = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, U21codepoints, WhitespaceMask, WordBoundaries);
        SHOW_STREAM(WordBoundaries);
    }
    else if (preTokenizer == bytelevel) {
        // GPT-2 pretoken boundaries via CHARACTER-CLASS transitions (parallel
        // leftmost-longest), NOT RE_Kernel (which over-marks — see kernel comment).
        // Classes are on the ORIGINAL codepoints (before the byte-level Ġ remap below),
        // since \p{L}/\p{N}/\s apply to the source text, not the remapped bytes.
        auto letterProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Letter");
        letterProp = cast<re::PropertyExpression>(UCD::linkAndResolve(letterProp));
        StreamSet * BL_Letter = P.CreateStreamSet(1);
        P.CreateKernelCall<UnicodePropertyKernelBuilder>(letterProp, U21codepoints, BL_Letter);
        StreamSet * BL_Apostrophe = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(
            std::vector<re::CC *>{re::makeCC((codepoint_t)0x27)}, U21codepoints, BL_Apostrophe);
        // Contraction-suffix classes (HF's '(?:[sdmt]|ll|ve|re), lowercase): [sdmt] for
        // the single-letter suffix, l / [vr] / e for the double-letter ll / ve / re.
        StreamSet * BL_sdmt = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{
            re::makeCC(re::makeCC((codepoint_t)0x64),                     // d
            re::makeCC(re::makeCC((codepoint_t)0x6D),                     // m
                       re::makeCC((codepoint_t)0x73, (codepoint_t)0x74)))// s, t
            }, U21codepoints, BL_sdmt);
        StreamSet * BL_l = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(
            std::vector<re::CC *>{re::makeCC((codepoint_t)0x6C)}, U21codepoints, BL_l);   // l
        StreamSet * BL_vr = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(
            std::vector<re::CC *>{re::makeCC(re::makeCC((codepoint_t)0x72),
                                             re::makeCC((codepoint_t)0x76))}, U21codepoints, BL_vr); // r, v
        StreamSet * BL_e = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(
            std::vector<re::CC *>{re::makeCC((codepoint_t)0x65)}, U21codepoints, BL_e);   // e
        // Full \p{White_Space} (HF's \s): 0x20, tab/newline, NBSP U+00A0, etc. Drives
        // run-splitting. WhitespaceMask (from WhitespaceDetector) is the literal 0x20
        // ONLY, which is HF's " ?" attaching space — pass it as spaceLit.
        auto wsProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "White_Space");
        wsProp = cast<re::PropertyExpression>(UCD::linkAndResolve(wsProp));
        StreamSet * BL_WSpace = P.CreateStreamSet(1);
        P.CreateKernelCall<UnicodePropertyKernelBuilder>(wsProp, U21codepoints, BL_WSpace);
        // nonspace = InFile(NOT space): lets the boundary kernel tell "next codepoint
        // is a non-space" apart from "no next codepoint", so a TRAILING whitespace run
        // is not split (HF's "\s+(?!\S)" matches the whole run at end-of-input).
        StreamSet * BL_NonSpace = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<InFileNonSpaceKernel>(BL_WSpace, BL_NonSpace);
        WordBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<GPT2PretokenBoundaryKernel>(
            BL_WSpace, BL_NonSpace, WhitespaceMask, BL_Letter, NumberStream, BL_Apostrophe,
            BL_sdmt, BL_l, BL_vr, BL_e, WordBoundaries);
        SHOW_STREAM(WordBoundaries);

        StreamSet * GPT2Codepoints = P.CreateStreamSet(21, 1);
        StreamSet * FirstByteMask  = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<ByteLevelGPT2Kernel>(BasisBits, GPT2Codepoints, FirstByteMask);
        SHOW_BIXNUM(GPT2Codepoints);

        StreamSet * ByteWordBoundaries = P.CreateStreamSet(1, 1);
        SpreadByMask(P, FirstByteMask, WordBoundaries, ByteWordBoundaries);

        // Switch to byte domain
        U21codepoints  = GPT2Codepoints;
        WordBoundaries = ByteWordBoundaries;

        // Recompute NumberStream in byte domain
        NumberStream = P.CreateStreamSet(1);
        P.CreateKernelCall<UnicodePropertyKernelBuilder>(numberProp, GPT2Codepoints, NumberStream);

        // Zero WhitespaceMask in byte domain (space 0x20 → Ġ 0x120, no longer whitespace)
        re::CC * neverCC = re::makeCC((codepoint_t)0x00);
        StreamSet * BL_WhitespaceMask = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(
            std::vector<re::CC *>{neverCC}, GPT2Codepoints, BL_WhitespaceMask);
        WhitespaceMask = BL_WhitespaceMask;
    }
    else if (preTokenizer == sequence_whitespace_punctuation) {
        StreamSet * preTokenStrm1 = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, U21codepoints, WhitespaceMask, preTokenStrm1);
        StreamSet * preTokenStrm2 = buildREBasedTokenizer(P, "PC",
            re::generateRE_TokenizerRule(re::PunctuationBoundary), U21codepoints);
        WordBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<OrKernel>(preTokenStrm1, preTokenStrm2, WordBoundaries);
        SHOW_STREAM(WordBoundaries);
    }
    else if (preTokenizer == chardelimiter) {
        uint32_t delimCP;
        if (delimiterString.empty()) {
            delimCP = (uint32_t)',';
        } else {
            unsigned char c0 = (unsigned char)delimiterString[0];
            if      (c0 < 0x80) delimCP = c0;
            else if (c0 < 0xE0) delimCP = ((c0 & 0x1F) << 6)  | ((unsigned char)delimiterString[1] & 0x3F);
            else if (c0 < 0xF0) delimCP = ((c0 & 0x0F) << 12) | (((unsigned char)delimiterString[1] & 0x3F) << 6)  | ((unsigned char)delimiterString[2] & 0x3F);
            else                delimCP = ((c0 & 0x07) << 18) | (((unsigned char)delimiterString[1] & 0x3F) << 12) | (((unsigned char)delimiterString[2] & 0x3F) << 6) | ((unsigned char)delimiterString[3] & 0x3F);
        }
        StreamSet * CharDelimStream = P.CreateStreamSet(1, 1);
        re::CC * delimCC = re::makeCC(delimCP);
        P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{delimCC}, U21codepoints, CharDelimStream);
        WordBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<BoundaryKernel>(CharDelimStream, nullptr, WordBoundaries);
        WhitespaceMask = CharDelimStream;
        SHOW_STREAM(WordBoundaries);
    }
    else if (preTokenizer == digits) {
        WordBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<SplitMarksToTokens>(isolated, NumberStream, WordBoundaries);
    }
    else {
        // RE-based tokenizers via lookup table
        auto it = TokenizerConfigs.find(preTokenizer);
        if (it != TokenizerConfigs.end()) {
            re::RE * rule = generateRE_TokenizerRule(it->second.kind);
            WordBoundaries = buildREBasedTokenizer(P, it->second.prefix, rule, U21codepoints);
        } else {
            // Default: UAX#29 word boundaries
            WordBoundaries = P.CreateStreamSet(1);
            auto wbProp = re::makePropertyExpression(PropertyExpression::Kind::Boundary, "w");
            wbProp = cast<re::PropertyExpression>(UCD::linkAndResolve(wbProp));
            UnicodePropertyLogic(P, wbProp, U21codepoints, WordBoundaries);
        }
    }

    StreamSet * U21_tokenBoundaries = WordBoundaries;

    // Unicode property streams for behavior kernels
    auto letterProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Letter");
    letterProp = cast<re::PropertyExpression>(UCD::linkAndResolve(letterProp));
    StreamSet * LetterStream = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(letterProp, U21codepoints, LetterStream);
    SHOW_STREAM(LetterStream);

    auto punctuationProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Punctuation");
    punctuationProp = cast<re::PropertyExpression>(UCD::linkAndResolve(punctuationProp));
    StreamSet * PunctuationStream = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(punctuationProp, U21codepoints, PunctuationStream);
    SHOW_STREAM(PunctuationStream);

    StreamSet * AlphanumericMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodeAlphanumericDetector>(U21codepoints, AlphanumericMask, LetterStream, NumberStream);
    SHOW_STREAM(AlphanumericMask);

    // Compute effective behavior (some pretokenizers force "removed")
    SplitBehaviorMode effectiveBehavior = splitBehavior;
    if (preTokenizer == bert || preTokenizer == sequence_whitespace_punctuation ||
        preTokenizer == whitespacesplit || preTokenizer == whitespace ||
        preTokenizer == chardelimiter) {
        effectiveBehavior = removed;
    }

    // Apply behavior kernel to compute insertion boundaries
    StreamSet * insertionBoundaries = U21_tokenBoundaries;
    if (effectiveBehavior == removed) {
        StreamSet * notWhitespaceMask = P.CreateStreamSet(1);
        P.CreateKernelCall<NotKernel>(WhitespaceMask, notWhitespaceMask);
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<AndKernel>(U21_tokenBoundaries, notWhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == isolated && preTokenizer != digits && preTokenizer != punctuation) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<IsolatedBehavior>(U21_tokenBoundaries, WhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == mergedwithprevious) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<MergedWithPreviousBehavior>(U21_tokenBoundaries, WhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == mergedwithnext) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<MergedWithNextBehavior>(U21_tokenBoundaries, WhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == contiguous) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<ContiguousBehavior>(U21_tokenBoundaries, WhitespaceMask, AlphanumericMask,
                                               PunctuationStream, insertionBoundaries);
    }

    // Remove boundary mark at position 0
    StreamSet * insertionBoundariesClean = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<RemoveFirstMarkKernel>(insertionBoundaries, insertionBoundariesClean);

    return PreTokenizerResult{
        insertionBoundariesClean,
        U21_tokenBoundaries,
        WhitespaceMask,
        U21codepoints,
        AlphanumericMask,
        PunctuationStream,
        effectiveBehavior
    };
}

//  Stage 2b: spread + insert LF + filter 

StreamSet * applyTokenSeparatorInsertion(
    PipelineBuilder & P,
    const PreTokenizerResult & result)
{
    StreamSet * lineInsertMask = P.CreateStreamSet(1);
    UnitInsertionSpreadMask(P, result.insertionBoundaries, lineInsertMask,
                            kernel::InsertPosition::Before);
    SHOW_STREAM(lineInsertMask);

    StreamSet * spreadBasis = P.CreateStreamSet(21);
    SpreadByMask(P, lineInsertMask, result.U21codepoints, spreadBasis);
    SHOW_BIXNUM(spreadBasis);

    StreamSet * tokenBasis = P.CreateStreamSet(21);
    P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    SHOW_BIXNUM(tokenBasis);

    StreamSet * spreadTokenBoundaries = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, result.U21tokenBoundaries, spreadTokenBoundaries);

    StreamSet * spreadWhitespaceMask = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, result.WhitespaceMask, spreadWhitespaceMask);

    StreamSet * spreadAlphanumericMask = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, result.AlphanumericMask, spreadAlphanumericMask);

    StreamSet * spreadPunctuationStream = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, result.PunctuationStream, spreadPunctuationStream);

    StreamSet * tokenMask = P.CreateStreamSet(1);
    P.CreateKernelCall<NotKernel>(spreadWhitespaceMask, tokenMask);

    if (result.effectiveBehavior == removed) {
        StreamSet * newBasis = P.CreateStreamSet(21);
        FilterByMask(P, tokenMask, tokenBasis, newBasis);
        tokenBasis = newBasis;
        StreamSet * newBoundaries = P.CreateStreamSet(1);
        FilterByMask(P, tokenMask, spreadTokenBoundaries, newBoundaries);
        spreadTokenBoundaries = newBoundaries;
    }

    StreamSet * finalU21codepoints = tokenBasis;
    StreamSet * TransformedBoundaries = spreadTokenBoundaries;
    applySplitBehaviorTransformation(P, result.effectiveBehavior, tokenBasis, spreadWhitespaceMask,
                                     spreadTokenBoundaries, spreadAlphanumericMask,
                                     spreadPunctuationStream, finalU21codepoints, TransformedBoundaries);
    SHOW_STREAM(TransformedBoundaries);

    // Remove trailing null codepoints (U+0000) from zero-padded stream end
    re::CC * nonNullCC = re::makeCC(0x01, 0x10FFFF);
    StreamSet * nonNullMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{nonNullCC}, finalU21codepoints, nonNullMask);
    StreamSet * filteredU21 = P.CreateStreamSet(21);
    FilterByMask(P, nonNullMask, finalU21codepoints, filteredU21);

    return filteredU21;
}

StreamSet * applyByteLevelEncoding(PipelineBuilder & P, StreamSet * BasisBits) {
    StreamSet * GPT2Codepoints = P.CreateStreamSet(21, 1);
    StreamSet * FirstByteMask  = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<ByteLevelGPT2Kernel>(BasisBits, GPT2Codepoints, FirstByteMask);
    return GPT2Codepoints;
}


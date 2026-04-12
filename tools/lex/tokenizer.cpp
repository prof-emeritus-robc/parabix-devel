/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/core/idisa_target.h>
#include <boost/filesystem.hpp>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/adt.h>
#include <re/parse/parser.h>
#include <re/unicode/resolve_properties.h>
#include <re/cc/cc_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/core/streamset.h>
#include <kernel/unicode/utf8_support.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pe_ones.h>
#include <pablo/pablo_toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/re/regexp_kernel.h>
#include <toolchain/toolchain.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <map>
#include <re/unicode/regex_passes.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <re/unicode/boundaries.h>
#include <re/analysis/collect_ccs.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/re_multiplex.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/boundary_kernels.h>

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;
using namespace kernel;
using namespace pablo;
using namespace re;

static cl::OptionCategory wordBreakerFlags("Command Flags", "Unicode word breaker options");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(wordBreakerFlags));

// Enum for pre-tokenizer selection
enum PreTokenizerMode {
  uax29,
  whitespace,
  whitespacesplit,
  digits,
  punctuation,
  simpleWordBoundaries,
  bytelevel,
  chardelimiter,
  bert,
  sequence_whitespace_punctuation 
};

// Enum for split behavior
enum SplitBehaviorMode {
  removed,
  isolated,
  mergedwithprevious,
  mergedwithnext,
  contiguous
};

std::string SplitCode(SplitBehaviorMode s) {
    if (s == isolated) return "i";
    if (s == contiguous) return "c";
    if (s == mergedwithprevious) return "p";
    if (s == mergedwithnext) return "n";
    if (s == removed) return "r";
    return "d";
}

// pretokenizer selection as named alternative 
static cl::opt<PreTokenizerMode> PreTokenizer(
    "pretokenizer",
    cl::desc("Pre-tokenizer mode:"),
    cl::init(uax29),      // DEFAULT VALUE
    cl::values(
        clEnumValN(uax29, "uax29", "Unicode UAX#29 word boundaries (default)"),
        clEnumValN(whitespace, "whitespace", "Split on whitespace characters"),
        clEnumValN(whitespacesplit, "whitespacesplit", "Split on whitespace and output delimiters as separate tokens"),
        clEnumValN(digits, "digits", "Split on digit sequences"),
        clEnumValN(punctuation, "punctuation", "Split on punctuation characters"),
        clEnumValN(simpleWordBoundaries, "simplewordboundaries", "boundaries between word (\\w) and non-word (\\W) characters"),
        clEnumValN(bytelevel, "bytelevel", "ByteLevel tokenization: split on whitespace with byte remapping"),
        clEnumValN(chardelimiter, "chardelimiter", "Split on a specific character delimiter"),
        clEnumValN(bert, "bert", "BERT pre-tokenizer: separates punctuation and words"),
        clEnumValN(sequence_whitespace_punctuation, "sequence_whitespace_punctuation", 
                   "Sequence pre-tokenizer: Whitespace then Punctuation")
    ),
    cl::cat(wordBreakerFlags)
);

// Split behavior as named alternative
static cl::opt<SplitBehaviorMode> SplitBehavior("behavior",
    cl::desc("Split delimiter behavior:"),
    cl::init(isolated),  // DEFAULT VALUE
    cl::values(
        clEnumValN(removed, "removed", "only keep word/punctuation boundaries, exclude whitespace"),
        clEnumValN(isolated, "isolated", "keep token boundaries AND add space boundaries"),
        clEnumValN(mergedwithprevious, "mergedwithprevious", "attach whitespace to previous word"),
        clEnumValN(mergedwithnext, "mergedwithnext", "attach whitespace to next word"),
        clEnumValN(contiguous, "contiguous", "keep punctuation with words, separate spaces")),
    cl::cat(wordBreakerFlags));

// Delimiter character for chardelimiter pretokenizer
static cl::opt<std::string> DelimiterString("delimiter",
    cl::desc("Delimiter character for --pretokenizer chardelimiter (default: ',')"),
    cl::init(","),
    cl::cat(wordBreakerFlags));

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
        
        // Create a stream that is 0 at position 0 and 1 everywhere else
        PabloAST * notAtFirst = pb.createAdvance(pb.createOnes(), 1);

        // AND the mask with notAtFirst to exclude position 0
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
        
        // insertMask has 0 bits where we want to insert LF, 1s elsewhere
        PabloAST * insert = getInputStreamSet("insertMask")[0];
        std::vector<PabloAST *> basis = getInputStreamSet("spreadBasis");
        
        // Copy the spread basis bits
        std::vector<PabloAST *> out(basis.size());
        for (unsigned i = 0; i < basis.size(); ++i) {
            out[i] = basis[i];
        }

        // Create insertion mark (1s where we want to insert LF)
        PabloAST * insertMark = pb.createNot(insert);
        
        // Insert LF character (0x0A = 00001010) at marked positions
        // LF has bit 1 and bit 3 set
        if (out.size() >= 4) {
            out[1] = pb.createOr(out[1], insertMark); // Set bit 1
            out[3] = pb.createOr(out[3], insertMark); // Set bit 3
        }

        writeOutputStreamSet("finalBasis", out);
    }
};

// AND kernel to ensure boundaries only occur at UTF-8 character starts
class AndKernel : public PabloKernel {
public:
    AndKernel(LLVMTypeSystemInterface & ts,
              StreamSet * input1,  // WordBoundaries
              StreamSet * input2, // u8index
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

// NOT kernel to invert a bit stream (1→0, 0→1)
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

// OR kernel to combine two boundary streams (union)
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

// Unicode Alphanumeric Detection kernel using Unicode properties
// Combines (L*), Mark (M*), and Number (N*) Unicode categories
// to detect alphanumeric characters properly according to Unicode standard
class UnicodeAlphanumericDetector : public PabloKernel {
public:
    UnicodeAlphanumericDetector(LLVMTypeSystemInterface & ts,
                                StreamSet * BasisBits,
                                StreamSet * AlphanumericMask,
                                StreamSet * LetterStream,
                                //StreamSet * MarkStream,
                                StreamSet * NumberStream)
    : PabloKernel(ts, "unicodeAlphanumericDetector",
                  {Binding{"LetterStream", LetterStream}, 
                   Binding{"NumberStream", NumberStream}},
                  {Binding{"AlphanumericMask", AlphanumericMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        
        // Get the Unicode property streams for letters, marks, and numbers
        PabloAST * letter = getInputStreamSet("LetterStream")[0];
        PabloAST * number = getInputStreamSet("NumberStream")[0];
        
        // Alphanumeric = Letter OR Number (excluding marks)
        // L* categories include: Lu (Uppercase), Ll (Lowercase), Lt (Titlecase), 
        //                        Lm (Modifier), Lo (Other Letter)
        // N* categories include: Nd (Decimal Digit), Nl (Letter Number), No (Other Number)
        PabloAST * alphanumeric = pb.createOr(letter, number);
        
        writeOutputStreamSet("AlphanumericMask", std::vector<PabloAST*>{alphanumeric});
    }
};

// ByteLevel transform: for codepoints in [0x00, 0x20], OR bit 8 (adds 0x100).
// Matches HuggingFace GPT-2 byte-level mapping for this range:
//   Space 0x20 → Ġ (U+0120), Newline 0x0A → Ċ (U+010A), Tab 0x09 → ĉ (U+0109), etc.
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
            if (i == 8) {
                out[i] = pb.createOr(u21[i], invisible);  // bit 8 gets OR'd with the invisible mask
            } else {
                out[i] = u21[i];  // all other bits pass through unchanged
            }
        }
        writeOutputStreamSet("transformed", out);
    }
};

// ByteLevel GPT-2 byte encoding kernel.
// Input:  BasisBits       — 8 parallel streams, one position per UTF-8 byte (byte domain)
// Output: GPT2Codepoints  — 21 streams, bytes_char() codepoints (byte domain)
//         FirstByteMask   —  1 stream,  1 at the first byte of each UTF-8 sequence
//
// bytes_char() mapping (identical to HuggingFace / GPT-2):
//   Bytes 33-126, 161-172, 174-255  →  same value        (passthrough, bit 8 = 0)
//   Bytes   0-32                    →  byte + 256         (set bit 8, bits 0-7 unchanged)
//   Byte  127                       →  289  (0x121)
//   Bytes 128-160                   →  byte + 162
//   Byte  173                       →  323  (0x143)
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
        // b[0]=LSB (bit 0), b[7]=MSB (bit 7)

        // ── Zone detection ────────────────────────────────────────────────────
         
        // Bytes 0–31 are control characters (null, tab, newline, bell, escape, etc.)
        // Byte 32 is ASCII space  
        // Zone 1: bytes 0-32  →  byte + 256  (passthrough bits 0-7, set bit 8)
        //   bytes 0-31: b[7]=0, b[6]=0, b[5]=0
        //   byte 32:    b[7]=0, b[6]=0, b[5]=1, b[4-0]=00000
        PabloAST * notHi   = pb.createAnd(pb.createNot(b[7]), pb.createNot(b[6]));   // bytes 0–63
        PabloAST * isLow   = pb.createAnd(notHi, pb.createNot(b[5]));                // bytes 0–31
        // isLow catches 0–31, is32 catches exactly 32. Together they cover 0–32 with no overlap.
        // match for byte 32 = 0b00100000
        PabloAST * is32    = pb.createAnd(notHi,
                             pb.createAnd(b[5],
                             pb.createAnd(pb.createNot(b[4]),
                             pb.createAnd(pb.createNot(b[3]),
                             pb.createAnd(pb.createNot(b[2]),
                             pb.createAnd(pb.createNot(b[1]),
                                          pb.createNot(b[0])))))));                  // bytes 32
        PabloAST * isZone1 = pb.createOr(isLow, is32);  // bytes 0–32

        // Byte 127 is the DEL (delete) control character
        // Zone 2: byte 127 (0b01111111)  →  289  (bits 0-7 = 33 = 0b00100001)
        PabloAST * isZone2 = pb.createAnd(pb.createNot(b[7]),
                             pb.createAnd(b[6],
                             pb.createAnd(b[5],
                             pb.createAnd(b[4],
                             pb.createAnd(b[3],
                             pb.createAnd(b[2],
                             pb.createAnd(b[1], b[0])))))));

        // bytes 128–159 are control characters, and byte 160 is the non-breaking space
        // Zone 3: bytes 128-160  →  byte + 162
        //   bytes 128-159: b[7]=1, b[6]=0, b[5]=0
        //   byte 160:      b[7]=1, b[6]=0, b[5]=1, b[4-0]=00000
        PabloAST * hi1lo0  = pb.createAnd(b[7], pb.createNot(b[6]));    // bytes 128–191
        PabloAST * isLow3  = pb.createAnd(hi1lo0, pb.createNot(b[5]));  // bytes 128–159
        PabloAST * is160   = pb.createAnd(hi1lo0,
                             pb.createAnd(b[5],
                             pb.createAnd(pb.createNot(b[4]),
                             pb.createAnd(pb.createNot(b[3]),
                             pb.createAnd(pb.createNot(b[2]),
                             pb.createAnd(pb.createNot(b[1]),
                                          pb.createNot(b[0])))))));    // Only byte 160 
        PabloAST * isZone3 = pb.createOr(isLow3, is160);   // bytes 128-160
        
        // Byte 173 is the soft hyphen - 10101101
        // Zone 4: byte 173 (0b10101101)  →  323  (bits 0-7 = 67 = 0b01000011)
        PabloAST * isZone4 = pb.createAnd(b[7],
                             pb.createAnd(pb.createNot(b[6]),
                             pb.createAnd(b[5],
                             pb.createAnd(pb.createNot(b[4]),
                             pb.createAnd(b[3],
                             pb.createAnd(b[2],
                             pb.createAnd(pb.createNot(b[1]), b[0])))))));

        // Passthrough: all bytes NOT in zones 2/3/4. zone 1 and passthrough keep bits 0–7 the same
        // Zone 1 also uses passthrough bits 0-7 (its identity carries through automatically).
        PabloAST * notPass = pb.createOr(isZone2, pb.createOr(isZone3, isZone4));  // every position where bits 0–7 need to change
        PabloAST * isPass  = pb.createNot(notPass); // every position where bits 0–7 stay the same, passthrough and zone 1

        //  Zone 3 carry chain: byte + 162  (162 = 0b10100010) 
        // bytes 128-160 - adding all zone 3 bytes simultaneously
        // In zone 3: b[7]=1 always, and bit 7 of 162=1, so they cancel → sum[7]=0,
        // carry propagates into bit 8 (always 1).  b[6]=0 in zone 3.
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
        z3[6] = pb.createOr(b[5], c4);   // b[6]=0 in zone3 → result = carry5
        z3[7] = pb.createZeroes();        // 1 XOR 1 = 0; carry always enters bit 8

        //  Output bit 8: set for all remapped zones 
        std::vector<PabloAST *> out(21);
        // Output bits 8 - 1
        // sets bit 8 of the output to 1 at every position where any remapping happened.
        out[8] = pb.createOr(isZone1, pb.createOr(isZone2, pb.createOr(isZone3, isZone4)));
        // highest output is 323 which only needs 9 bits (bits 0–8), bits 9 through 20 are always zero
        // sets 9 through 20  to zero so the output streams are clean.
        for (unsigned i = 9; i < 21; i++) out[i] = pb.createZeroes();

        //  Output bits 0-7
        //  combine correct output bits for each zone, into one single output stream
        constexpr uint32_t zone2_low = 33;  // bits 0-7 of 289 (= 256+33) - zone 2 constant - 127
        constexpr uint32_t zone4_low = 67;  // bits 0-7 of 323 (= 256+67) - zone 4 constant - 173
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

        //  First-byte mask 
        // UTF-8 continuation bytes have pattern 10xxxxxx (b[7]=1, b[6]=0).
        // First byte of any UTF-8 sequence is NOT a continuation byte.
        PabloAST * isCont     = pb.createAnd(b[7], pb.createNot(b[6]));
        PabloAST * isFirstByte = pb.createNot(isCont);
        writeOutputStreamSet("firstbyte", std::vector<PabloAST *>{isFirstByte});
    }
};

// Detect whitespace/delimiter positions (ASCII space 0x20 = 00100000)
class WhitespaceDetector : public PabloKernel {
public:
    WhitespaceDetector(LLVMTypeSystemInterface & ts,
                       StreamSet * BasisBits,
                       StreamSet * WhitespaceMask)
    : PabloKernel(ts, "whitespaceDetector" + BasisBits -> shapeString(),
                  {Binding{"BasisBits", BasisBits}},
                  {Binding{"WhitespaceMask", WhitespaceMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("BasisBits");
        
        // ASCII space is 0x20 = 00100000 (bit 5 set, all others clear) ? unicdoe space?
        PabloAST * isSpace = basis[5];
        for (unsigned i = 0; i < basis.size(); ++i) {
            if (i == 5) continue;
            isSpace = pb.createAnd(isSpace, pb.createNot(basis[i]));
        }
        // WhitespaceMask is a binary stream that marks every position in the input 
        //where an ASCII space character (0x20) occurs.
        writeOutputStreamSet("WhitespaceMask", std::vector<PabloAST*>{isSpace});
    }
};

// 
//  Given SplitMarks marking characters that are "split" characters,
//  produce output token marks according to a given split behaviour
//  mode.   The result is a correct token mark stream, but an additional
//  step is required when the behaviour is "removed" in which case all
//  the split characters must be deleted.
//
class SplitMarksToTokens : public PabloKernel {
public:
    SplitMarksToTokens(LLVMTypeSystemInterface & ts,
                  SplitBehaviorMode b, StreamSet * SplitMarks, StreamSet * ResultBoundaries)
    : PabloKernel(ts, "SplitMarksToTokens:" + SplitCode(b) ,
                  {Binding{"SplitMarks", SplitMarks}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}), mBehavior(b) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * SplitMarks = getInputStreamSet("SplitMarks")[0];
        //
        PabloAST * SplitRun1 = pb.createAnd(pb.createAdvance(pb.createNot(SplitMarks), 1), SplitMarks);
        PabloAST * SplitRunFollow = pb.createAnd(pb.createAdvance(SplitMarks, 1), pb.createNot(SplitMarks));
        PabloAST * result = nullptr;
        if (mBehavior == contiguous) {
            result = pb.createOr(SplitRun1, SplitRunFollow);
        } else if (mBehavior == mergedwithprevious) {
            result = SplitRunFollow;
        } else if (mBehavior == removed) {
            result = SplitRunFollow;
        } else if (mBehavior == mergedwithnext) {
            result = SplitRun1;
        } else { // isolated
            result = pb.createOr(SplitMarks, SplitRunFollow);
        }
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
private:
    SplitBehaviorMode mBehavior;
};

// Behavior mode 1: Isolated - keep token boundaries AND add space boundaries
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
        
        // Find start of whitespace runs (first space in consecutive spaces)
        PabloAST * spaceStart = pb.createAnd(whitespace, pb.createNot(pb.createAdvance(whitespace, 1)));
        
        // Result = token boundaries OR space starts(space boundries, word/punctuation positions kept, spaces isolated)
        PabloAST * result = pb.createOr(boundaries, spaceStart);
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 3: MergedWithPrevious - attach whitespace to previous word
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
        // Shift right by 1: moves boundaries to merge whitespace with previous token
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(whitespace));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 4: MergedWithNext - attach whitespace to next word
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
        // Shift left by 1: moves boundaries to merge whitespace with next token
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(pb.createAdvance(whitespace, 1)));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 5: Contiguous - keep punctuation with words, separate spaces
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
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * alphanumeric = getInputStreamSet("AlphanumericMask")[0];
        PabloAST * punctuation = getInputStreamSet("PunctuationStream")[0];

        // Punctuation immediately follows alphanumeric should not have boundaries
        PabloAST * currentIsPunctuation = punctuation;
        PabloAST * previousIsAlpha = pb.createAdvance(alphanumeric, 1); // looks BEHIND by 1 character position
        PabloAST * punctAfterAlpha = pb.createAnd(currentIsPunctuation, previousIsAlpha);
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(punctAfterAlpha));
       
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Detects positions where the input codepoint equals delimCodepoint.
// Works on any parallel bit-stream set (BasisBits width=8 or U21codepoints width=21).
// Each stream b[i] represents bit i of the character value at each position.
class CharDelimiterKernel : public PabloKernel {
public:
// character class kernel ? 
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

        // Build AND of: b[i] for every bit that is SET in delimCodepoint,
        //               NOT(b[i]) for every bit that is CLEAR.
        PabloAST * result = pb.createOnes();
        for (unsigned i = 0; i < b.size(); i++) {
            bool bit_set = (mDelimCodepoint >> i) & 1;
            PabloAST * cond = bit_set ? b[i] : pb.createNot(b[i]);
            result = pb.createAnd(result, cond);
        }
        writeOutputStreamSet("DelimMask", std::vector<PabloAST*>{result});
    }
private:
    uint32_t mDelimCodepoint;
};

// Debug macros for visualization
#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name)  if (codegen::EnableIllustrator) P.captureByteData(#name, name)

using WordBreakerFunctionType = void (*)(uint32_t fd);


// make a new function here 
void whiteSpaceLogic (PipelineBuilder & P, StreamSet * U21Basis, StreamSet * WSmask, StreamSet * results) {
        
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
};

// Function to apply split behavior transformation based on split behavior mode
void applySplitBehaviorTransformation(
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
    // All behavior transformations are applied at the U21 level (before LF insertion)
    // via the insertionBoundaries block in wordBreakerPipeline. This is a pass-through.
    finalU21codepoints = spreadBasis;
    TransformedBoundaries = spreadTokenBoundaries;
    SHOW_STREAM(TransformedBoundaries);
}
// Function Declaration
// Helper function to build RE-based tokenizer boundaries
// Centralizes the common pattern: generate RE → collect CCs → multiplex → create kernels
StreamSet* buildREBasedTokenizer(
    PipelineBuilder& P,
    const std::string& prefixName,  // unique identifier for this tokenizer e.g., "PC", "WS"
    re::RE* rule,                  // the regex pattern to use
    StreamSet* U21Basis          // the basis bits representing the input characters
) {
    if (!rule) {
        llvm::errs() << "Error: null RE rule for " << prefixName << "\n";
        return nullptr;
    }
    // empty stream to hold the boundary results
    StreamSet* WordBoundaries = P.CreateStreamSet(1, 1);
    
    RE_CompilerContext ctxt;
    ctxt.setCodeUnitContext(&cc::Unicode, U21Basis);
    RE_PipelineBuilder RE_PB(P, ctxt);
    RE_PB.matchSearchPipeline(rule, WordBoundaries);
    
    SHOW_STREAM(WordBoundaries);
    return WordBoundaries;
}

// Struct to hold tokenizer configuration
struct TokenizerConfig {
    re::RE_TokenizerKind kind;   // Enum value from boundaries.h
    std::string prefix;  // Prefix for multiplexed alphabet (e.g., "PC", "WS")git cd
};

// Single map combining both pieces of information
const static std::map<PreTokenizerMode, TokenizerConfig> TokenizerConfigs = {
    {whitespace, {re::WhitespaceBoundary, "WS"}},
    {whitespacesplit, {re::WhitespaceSplitBoundary, "WSS"}},
    {punctuation, {re::PunctuationBoundary, "PC"}},
    // {digits, {re::DigitBoundary, "DG"}},
    {bytelevel, {re::ByteLevelBoundary, "BL"}},
    {bert, {re::BertPreTokenizer, "BERT"}}
};

WordBreakerFunctionType wordBreakerPipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>{"fileDescriptor"});
    
    Scalar * const fileDescriptor = P.getInputScalar("fileDescriptor");
    
    // Input Processing
    // Read file into byte stream
    // 1 stream, each element is 8 bits wide
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    
    // Convert serial bytes to 8 parallel bit streams(8 streams of 1-bit values)
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);
    
    // Create UTF-8 character boundary index (UTF-8 Index Creation)
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);
    SHOW_STREAM(u8index);

    // UTF-8 to U21 Conversion Pipeline
    // Creating U21 codepoint stream from UTF-8 basis bits (U21 Codepoint Generation)
    StreamSet * U21_u8indexed = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    // filter by mask with UTF-8 index stream ?
    SHOW_BIXNUM(U21_u8indexed);

    StreamSet * U21codepoints = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21codepoints);
    SHOW_BIXNUM(U21codepoints);

    // Unicode Word Boundary Rules
    StreamSet * WordBoundaries = nullptr;

    // Create Number property stream using Unicode general category 'Number'
    // Detects all Unicode number characters (Nd, Nl, No)
    // Nd: Decimal digit numbers, Nl: Letter numbers, No: Other numbers
    auto numberProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Number");
    numberProp = cast<re::PropertyExpression>(UCD::linkAndResolve(numberProp));
    StreamSet * NumberStream = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(numberProp, U21codepoints, NumberStream);
    SHOW_STREAM(NumberStream);

    // Detect whitespace/delimiter positions BEFORE spreading/inserting
    // This ensures alignment with U21_tokenBoundaries
    StreamSet * WhitespaceMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<WhitespaceDetector>(U21codepoints, WhitespaceMask);
    SHOW_STREAM(WhitespaceMask);

    // Special case: simpleWordBoundaries uses different kernel pipeline
    if(PreTokenizer == simpleWordBoundaries){
        // Use simple word boundaries based on Unicode "word" property
        auto wb = re::makePropertyExpression(PropertyExpression::Kind::Boundary, "word");
        wb = cast<re::PropertyExpression>(UCD::linkAndResolve(wb));
        WordBoundaries = P.CreateStreamSet(1, 1);
        UnicodePropertyLogic(P, wb, U21codepoints, WordBoundaries);
    }
    // whitespace uses separate whiteSpaceLogic function
    else if (PreTokenizer == whitespace){
        WordBoundaries = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, U21codepoints, WhitespaceMask, WordBoundaries);
        SHOW_STREAM(WordBoundaries);
    }
    // ByteLevel pre-tokenizer (GPT-2 style):
    //
    // Step 1 — find token boundaries on the ORIGINAL Unicode codepoints.
    //   The GPT-2 regex uses ' ?' (literal ASCII space) as an optional prefix:
    //     | ?\p{L}++   matches " world" as one token
    //     | ?[^\s\p{L}\p{N}]++  matches " ." as one token
    //   The regex must see space as 0x20 (whitespace) for these ` ?` patterns to work.
    //   We also removed the |\s fallback alternatives from ByteLevelBoundary in boundaries.cpp
    //   because Parabix evaluates all alternatives in parallel — |\s would fire for every
    //   space independently, creating spurious extra boundaries even when the space was
    //   already consumed as a ` ?` prefix by a longer match.
    //
    // Step 2 — remap [0x00, 0x20] → [0x100, 0x120] for OUTPUT display only.
    //   space 0x20 → Ġ (U+0120), newline 0x0A → Ċ, tab 0x09 → ĉ, etc.
    //   This encoding only affects the token text shown in output, not the boundaries.
    else if(PreTokenizer == bytelevel) {
        // find token boundaries on the ORIGINAL Unicode codepoints.
        // The GPT-2 regex uses ' ?' (literal space) as an optional prefix, so space must
        // still be 0x20 (whitespace) here. Boundaries are in char domain (one per codepoint).
        WordBoundaries = buildREBasedTokenizer(P, "BL",
            re::generateRE_TokenizerRule(re::ByteLevelBoundary), U21codepoints);
        SHOW_STREAM(WordBoundaries);

        // GPT-2 byte encoding + first-byte mask, both derived from BasisBits.
        // ByteLevelGPT2Kernel maps every raw UTF-8 byte through bytes_char() — the full
        // HuggingFace mapping — and simultaneously marks first bytes of each UTF-8 sequence.
        StreamSet * GPT2Codepoints = P.CreateStreamSet(21, 1);
        StreamSet * FirstByteMask  = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<ByteLevelGPT2Kernel>(BasisBits, GPT2Codepoints, FirstByteMask);
        SHOW_BIXNUM(GPT2Codepoints);

        // Spread char-domain boundaries → byte domain using FirstByteMask.
        // u8index marks LAST bytes of UTF-8 sequences (used by FilterByMask for decoding),
        // so we MUST use FirstByteMask here to land each boundary at the correct first byte.
        StreamSet * ByteWordBoundaries = P.CreateStreamSet(1, 1);
        SpreadByMask(P, FirstByteMask, WordBoundaries, ByteWordBoundaries);

        // Switch the pipeline to byte domain.
        U21codepoints  = GPT2Codepoints;
        WordBoundaries = ByteWordBoundaries;

        // re-compute NumberStream in byte domain.
        // GPT-2 maps ASCII digits 48-57 to themselves, so decimal-digit property still works.
        NumberStream = P.CreateStreamSet(1);
        P.CreateKernelCall<UnicodePropertyKernelBuilder>(numberProp, GPT2Codepoints, NumberStream);

        // zero WhitespaceMask in byte domain.
        // Space 0x20 → Ġ (0x120) in GPT-2 — no longer whitespace — so IsolatedBehavior
        // must not add extra boundaries.  Codepoint 0x00 never appears in GPT-2 output
        // (minimum mapped value is 33), so this mask is always zero.
        re::CC * neverCC = re::makeCC((codepoint_t)0x00);
        StreamSet * BL_WhitespaceMask = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(
            std::vector<re::CC *>{neverCC}, GPT2Codepoints, BL_WhitespaceMask);
        WhitespaceMask = BL_WhitespaceMask;
    }
    // composite tokenizer (OR of two RE rules)
    else if (PreTokenizer == sequence_whitespace_punctuation) {
        // Composite tokenizer: whitespace + punctuation combined with OR
        StreamSet * preTokenStrm1 = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, U21codepoints, WhitespaceMask, preTokenStrm1);
       
        StreamSet * preTokenStrm2 = buildREBasedTokenizer(P, "PC", 
            re::generateRE_TokenizerRule(re::PunctuationBoundary), U21codepoints);
        // Combine boundaries (OR operation)
        WordBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<OrKernel>(preTokenStrm1, preTokenStrm2, WordBoundaries);
        SHOW_STREAM(WordBoundaries);
    }
    else if (PreTokenizer == chardelimiter) {
        // Detect positions of the delimiter character in raw bytes (BasisBits space)
       // replace with:
       // uint32_t — 32 bits, comfortably holds any Unicode code point 
        uint32_t delimCP;
        if (DelimiterString.empty()) {
            delimCP = (uint32_t)',';
        } else {
            unsigned char c0 = (unsigned char)DelimiterString[0];
            if      (c0 < 0x80) delimCP = c0; // starts with 0 → 1 byte
            else if (c0 < 0xE0) delimCP = ((c0 & 0x1F) << 6)  | ((unsigned char)DelimiterString[1] & 0x3F);  // starts with 110 → 2 bytes
            else if (c0 < 0xF0) delimCP = ((c0 & 0x0F) << 12) | (((unsigned char)DelimiterString[1] & 0x3F) << 6)  | ((unsigned char)DelimiterString[2] & 0x3F);    // starts with 1110 → 3 bytes
            else                delimCP = ((c0 & 0x07) << 18) | (((unsigned char)DelimiterString[1] & 0x3F) << 12) | (((unsigned char)DelimiterString[2] & 0x3F) << 6) | ((unsigned char)DelimiterString[3] & 0x3F);   // starts with 11110 → 4 bytes (emoji etc.)
        } 
        StreamSet * CharDelimStream = P.CreateStreamSet(1, 1);
        // {delimCC} — the character class to match the delimiter code point
        re::CC * delimCC = re::makeCC(delimCP);
        P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{delimCC}, U21codepoints, CharDelimStream);
        // BoundaryKernel fires at transitions: non-delim→delim and delim→non-delim
        WordBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<BoundaryKernel>(CharDelimStream, nullptr, WordBoundaries);
        WhitespaceMask = CharDelimStream;
        SHOW_STREAM(WordBoundaries);
        
    }
    else if (PreTokenizer == digits) {
        WordBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<SplitMarksToTokens>(isolated, NumberStream, WordBoundaries);
    }
    else {
        // Standard RE-based tokenizers - use lookup table
        auto it = TokenizerConfigs.find(PreTokenizer);
        
        if (it != TokenizerConfigs.end()) {
            // Found in map: call the generator function and build pipeline
            re::RE* rule = generateRE_TokenizerRule(it->second.kind);  // Call enum depatcher to get the appropriate RE rule
            WordBoundaries = buildREBasedTokenizer(P, it->second.prefix, 
                                                   rule, U21codepoints);
        } else {
            // Default fallback: UAX#29 word boundaries
            WordBoundaries = P.CreateStreamSet(1);
            auto wbProp = re::makePropertyExpression(PropertyExpression::Kind::Boundary, "w");
            wbProp = cast<re::PropertyExpression>(UCD::linkAndResolve(wbProp));     
            UnicodePropertyLogic(P, wbProp, U21codepoints, WordBoundaries);
        }
    }
   
    StreamSet * U21_tokenBoundaries = WordBoundaries;
    
    // Detect alphanumeric positions using Unicode properties (Letter, Mark, Number)
    // L* categories: Letter (uppercase, lowercase, titlecase, modifier, other)
    // M* categories: Mark (nonspacing, spacing, enclosing)
    // N* categories: Number (decimal, letter number, other)
    // Unicode Properties Created from U21 codepoint stream
    // Create Letter property stream using Unicode general category 'Letter' 
    // Detects all Unicode letter characters (Lu, Ll, Lt, Lm, Lo)
    auto letterProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Letter");
    letterProp = cast<re::PropertyExpression>(UCD::linkAndResolve(letterProp));
    StreamSet * LetterStream = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(letterProp, U21codepoints, LetterStream);
    SHOW_STREAM(LetterStream);

    // Create Punctuation property stream using Unicode general category 'Punctuation'
    // Detects all Unicode punctuation characters (Pc, Pd, Ps, Pe, Pi, Pf, Po)
    // Pc: Connector punctuation (_, ‿), Pd: Dash punctuation (-, –, —)
    // Ps: Open punctuation ( (, {, [ ), Pe: Close punctuation ( ), }, ] )
    // Pi: Initial quote punctuation (‘, “), Pf: Final quote punctuation (’, ”)
    // Po: Other punctuation (!, ?, ., , , ;, :, etc.)
    auto punctuationProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Punctuation");
    punctuationProp = cast<re::PropertyExpression>(UCD::linkAndResolve(punctuationProp));
    StreamSet * PunctuationStream = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(punctuationProp, U21codepoints, PunctuationStream);
    SHOW_STREAM(PunctuationStream);

    // Combine Letter and Number properties to detect alphanumeric characters
    StreamSet * AlphanumericMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodeAlphanumericDetector>(U21codepoints, AlphanumericMask, LetterStream, NumberStream);
    SHOW_STREAM(AlphanumericMask);

    // For chardelimiter: treat the delimiter character as the "split char" everywhere
    // WhitespaceMask is used in removed behavior to remove split chars from output.
    // if (PreTokenizer == chardelimiter) {
    //     uint32_t delimCP = DelimiterString.empty() ? (uint32_t)',' : (uint32_t)(unsigned char)DelimiterString[0];
    //     StreamSet * DelimMask = P.CreateStreamSet(1, 1);
    //     P.CreateKernelCall<CharDelimiterKernel>(U21codepoints, DelimMask, delimCP);
    //     WhitespaceMask = DelimMask;
    //     SHOW_STREAM(WhitespaceMask);
    // }

    // Apply behavior transformation using appropriate kernel
    StreamSet * TransformedBoundaries = P.CreateStreamSet(1, 1);

    // moved here: needs to know effectiveBehavior BEFORE computing lineInsertMask
    // Compute effective behavior mode based on PreTokenizer choice
    SplitBehaviorMode effectiveBehavior = SplitBehavior;
    if (PreTokenizer == bert || PreTokenizer == sequence_whitespace_punctuation || PreTokenizer == whitespacesplit || PreTokenizer == whitespace || PreTokenizer == chardelimiter) {
        effectiveBehavior = removed;
    }

    // For removed behavior: U21_tokenBoundaries has boundaries on BOTH sides of whitespace
    // (UAX29 places a boundary before AND after each whitespace run). Inserting LF before
    // the whitespace-position boundary + removing whitespace = two consecutive LFs = blank line.
    // so to Fix it: we remove boundaries that fall ON whitespace positions before computing lineInsertMask.
    // Compute corrected LF-insertion boundaries at U21 level (BEFORE spreading/insertion).
    // Each behavior kernel removes or adds boundaries here so only the RIGHT LFs get inserted.
    // use RemoveFirstMarkKernel to remove the first boundary mark at whitespace positions for "removed" behavior, ensuring no LF is inserted before spaces. For other behaviors, compute insertion boundaries according to the specified rules??
    StreamSet * insertionBoundaries = U21_tokenBoundaries;
    if (effectiveBehavior == removed) {
        // Remove boundaries ON whitespace positions so no LF is inserted before a space.
        // FilterByMask later deletes the space characters themselves.
        StreamSet * notWhitespaceMask = P.CreateStreamSet(1);
        P.CreateKernelCall<NotKernel>(WhitespaceMask, notWhitespaceMask);
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<AndKernel>(U21_tokenBoundaries, notWhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == isolated && PreTokenizer != digits && PreTokenizer != punctuation) {
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
        P.CreateKernelCall<ContiguousBehavior>(U21_tokenBoundaries, WhitespaceMask, AlphanumericMask, PunctuationStream, insertionBoundaries);
    }

   // same convention as insertionBoundaries (1 = mark, 0 = nothing)
    StreamSet * insertionBoundariesClean = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<RemoveFirstMarkKernel>(insertionBoundaries, insertionBoundariesClean);
    insertionBoundaries = insertionBoundariesClean;

    StreamSet * lineInsertMask = P.CreateStreamSet(1);
    UnitInsertionSpreadMask(P, insertionBoundaries, lineInsertMask, kernel::InsertPosition::Before);
    SHOW_STREAM(lineInsertMask);
   
    StreamSet * spreadBasis = P.CreateStreamSet(21);
    SpreadByMask(P, lineInsertMask, U21codepoints, spreadBasis);
    SHOW_BIXNUM(spreadBasis);

    // Insert Token Separators
    StreamSet * tokenBasis = P.CreateStreamSet(21);
    P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    SHOW_BIXNUM(tokenBasis);

    // spreading to match the streams
    // Spread boundaries to match tokenBasis length (L+I)
    StreamSet * spreadTokenBoundaries = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, U21_tokenBoundaries, spreadTokenBoundaries);

    // Spread whitespace mask to match tokenBasis length
    StreamSet * spreadWhitespaceMask = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, WhitespaceMask, spreadWhitespaceMask);
    
    // Spread alphanumeric mask to match tokenBasis length (needed for contiguous behavior)
    StreamSet * spreadAlphanumericMask = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, AlphanumericMask, spreadAlphanumericMask);
    
    // Spread punctuation stream to match tokenBasis length (needed for contiguous behavior)
    StreamSet * spreadPunctuationStream = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, PunctuationStream, spreadPunctuationStream);
    
    StreamSet * tokenMask = P.CreateStreamSet(1);
    P.CreateKernelCall<NotKernel>(spreadWhitespaceMask, tokenMask);

    if (effectiveBehavior == removed) {
        StreamSet * newBasis = P.CreateStreamSet(21);
        FilterByMask(P, tokenMask, tokenBasis, newBasis);
        tokenBasis = newBasis;
        StreamSet * newBoundaries = P.CreateStreamSet(1);
        FilterByMask(P, tokenMask, spreadTokenBoundaries, newBoundaries);
        spreadTokenBoundaries = newBoundaries;
    }
    
    // After spreading and inserting, we have new codepoints at the inserted positions.
    StreamSet * finalU21codepoints = tokenBasis;
    applySplitBehaviorTransformation(P, effectiveBehavior, tokenBasis, spreadWhitespaceMask, 
                                  spreadTokenBoundaries, spreadAlphanumericMask, spreadPunctuationStream,
                                  finalU21codepoints, TransformedBoundaries);
    SHOW_STREAM(TransformedBoundaries);

    // Remove null codepoints (U+0000) from zero-padded stream end
    re::CC * nonNullCC = re::makeCC(0x01, 0x10FFFF);
    StreamSet * nonNullMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{nonNullCC}, finalU21codepoints, nonNullMask);
    StreamSet * filteredU21 = P.CreateStreamSet(21);
    FilterByMask(P, nonNullMask, finalU21codepoints, filteredU21);
    finalU21codepoints = filteredU21;

    // Convert U21 codepoints back to UTF-8 basis bits
    StreamSet * output_basis = P.CreateStreamSet(8);
    U21_to_UTF8(P, finalU21codepoints, output_basis);
    SHOW_BIXNUM(output_basis);

    // Convert parallel bit streams back to serial bytes
    StreamSet * tokenizedOutput = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(output_basis, tokenizedOutput);
    SHOW_BYTES(tokenizedOutput);

    // Write to standard output
    P.CreateKernelCall<StdOutKernel>(tokenizedOutput);
    
    return reinterpret_cast<WordBreakerFunctionType>(P.compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&wordBreakerFlags, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});  //command line options 
    CPUDriver driver("unicode_word_tokenizer");

    WordBreakerFunctionType wordBreakerFn = wordBreakerPipeline(driver);

    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
        return 1;
    } else {
        wordBreakerFn(fd);
        close(fd);
    
    }    return 0;
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/core/kernel.h>
#include <pablo/pablo.h>
namespace kernel { class PipelineBuilder; }

namespace kernel {
//
// Convert a byte stream of hexadecimal values into a bit stream.
// Each hexadecimal input byte generates 4 bits to the output stream.
//

class HexToBinary final : public kernel::BlockOrientedKernel {
public:
    HexToBinary(LLVMTypeSystemInterface & ts, StreamSet * hexStream, StreamSet * binStream);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
};

//
// Produce a hexadecimal output stream with one hexadecimal byte
// for each 4 bits of an input bit stream.
//

class BinaryToHex final : public kernel::BlockOrientedKernel {
public:
    BinaryToHex(LLVMTypeSystemInterface & ts, StreamSet * binStream, StreamSet * hexStream);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
    void generateFinalBlockMethod(KernelBuilder & b, llvm::Value * const remainingBits) override;
};

//
//  Given an Mx1 input stream bixData and an Nx1 output variable bixPairData,
//  where M > N and M <= 2*N, write the bits of bixData to bixPairData such that:
//  bixPairData[i] at pos p = bixData[N + i] at position p/2, if p is even and i < M
//                          = bixData[i] at position (p - 1)/2, if p is odd
//                          = 0 otherwise
//

class BixSplit final : public kernel::BlockOrientedKernel {
public:
    BixSplit(LLVMTypeSystemInterface & ts, StreamSet * bixData, StreamSet * bixPairData);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
private:
    unsigned mInputBits;
    unsigned mOutputBits;
};

//
//  Given a 4 x i1 input stream, generate a corresponding 8 x i1 ASCII output
//  stream consisting of the hex representation of the input.
//
class Bix4toHex final : public pablo::PabloKernel {
public:
    Bix4toHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * hexBasis, bool LowerHex = false);
protected:
    void generatePabloMethod() override;
private:
    bool mLowerHex;
};

void UnicodeHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void JavaHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void CHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void XMLHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

void PerlHexPipeline(kernel::PipelineBuilder & p, StreamSet * U21, StreamSet * U21_out);

//
//  Given a 21-bit basis stream and a single stream marking spans of hex digits
//  that represent codepoints, calculate and produce the encoded 21-bit codepoint
//  value at the end of each such span stream.  Elsewhere, the basis is copied.
//  The spans hold only hex digits ([0-9A-Fa-f]) and are separated by other
//  positions (adjacent spans are taken as one).  Only the last 6 digits of a
//  span are used, of which the first contributes its low bit (the 21 bits of
//  the value): longer spans are accommodated if the digits before these are
//  leading zeros, and a value is at most 10FFFF.

class HexDecode final : public pablo::PabloKernel {
public:
    HexDecode(LLVMTypeSystemInterface & ts, StreamSet * sourceBasis, StreamSet * hexSpans,
                                            StreamSet * decoded);
protected:
    void generatePabloMethod() override;
};

//
//  As HexDecode, for spans of decimal digits ([0-9], at most 8 significant
//  ones): at the end of each span, its value, if at most 10FFFF, and otherwise
//  a mark in overflow (the basis copied there).
//
class DecimalDecode final : public pablo::PabloKernel {
public:
    DecimalDecode(LLVMTypeSystemInterface & ts, StreamSet * sourceBasis, StreamSet * decSpans,
                  StreamSet * decoded, StreamSet * overflow);
protected:
    void generatePabloMethod() override;
};

//
//  The forms of escaped codepoints decoded by the Hex-Any transforms (ICU's
//  variants): Java (\uXXXX), C (also \UXXXXXXXX), Perl (\x{X...}, 1 to 6
//  digits), Unicode (U+XXXX, 4 to 6), XML (&#xX...;, 1 to 6) and XML10 (&#D...;,
//  1 to 7 decimal digits).  Hex-Any itself decodes all of them.
//
enum HexEscapeForm : unsigned {
    HexEscapeJava = 1, HexEscapeC = 2, HexEscapePerl = 4, HexEscapeUnicode = 8, HexEscapeXML = 16, HexEscapeXML10 = 32,
    HexEscapeAll = 63
};

//
//  Decode the escaped codepoints of the given forms in U21 (within mask, if
//  not null: all the characters of an escape must be in it), as ICU 78:
//  the hex digits may be in either case, the prefixes are case-sensitive, an
//  escape of a value beyond 10FFFF is deleted, a high surrogate decoded right
//  before a low surrogate makes one codepoint with it, and a lone surrogate
//  becomes U+FFFD.  keeps receives the masks of the positions kept by each of
//  the deletions made (in order), to keep other streams in step.
//
void HexAnyPipeline(kernel::PipelineBuilder & p, unsigned forms, StreamSet * U21, StreamSet * mask, StreamSet * U21_out,
                    std::vector<StreamSet *> & keeps);

}

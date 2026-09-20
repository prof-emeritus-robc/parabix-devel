/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/pablo.h>
#include <kernel/core/kernel_builder.h>

namespace kernel {

struct LengthGroup {unsigned lo; unsigned hi;};
    
struct LengthGroupInfo {
    unsigned lo;                    //  the low bound of the length group
    unsigned hi;                    //  the high bound of the length group
    unsigned encoding_bytes;        //  the number of bytes for encoding this group
    unsigned prefix_base;           //  the base value of encoded prefix bytes
    unsigned hash_bits;             //  number of bits used for hash codes
    unsigned length_extension_bits; //  number of bits used for length extension
};

class EncodingInfo {
public:
    EncodingInfo(unsigned maxHashBits, std::vector<LengthGroupInfo> lengthGroups) :
        MAX_HASH_BITS(maxHashBits), byLength(lengthGroups) {}
    const unsigned MAX_HASH_BITS;
    const std::vector<LengthGroupInfo> byLength;
public:
    unsigned getLengthGroupNo(unsigned lgth) const;
    unsigned maxSymbolLength() const;
    unsigned maxEncodingBytes() const;
    unsigned prefixLengthOffset(unsigned lgth) const;
    std::string uniqueSuffix() const;
};

class WordMarkKernel : public pablo::PabloKernel {
public:
    WordMarkKernel(LLVMTypeSystemInterface & ts, StreamSet * BasisBits, StreamSet * WordMarks);
protected:
    void generatePabloMethod() override;
};

class ByteRun final: public pablo::PabloKernel {
public:
    ByteRun(LLVMTypeSystemInterface & ts, StreamSet * const basis, StreamSet * excluded, StreamSet * runMask)
    : pablo::PabloKernel(ts, "byteRun", {Binding{"basis", basis}, Binding{"excluded", excluded}}, {Binding{"runMask", runMask}}) {}
protected:
    void generatePabloMethod() override;
};

/*
 This kernel decodes the insertion length for two-byte ZTF code symbols.
 The insertion length is the number of zero bytes to insert so that,
 after insertion the zeroes together with the encoded symbol can be
 replaced by the dictionary symbol of the appropriate length.
*/

class ZTF_ExpansionDecoder final: public pablo::PabloKernel {
public:
    ZTF_ExpansionDecoder(LLVMTypeSystemInterface & ts,
                         EncodingInfo & encodingScheme,
                         StreamSet * const basis,
                         StreamSet * insertBixNum);
protected:
    void generatePabloMethod() override;
    EncodingInfo & mEncodingScheme;
};

class ZTF_DecodeLengths : public pablo::PabloKernel {
public:
    ZTF_DecodeLengths(LLVMTypeSystemInterface & ts,
                      EncodingInfo & encodingScheme,
                      StreamSet * basisBits,
                      StreamSet * groupStreams);
protected:
    void generatePabloMethod() override;
    EncodingInfo & mEncodingScheme;
};

// Parse encodable ZTF words or symbols from plaintext or ciphertext.
// The result is a stream symbolRuns marking symbol continuation bits
// with 1 bits.   Each 0 bit represents a start of a new symbol.
class ZTF_Symbols : public pablo::PabloKernel {
public:
    ZTF_Symbols(LLVMTypeSystemInterface & ts,
                StreamSet * basisBits, StreamSet * wordChar, StreamSet * symbolRuns)
    : pablo::PabloKernel(ts, "ZTF_Symbols",
                         {Binding{"basisBits", basisBits, FixedRate(1), LookAhead(1)},
                             Binding{"wordChar", wordChar, FixedRate(1), LookAhead(3)}},
                         {Binding{"symbolRuns", symbolRuns}}) { }
protected:
    void generatePabloMethod() override;
};

// Given parsed symbol runs, produce a stream marking end positions only.
class ZTF_SymbolEnds : public pablo::PabloKernel {
public:
    ZTF_SymbolEnds(LLVMTypeSystemInterface & ts,
                   StreamSet * symbolRuns, StreamSet * overflow, StreamSet * symbolEnds)
    : pablo::PabloKernel(ts, "ZTF_SymbolEnds",
                         {Binding{"symbolRuns", symbolRuns, FixedRate(1), LookAhead(1)},
                          Binding{"overflow", overflow}},
                         {Binding{"symbolEnds", symbolEnds}}) { }
protected:
    void generatePabloMethod() override;
};

class ZTF_SymbolEncoder final: public pablo::PabloKernel {
public:
    ZTF_SymbolEncoder(LLVMTypeSystemInterface & ts,
                      EncodingInfo & encodingScheme,
                      StreamSet * const basis,
                      StreamSet * bixHash,
                      StreamSet * extractionMask,
                      StreamSet * runIdx,
                      StreamSet * encoded);
protected:
    void generatePabloMethod() override;
    EncodingInfo & mEncodingScheme;
};

class LengthGroupSelector final: public pablo::PabloKernel {
public:
    LengthGroupSelector(LLVMTypeSystemInterface & ts,
                 EncodingInfo & encodingScheme,
                    unsigned groupNo,
                 StreamSet * symbolRun, StreamSet * const lengthBixNum,
                 StreamSet * overflow,
                 StreamSet * selected);
protected:
    void generatePabloMethod() override;
    EncodingInfo & mEncodingScheme;
    unsigned mGroupNo;
};

class LengthSorter final: public pablo::PabloKernel {
public:
    LengthSorter(LLVMTypeSystemInterface & ts,
                 EncodingInfo & encodingScheme,
                 StreamSet * symbolRun, StreamSet * const lengthBixNum,
                 StreamSet * overflow,
                 StreamSet * groupStreams);
protected:
    void generatePabloMethod() override;
    EncodingInfo & mEncodingScheme;
};
}

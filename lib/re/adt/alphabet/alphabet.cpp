/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */


#include <re/alphabet/alphabet.h>
#include <llvm/Support/ErrorHandling.h>

namespace cc {

UnicodeMappableAlphabet::UnicodeMappableAlphabet
        (const std::string alphabetName, const std::string code,
         unsigned unicodeCommon,
         std::vector <UCD::codepoint_t> aboveCommon)
: Alphabet(alphabetName, code, ClassTypeId::UnicodeMappableAlphabet)
, mUnicodeCommon(unicodeCommon)
, mAboveCommon(std::move(aboveCommon)) {

}

UCD::codepoint_t UnicodeMappableAlphabet::toUnicode(const unsigned n) const {
    UCD::codepoint_t cp = n;
    if (n < mUnicodeCommon) return cp;
    assert(n < mUnicodeCommon + mAboveCommon.size());
    return mAboveCommon[n - mUnicodeCommon];
}
  
unsigned UnicodeMappableAlphabet::fromUnicode(const UCD::codepoint_t codepoint) const {
    unsigned n = codepoint;
    if (n < mUnicodeCommon) return n;
    for (unsigned i = 0; i < mAboveCommon.size(); i++) {
        if (mAboveCommon[i] == codepoint) return mUnicodeCommon + i;
    }
    llvm::report_fatal_error("fromUnicode: codepoint not found in alphabet.");
}

CodeUnitAlphabet::CodeUnitAlphabet(const std::string alphabetName, const std::string code, uint8_t bits) :
    Alphabet(alphabetName, code, ClassTypeId::CodeUnitAlphabet), mCodeUnitBits(bits) {}

const UnicodeMappableAlphabet Unicode("Unicode", "U", UCD::UNICODE_MAX, {});

const UnicodeMappableAlphabet ASCII("ASCII", "A", 0x7F, {});

const UnicodeMappableAlphabet ISO_Latin1("ISO_Latin1", "l1", 0xFF, {});

const CodeUnitAlphabet Byte("Byte", "x8", 8);
    
const CodeUnitAlphabet UTF8("UTF8", "u8", 8);

const CodeUnitAlphabet UTF16("UTF16", "u16", 16);
    
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <cstring>
#include <string>
#include <vector>
#include <llvm/Support/Casting.h>
#include <re/adt/re_re.h>
#include <re/adt/re_cc.h>

//
// A String is a fixed sequence of Unicode codepoints.  It matches exactly
// what the Seq of the corresponding singleton CCs matches.
namespace re {

class String : public RE {
public:
    static String * Create(const std::u32string & s) {return new String(s);}
    std::u32string getString() const {return std::u32string(mString, mLength);}
    length_t size() const {return mLength;}
    bool empty() const {return mLength == 0;}
    const char32_t * begin() const {return mString;}
    const char32_t * end() const {return mString + mLength;}
    char32_t operator[](const length_t i) const {return mString[i];}
    RE_SUBTYPE(String)
private:
    String(const std::u32string & s)
    : RE(ClassTypeId::String)
    , mLength(s.size())
    , mString(replicateU32String(s.data(), s.size())) {}
    static const char32_t * replicateU32String(const char32_t * s, const length_t length) {
        if (length > 0) {
            char32_t * allocated = ThreadSafeSlabAllocator::allocate_array_of<char32_t>(length);
            std::memcpy(allocated, s, length * sizeof(char32_t));
            return allocated;
        }
        return nullptr;
    }
    const length_t mLength;
    const char32_t * const mString;
};

inline String * makeString(const std::u32string & s) {return String::Create(s);}

// Is re a literal codepoint: a CC of one Unicode codepoint?
inline bool isLiteralCodepoint(const RE * re) {
    const CC * const cc = llvm::dyn_cast<CC>(re);
    return cc && (cc->getAlphabet() == &cc::Unicode) && !cc->empty() && (cc->min_codepoint() == cc->max_codepoint());
}

// Replace each run of two or more literal codepoints among the items of a
// sequence by the String of their codepoints.
inline void packStrings(std::vector<RE *> & items) {
    std::vector<RE *> packed;
    packed.reserve(items.size());
    for (size_t i = 0; i < items.size(); ) {
        size_t j = i;
        while ((j < items.size()) && isLiteralCodepoint(items[j])) ++j;
        if (j - i >= 2) {
            std::u32string s;
            for (size_t k = i; k < j; ++k) {
                s.push_back(llvm::cast<CC>(items[k])->min_codepoint());
            }
            packed.push_back(makeString(s));
            i = j;
        } else {
            packed.push_back(items[i++]);
        }
    }
    items.swap(packed);
}

}

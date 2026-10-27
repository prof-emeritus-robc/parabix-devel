/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <re/alphabet/alphabet.h>
#include <re/adt/re_re.h>

namespace re {

class Any : public RE {
public:
    static Any * Create(const cc::Alphabet * a = &cc::Unicode) {return new Any(a);}
    RE_SUBTYPE(Any)
    const cc::Alphabet * getAlphabet() const {return mAlphabet;}
private:
    Any(const cc::Alphabet * a) : RE(ClassTypeId::Any), mAlphabet(a) {}
    const cc::Alphabet * mAlphabet;
};

inline Any * makeAny(const cc::Alphabet * a = &cc::Unicode) {return Any::Create(a);}

}


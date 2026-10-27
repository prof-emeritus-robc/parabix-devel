#pragma once

#include <re/adt/re_re.h>

//
// Ranges generally represent CCs, but might not be able to be
// converted to CCs during parsing, if one or both endpoints are
// symbolic.   [\N{some name}-\N{some other name}],  [\q(ch)-k]
namespace re {

class Range : public RE {
public:
    RE * getLo() const {return mLo;}
    RE * getHi() const {return mHi;}
    static Range * Create(RE * lh, RE * rh) {return new Range(lh, rh);}
    RE_SUBTYPE(Range)
private:
    Range(RE * lh, RE * rh) : RE(ClassTypeId::Range), mLo(lh), mHi(rh) {}
    RE * const mLo;
    RE * const mHi;
};

inline RE * makeRange(RE * lh, RE * rh) {return Range::Create(lh, rh);}
}


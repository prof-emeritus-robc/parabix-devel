/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <re/adt/re_re.h>

namespace re {

class Rep : public RE {
public:
    enum class Kind {Standard, Possessive};
    enum { UNBOUNDED_REP = -1 };
    RE * getRE() const {return mRE;}
    int getLB() const {return mLB;}
    int getUB() const {return mUB;}
    Kind getKind() const {return mKind;}
    static Rep * Create(RE * r, const int lb, const int ub, Rep::Kind k = Rep::Kind::Standard)
        {return new Rep(r, lb, ub, k);}
    RE_SUBTYPE(Rep)
private:
    Rep(RE * repeated, const int lb, const int ub, Kind k = Rep::Kind::Standard) :
        RE(ClassTypeId::Rep), mRE(repeated), mLB(lb), mUB(ub), mKind(k) {}
    RE* mRE;
    int mLB;
    int mUB;
    Rep::Kind mKind;
};

RE * makeRep(RE * re, const int lower_bound, const int upper_bound, Rep::Kind k = Rep::Kind::Standard);
    
}


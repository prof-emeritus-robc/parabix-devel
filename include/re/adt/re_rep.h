/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <re/adt/re_re.h>

namespace re {

class Rep : public RE {
public:
    enum { UNBOUNDED_REP = -1 };
    RE * getRE() const {return mRE;}
    int getLB() const {return mLB;}
    int getUB() const {return mUB;}
    static Rep * Create(RE * r, const int lb, const int ub) {return new Rep(r, lb, ub);}
    RE_SUBTYPE(Rep)
private:
    Rep(RE * repeated, const int lb, const int ub) : RE(ClassTypeId::Rep), mRE(repeated), mLB(lb), mUB(ub) {}
    RE* mRE;
    int mLB;
    int mUB;
};

RE * makeRep(RE * re, const int lower_bound, const int upper_bound);
    
}


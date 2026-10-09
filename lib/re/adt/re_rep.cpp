/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/adt/re_rep.h>

#include <re/adt/adt.h>
#include <re/analysis/nullable.h>

using namespace llvm;

namespace re {

inline int ubCombine(const int h1, const int h2) {
    if ((h1 == Rep::UNBOUNDED_REP) || (h2 == Rep::UNBOUNDED_REP)) {
        return Rep::UNBOUNDED_REP;
    }
    else {
        return h1 * h2;
    }
}
    
RE * makeRep(RE * re, int lb, const int ub, Rep::Kind k) {
    if (LLVM_UNLIKELY(lb == Rep::UNBOUNDED_REP)) {
        report_fatal_error("repetition lower bound must be finite!");
    }
    if (LLVM_UNLIKELY(ub != Rep::UNBOUNDED_REP && ub < lb)) {
        report_fatal_error("lower bound cannot exceed upper bound");
    }
    if (isEmptySet(re)) {
        // Match failure.
        return re;
    }
    if (isEmptySeq(re)) {
        // Repeated match of empty string: just match once.
        return re;
    }
    //  Nested repetitions are combined only if both are standard: e.g., (a*+)? is
    //  not a* (a possessive repetition takes all the repetitions it can).
    Rep * const nested = (k == Rep::Kind::Standard) ? dyn_cast<Rep>(re) : nullptr;
    if (Rep * rep = (nested && nested->getKind() == Rep::Kind::Standard) ? nested : nullptr) {
        int l = rep->getLB();
        int u = rep->getUB();
        if (lb == ub) {
            return Rep::Create(rep->getRE(), l * lb, ubCombine(u, ub));
        }
        else if (u == Rep::UNBOUNDED_REP) {
            if (l == 0) {
                /*  R{0,}{lb, ub} = R{0,} */
                return rep;
            } else if (l == 1) {
                /*  R{1,}{lb, ub} = R{lb,} */
                return Rep::Create(rep->getRE(), lb, Rep::UNBOUNDED_REP);
            } else if (lb == 0) {
                /*  R{l,}{0, ub} = R{l,}? */
                return Rep::Create(rep, 0, 1);
            } else {
                /* R{l,}{lb, ub} = R{l * lb,} */
                return Rep::Create(rep->getRE(), l * lb, Rep::UNBOUNDED_REP);
            }
        }
        else if (u > l) {
            // Calculate the smallest number of repetitions n such that n * u + 1 >= (n + 1) * l
            int n = (u - 2)/(u-l);
            if (lb >= n) {
                return Rep::Create(rep->getRE(), l * lb, ubCombine(u, ub));
            }
            if ((ub == Rep::UNBOUNDED_REP) || (ub >= n)) {
                RE * r1 = Rep::Create(rep->getRE(), n * l, ubCombine(u, ub));
                RE * r2 = makeRep(rep, lb, n - 1);  // makeRep recursive simplifies.
                return makeAlt({r1, r2});
            }
        }
    }
    else if (isa<Assertion>(re)) {
        if (lb > 0) return re;
        else return makeSeq();
    }
    else {
        if (Seq * seq = dyn_cast<Seq>(re)) {
            if (seq->empty()) {
                return seq;
            }
        }
        if ((lb == 0) && (ub == 0)) {
            return makeSeq();
        }
        else if ((lb == 1) && (ub == 1)) {
            return re;
        }
    }
    return Rep::Create(re, lb, ub, k);
}

}

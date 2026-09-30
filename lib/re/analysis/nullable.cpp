/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/analysis/nullable.h>

#include <re/adt/adt.h>
#include <re/analysis/validation.h>

using namespace llvm;

namespace re {

bool isNullable(const RE * re) {
    if (const Seq * re_seq = dyn_cast<const Seq>(re)) {
        for (const RE * re : *re_seq) {
            if (!isNullable(re)) {
                return false;
            }
        }
        return true;
    } else if (const Alt * re_alt = dyn_cast<const Alt>(re)) {
        for (const RE * re : *re_alt) {
            if (isNullable(re)) {
                return true;
            }
        }
        return false;
    } else if (const Rep* re_rep = dyn_cast<const Rep>(re)) {
        return (re_rep->getLB() == 0) || isNullable(re_rep->getRE());
    } else if (isa<Diff>(re)) {
        // a Diff of Seq({}) and an Assertion represents a complemented assertion.
        //return isNullable(d->getLH()) && (!isNullable(d->getRH())) && (!isZeroWidth(d->getRH()));
        return false;
    } else if (const Intersect * e = dyn_cast<const Intersect>(re)) {
        return isNullable(e->getLH()) && isNullable(e->getRH());
    } else if (const Group * g = dyn_cast<const Group>(re)) {
        return isNullable(g->getRE());
    } else if (const Name * n = dyn_cast<const Name>(re)) {
        const RE * const defn = n->getDefinition();
        return defn && isNullable(defn);
    } else if (const Capture * c = dyn_cast<const Capture>(re)) {
        return isNullable(c->getCapturedRE());
    } else if (const Permute * p = dyn_cast<const Permute>(re)) {
        for (const RE * term : *p) {
            if (!isNullable(term)) {
                return false;
            }
        }
        return true;
    } else if (const Interleavable * s = dyn_cast<const Interleavable>(re)) {
        for (const RE * term : *s) {
            if (!isNullable(term)) {
                return false;
            }
        }
        return true;
    } else if (isa<Any, CC, Range, PropertyExpression>(re)) {
        return false;
    } else if (isa<Assertion, Start, End, Reference>(re)) {
        // Zero-width or back-referenced: these match the empty string only in
        // some contexts, so they are not unconditionally nullable.
        return false;
    }
    UnexpectedRE("isNullable", re);
}

} // namespace re

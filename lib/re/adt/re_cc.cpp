/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/adt/re_cc.h>
#include <llvm/Support/Compiler.h>
#include <re/adt/adt.h>
#include <sstream>

using namespace llvm;

namespace re {

std::string CC::canonicalName() const {
    std::stringstream name;
    name << mAlphabet->getName();
    name << std::hex;
    char separator = '_';
    for (const interval_t i : *this) {
        name << separator;
        if (lo_codepoint(i) == hi_codepoint(i)) {
            name << lo_codepoint(i);
        }
        else {
            name << lo_codepoint(i) << '_' << hi_codepoint(i);
        }
        separator = ',';
    }
    return name.str();
}
    
CC::CC(const cc::Alphabet * alphabet)
: RE(ClassTypeId::CC)
, UnicodeSet()
, mAlphabet(alphabet) {}


CC::CC(const CC & cc)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(cc)
, mAlphabet(cc.getAlphabet()) {}


CC::CC(const codepoint_t codepoint, const cc::Alphabet * alphabet)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(codepoint)
, mAlphabet(alphabet) {}


CC::CC(const codepoint_t lo_codepoint, const codepoint_t hi_codepoint, const cc::Alphabet * alphabet)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(lo_codepoint, hi_codepoint)
, mAlphabet(alphabet) {}


CC::CC(const CC * cc1, const CC * cc2)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(std::move(*cc1 + *cc2))
, mAlphabet(cc1->getAlphabet()) {
    assert (cc1->getAlphabet() == cc2->getAlphabet());
}


CC::CC(const UCD::UnicodeSet set, const cc::Alphabet * alphabet)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(set)
, mAlphabet(alphabet) {}


CC::CC(std::initializer_list<interval_t>::iterator begin, std::initializer_list<interval_t>::iterator end, const cc::Alphabet * alphabet)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(begin, end)
, mAlphabet(alphabet) {}


CC::CC(const std::vector<interval_t>::iterator begin, const std::vector<interval_t>::iterator end, const cc::Alphabet * alphabet)
: RE(ClassTypeId::CC)
, UCD::UnicodeSet(begin, end)
, mAlphabet(alphabet) {}

const CC * matchableCodepoints(const RE * re) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        return cc;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        CC * matchable = makeCC();
        for (const RE * re : *alt) {
            matchable = makeCC(matchable, matchableCodepoints(re));
        }
        return matchable;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        CC * matchable = makeCC();
        bool pastCC = false;
        for (const RE * re : *seq) {
            if (pastCC) {
                if (!(isa<End>(re) || matchesEmptyString(re))) {
                    return makeCC();
                }
            } else if (isa<End>(re)) {
                return makeCC();
            } else {
                matchable = makeCC(matchable, matchableCodepoints(re));
                pastCC = !matchesEmptyString(re);
            }
        }
        return matchable;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        if ((rep->getLB() <= 1) || matchesEmptyString(rep->getRE())) {
            return matchableCodepoints(rep->getRE());
        } else {
            return makeCC();
        }
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return subtractCC(matchableCodepoints(diff->getLH()), matchableCodepoints(diff->getRH()));
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        return intersectCC(matchableCodepoints(e->getLH()), matchableCodepoints(e->getRH()));
    } else if (isa<Any>(re)) {
        return makeCC(0, 0x10FFFF);
    } else if (const Name * n = dyn_cast<Name>(re)) {
        const RE * const defn = n->getDefinition();
        return defn ? matchableCodepoints(defn) : makeCC();
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return matchableCodepoints(c->getCapturedRE());
    } else if (const Group * g = dyn_cast<Group>(re)) {
        // Modes are not applied here (case folding is not available at this
        // level), so e.g. (?i:a) yields only 'a': an under-approximation.
        return matchableCodepoints(g->getRE());
    } else if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        // A boundary is zero-width; an unresolved property is unknown.
        const RE * const resolved = pe->getResolvedRE();
        if ((pe->getKind() == PropertyExpression::Kind::Codepoint) && resolved) {
            return matchableCodepoints(resolved);
        }
        return makeCC();
    } else if (const Range * rg = dyn_cast<Range>(re)) {
        const CC * const lo = matchableCodepoints(rg->getLo());
        const CC * const hi = matchableCodepoints(rg->getHi());
        if (lo->empty() || hi->empty()) {
            return makeCC();
        }
        const auto lo_cp = lo_codepoint(lo->front());
        const auto hi_cp = hi_codepoint(hi->back());
        if (lo_cp > hi_cp) {
            return makeCC();
        }
        return makeCC(lo_cp, hi_cp, lo->getAlphabet());
    } else if (isa<Permute, Interleavable>(re)) {
        // A single character is matched by one term while all others match
        // the empty string.
        auto termsMatchable = [](const auto & terms) -> const CC * {
            const RE * nonEmptyTerm = nullptr;
            for (const RE * term : terms) {
                if (!matchesEmptyString(term)) {
                    if (nonEmptyTerm) {
                        return makeCC();  // at least two characters are required
                    }
                    nonEmptyTerm = term;
                }
            }
            if (nonEmptyTerm) {
                return matchableCodepoints(nonEmptyTerm);
            }
            CC * matchable = makeCC();
            for (const RE * term : terms) {
                matchable = makeCC(matchable, matchableCodepoints(term));
            }
            return matchable;
        };
        return isa<Permute>(re) ? termsMatchable(*cast<Permute>(re)) : termsMatchable(*cast<Interleavable>(re));
    } else if (isa<Start, End, Assertion, Reference>(re)) {
        // Zero-width, or a back-reference, which repeats a capture and so
        // cannot match a single character on its own.
        return makeCC();
    }
    UnexpectedRE("matchableCodepoints", re);
}

}

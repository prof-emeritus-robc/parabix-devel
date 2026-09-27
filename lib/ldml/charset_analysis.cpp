/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "charset_analysis.h"
#include <ldml/transform_rules.h>
#include <re/adt/adt.h>
#include <re/unicode/resolve_properties.h>

using namespace llvm;
using namespace re;

namespace ldml {

const UCD::UnicodeSet AllCodepoints(0, UCD::UNICODE_MAX);

bool CharSetAnalysis::isSetExpression(const RE * re) {
    return isa<CC>(re) || isa<PropertyExpression>(re) || isa<Any>(re) || isa<Diff>(re) || isa<Intersect>(re);
}

bool CharSetAnalysis::isSet(const RE * re) {
    if (isSetExpression(re)) return true;
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && isSet(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        // Unions, possibly with strings and the text boundary.
        for (const RE * a : *alt) {
            if (isa<Start>(a) || isa<End>(a)) continue;
            if (const Seq * seq = dyn_cast<Seq>(a)) {
                for (const RE * e : *seq) {
                    if (!isa<CC>(e)) return false;
                }
                continue;
            }
            if (!isSet(a)) return false;
        }
        return true;
    }
    return false;
}

UCD::UnicodeSet CharSetAnalysis::propertySet(const PropertyExpression * pe) {
    auto f = mPropertySets.find(pe);
    if (f != mPropertySets.end()) return f->second;
    // Resolve a copy, as linking modifies property expressions.
    PropertyExpression * copy = makePropertyExpression(pe->getKind(), pe->getPropertyIdentifier(),
                                                       pe->getOperator(), pe->getValueString());
    UCD::UnicodeSet result = AllCodepoints;  // unknown properties: any character
    RE * linked = UCD::linkProperties(copy);
    if (!isa<Any>(linked) && pe->getKind() == PropertyExpression::Kind::Codepoint && copy->getPropertyCode() >= 0) {
        UCD::resolveProperties(UCD::standardizeProperties(copy));
        if (const CC * cc = dyn_cast_or_null<CC>(copy->getResolvedRE())) {
            result = *cc;
        }
    }
    mPropertySets.emplace(pe, result);
    return result;
}

UCD::UnicodeSet CharSetAnalysis::setOf(const RE * re, bool stringChars) {
    if (const CC * cc = dyn_cast<CC>(re)) return *cc;
    if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) return propertySet(pe);
    if (isa<Any>(re)) return AllCodepoints;
    if (const Diff * d = dyn_cast<Diff>(re)) return setOf(d->getLH(), stringChars) - setOf(d->getRH(), stringChars);
    if (const Intersect * x = dyn_cast<Intersect>(re)) return setOf(x->getLH(), stringChars) & setOf(x->getRH(), stringChars);
    if (const Name * n = dyn_cast<Name>(re)) return n->getDefinition() ? setOf(n->getDefinition(), stringChars) : AllCodepoints;
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        UCD::UnicodeSet s;
        for (const RE * a : *alt) {
            if (isa<Start>(a) || isa<End>(a)) continue;
            if (const Seq * seq = dyn_cast<Seq>(a)) {
                if (stringChars) {
                    for (const RE * e : *seq) s = s + setOf(e, stringChars);
                }
            } else {
                s = s + setOf(a, stringChars);
            }
        }
        return s;
    }
    return AllCodepoints;
}

}

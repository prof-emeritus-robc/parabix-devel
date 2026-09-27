/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Internal to the ldml library: the codepoint sets of set expressions of
//  transform rules, with Unicode properties resolved.

#pragma once

#include <map>
#include <ucd/core/unicode_set.h>

namespace re { class RE; class PropertyExpression; }

namespace ldml {

extern const UCD::UnicodeSet AllCodepoints;

class CharSetAnalysis {
public:
    // The codepoints of a set expression (a CC, property expression, Any,
    // Diff, Intersect, set union, or variable defined as one of these).
    // Strings within sets are approximated by their characters, or excluded
    // if !stringChars, and the text boundary is excluded.  Unknown properties
    // are treated as all codepoints.
    UCD::UnicodeSet setOf(const re::RE * re, bool stringChars = true);
    // Is the RE a UnicodeSet, possibly with strings (possibly through variables)?
    static bool isSet(const re::RE * re);
    static bool isSetExpression(const re::RE * re);
private:
    UCD::UnicodeSet propertySet(const re::PropertyExpression * pe);
    std::map<const re::PropertyExpression *, UCD::UnicodeSet> mPropertySets;
};

}

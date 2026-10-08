/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Implementation plans for the rules of a transform in one direction.
//
//  The rules (as extracted forward rules, after any rewriting passes) are
//  divided into steps: each transform rule is a step, and each maximal
//  sequence of conversion rules (with any variable definitions among them)
//  is a conversion group.
//
//  A conversion rule is implementable if its text to replace is a single
//  character of a set (without strings or the text boundary), possibly with
//  before and after contexts, and its result is a fixed string (characters,
//  or variables defined as strings, without references, function calls or
//  text to revisit).  The rules of a group are implementable together as a
//  sequence of subgroups: the rules with the same contexts (as printed) form
//  a subgroup, in the order of their first rules, each a map from characters
//  to their replacement strings (the earliest rule for a character takes
//  precedence).

#pragma once

#include <map>
#include <string>
#include <vector>
#include <ldml/transform_rules.h>
#include <ucd/core/unicode_set.h>

namespace ldml {

struct CharMapSubgroup {
    // The contexts of the rules (nullptr if none), and the same contexts
    // as lookbehind and lookahead bodies for the regular expression engine
    // (see engineContext).
    re::RE * before = nullptr;
    re::RE * after = nullptr;
    re::RE * engineBefore = nullptr;
    re::RE * engineAfter = nullptr;
    std::string contextKey;     // the printed contexts, identifying the subgroup
    std::map<UCD::codepoint_t, std::u32string> charMap;
    unsigned rules = 0;         // the number of rules
};

struct TransformStep {
    enum class Kind {Conversion, Transform};
    Kind kind;
    // A conversion group: its subgroups.
    std::vector<CharMapSubgroup> subgroups;
    // A transform rule: the transform, and its filter set (if any).
    TransformID transform;
    re::RE * filter = nullptr;
    UCD::UnicodeSet filterSet;
};

struct TransformPlan {
    // The set of the filter rule, if any.
    re::RE * filter = nullptr;
    UCD::UnicodeSet filterSet;
    std::vector<TransformStep> steps;
    // The rules that are not implementable, each with the reason.
    std::vector<std::string> problems;
    bool implementable() const {return problems.empty();}
};

// The plan for the forward rules of a transform (as given by
// ExtractForwardRules or ExtractReverseBackwardRules).
TransformPlan planTransform(const std::vector<Rule *> & rules);

// A context (as given for the regular expression engine, see engineContext)
// rewritten for matching text in which each character c of the map has been
// followed by insertions[c] positions holding the filler codepoint, to make
// space for its replacement: each set item matches its characters, each
// followed by its fillers, and never matches the filler itself.  The filler
// should be a codepoint that cannot occur in the text (e.g., a surrogate), so
// that the inserted positions are distinguished from the characters of the
// text (including U+0000).
re::RE * expandedContext(re::RE * context, const std::map<UCD::codepoint_t, unsigned> & insertions,
                         UCD::codepoint_t filler);

// The characters that the set items of a pattern (e.g. a context) may match,
// including those of the variables it uses.
UCD::UnicodeSet patternCharacters(re::RE * pattern);

// The set items of a pattern (including the items of the variables it uses),
// in order: the characters of each, and whether it is repeated (within x*,
// x+, x{m,n} or x?).
struct PatternItem {
    UCD::UnicodeSet chars;
    bool repeated;
};
std::vector<PatternItem> patternItems(re::RE * pattern);

}

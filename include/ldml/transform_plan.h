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
//  A conversion rule is implementable if its text to replace has a fixed
//  length, each of its characters being of a set (without strings or the text
//  boundary), possibly with before and after contexts, and its result is a
//  fixed string (characters, or variables defined as strings, without
//  references, function calls or text to revisit).  The rules of a group
//  replacing single characters are implementable together as a sequence of
//  subgroups: the rules with the same contexts (as printed) form a subgroup,
//  in the order of their first rules, each mapping sets of characters to their
//  replacement strings (the earliest rule for a character takes precedence).
//  Each rule replacing a text of two or more characters is a string rule of
//  its own; no rule of the group may match within its text (other than at its
//  start), as the rules are applied to all positions at once.

#pragma once

#include <map>
#include <string>
#include <vector>
#include <ldml/transform_rules.h>
#include <ucd/core/unicode_set.h>

namespace ldml {

// The characters of a subgroup replaced by a string.
struct CharMapping {
    UCD::UnicodeSet chars;
    std::u32string replacement;
};

struct CharMapSubgroup {
    // The contexts of the rules (nullptr if none), and the same contexts
    // as lookbehind and lookahead bodies for the regular expression engine
    // (see engineContext).
    re::RE * before = nullptr;
    re::RE * after = nullptr;
    re::RE * engineBefore = nullptr;
    re::RE * engineAfter = nullptr;
    std::string contextKey;     // the printed contexts, identifying the subgroup
    // The characters of the rules, by their replacements: disjoint sets, one
    // per replacement string.
    std::vector<CharMapping> mappings;
    unsigned rules = 0;         // the number of rules
    // All the characters of the mappings.
    UCD::UnicodeSet characters() const;
};

// A conversion rule replacing a fixed-length text of two or more characters.
struct StringRule {
    std::vector<UCD::UnicodeSet> text;  // the characters at each position of the text
    std::u32string replacement;
    // The contexts, as for CharMapSubgroup.
    re::RE * before = nullptr;
    re::RE * after = nullptr;
    re::RE * engineBefore = nullptr;
    re::RE * engineAfter = nullptr;
    const ConversionRule * rule = nullptr;
};

struct TransformStep {
    enum class Kind {Conversion, Transform};
    Kind kind;
    // A conversion group: its subgroups, string rules, and implementable rules.
    std::vector<CharMapSubgroup> subgroups;
    std::vector<StringRule> stringRules;
    std::vector<const ConversionRule *> rules;
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
// rewritten for matching text in which each character of insertions[k] (the
// sets are disjoint) has been followed by k positions holding the filler
// codepoint, to make
// space for its replacement: each set item matches its characters, each
// followed by its fillers, and never matches the filler itself.  The filler
// should be a codepoint that cannot occur in the text (e.g., a surrogate), so
// that the inserted positions are distinguished from the characters of the
// text (including U+0000).
re::RE * expandedContext(re::RE * context, const std::map<unsigned, UCD::UnicodeSet> & insertions,
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

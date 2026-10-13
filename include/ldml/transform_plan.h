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
//  its own.  As the rules are applied to all positions at once, a rule that may
//  match within the text of a string rule (other than at its start) must be
//  kept from applying there: a single-character rule is, by the coverage of
//  the matches of the string rules (see TransformStep::coverStringRules); a
//  string rule is, if the string rules of the group can be applied in a single
//  pass (see StringRuleOverlaps, TransformStep::stringOrder and
//  StringRule::selfOverlapping); other string rules are not supported.

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
    // Whether the rule, of a text of two characters, may match within its own
    // text (e.g. rr → r): its matches then overlap where they end at
    // consecutive positions, and of each run of such matches, the first and
    // every other one after it apply (as ICU, scanning from the left, applies
    // the leftmost of overlapping matches).
    bool selfOverlapping = false;
};

struct TransformStep {
    enum class Kind {Conversion, Transform};
    Kind kind;
    // A conversion group: its subgroups, string rules, and implementable rules.
    std::vector<CharMapSubgroup> subgroups;
    std::vector<StringRule> stringRules;
    std::vector<const ConversionRule *> rules;
    // Whether a single-character rule may match within the text of a string
    // rule (other than at its start), where ICU, having replaced the text, does
    // not apply it: the rule then applies only at the positions not covered by
    // the matches of the string rules (other than their starts).
    bool coverStringRules = false;
    // If string rules may match within the texts of others, and they can be
    // applied in a single pass (see StringRuleOverlaps::singlePass): the string
    // rules in the order to apply them (indices in stringRules), and for each,
    // the rules within whose texts it may match (which precede it): a match of
    // the rule applies only if it does not begin within an applied match of
    // these.  Empty otherwise.
    std::vector<size_t> stringOrder;
    std::vector<std::vector<size_t>> stringPredecessors;
    // Otherwise (cyclic, two-way or longer self-overlaps), the matches are
    // selected sequentially, from the left: a match applies if it does not
    // begin within an applied match.  As no two string rules of a group match
    // at the same position (see DisambiguateOrder), this is the order in which
    // ICU applies them.  stringOrder then lists all the string rules, in
    // order, with no predecessors.
    bool sequentialSelection = false;
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

//  The overlaps of the string rules of a conversion group.
//
//  A string rule B overlaps a string rule A if B may match at a position
//  strictly within the text of a match of A (RuleOverlapAnalysis::
//  mayMatchWithin).  Where both match, ICU, scanning from the left, applies A,
//  the rule whose match begins first (if its first position is reached), and
//  not B.  For a pair of string rules, the overlap is
//    - one-way if B may match within A but not A within B: where both match
//      (overlapping), A always takes precedence (A → B);
//    - two-way if each may match within the other: which takes precedence
//      depends on which match begins first;
//    - a self-overlap if a rule may match within its own text (A → A): of its
//      overlapping matches, the leftmost takes precedence.
//  If the overlaps of a group are all one-way and the precedence graph
//  (A → B) is acyclic, the string rules can be applied sequentially, in a
//  single pass, in a topological order: the matches of each rule that apply
//  are those not beginning within an applied match of a rule preceding it.
//  The number of stages is the number of rules in the longest chain of the
//  graph.  A self-overlap is a cycle (of length 1): the overlapping matches of
//  a rule must be selected among themselves, from the left.
struct StringRuleOverlaps {
    enum class Kind {OneWay, TwoWay, Self};
    struct Pair {
        size_t outer;   // the rule within whose text the other may match (A)
        size_t inner;   // (B; for a two-way pair, outer < inner)
        Kind kind;
    };
    std::vector<Pair> pairs;    // indices in TransformStep::stringRules
    unsigned oneWay = 0, twoWay = 0, self = 0;
    // The precedence graph of the one-way pairs and self-overlaps: whether it
    // is acyclic, and if so its number of stages (0 without overlaps), and if
    // not a cycle (a sequence of rules, each preceding the next and the last
    // the first).
    bool acyclic = true;
    unsigned stages = 0;
    std::vector<size_t> cycle;
    // The same, for the graph without the self-overlaps: if acyclic, the rules
    // can be applied in a single pass as above, provided that the overlapping
    // matches of each self-overlapping rule are selected among themselves.
    bool acyclicApartFromSelf = true;
    unsigned stagesApartFromSelf = 0;
    // Whether the matches of each self-overlapping rule can be selected among
    // themselves: its text has two characters (see StringRule::selfOverlapping).
    bool selfSelectable = true;
    // Can the string rules be applied sequentially in a single pass?
    bool singlePass() const {return twoWay == 0 && (acyclic || (acyclicApartFromSelf && selfSelectable));}
    unsigned singlePassStages() const {return acyclic ? stages : stagesApartFromSelf;}
    // If the graph without the self-overlaps is acyclic: a topological order of
    // the rules, and for each rule the rules preceding it (one-way pairs).
    std::vector<size_t> order;
    std::vector<std::vector<size_t>> predecessors;
    // The strongly connected components of the graph of all the overlaps (one-
    // way, two-way and self) that are cycles: of two or more rules, or of one
    // self-overlapping rule.  Matches of two characters overlap only where they
    // begin at consecutive positions, whichever their rules: of a component of
    // rules of two characters, the matches that apply are those at the first
    // and every other position of each run of consecutive first positions of
    // the matches of its rules (as for the self-overlaps of one rule), and the
    // components, as single rules, form an acyclic graph.  pairSelectable: all
    // cyclic components consist of rules of two characters.
    unsigned cyclicComponents = 0;
    unsigned pairComponents = 0;    // cyclic components of rules of two characters
    size_t largestComponent = 0;    // rules in the largest cyclic component
    bool pairSelectable = true;
};
StringRuleOverlaps analyzeStringRuleOverlaps(const TransformStep & step);

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

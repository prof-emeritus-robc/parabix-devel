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
    // The characters of identity rules ((S) → $1): replaced by themselves, they
    // are not changed, but take precedence over later rules for them.
    UCD::UnicodeSet kept;
    unsigned rules = 0;         // the number of rules
    // All the characters of the mappings and kept.
    UCD::UnicodeSet characters() const;
};

// A conversion rule replacing a fixed-length text of two or more characters
// (or of one character, followed by a run).
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
    // A run: the text is followed by the longest run of characters of this set
    // (possibly none), which is deleted (S+ → x ; has the text S and the run S).
    // The rule then has no after context, and its group is selected
    // sequentially (TransformStep::sequentialSelection).
    UCD::UnicodeSet run;
};

// A MultipleCaptureMultipleReplacement rule: a conversion rule
//      before { T0 (B1) T1 (B2) ... (Bk) Tk } after → R0 $1 R1 $2 ... $k Rk ;
// with k >= 1 captures of any patterns Bi, fixed-length texts Ti (possibly
// empty), and fixed strings Ri; the references are in the order of the
// captures.  (With k = 1, A (B) C → D $1 E, it is a
// SingleCaptureDoubleReplacement rule.)  The captures stay in place: each Ti is
// replaced by Ri, or, if Ti is empty, Ri is inserted at its place.
//
// The rules of a group that differ only in T0 and R0 (with T0 not empty, of
// the same length) are matched together (see
// MultipleCaptureMultipleReplacementSet); a rule gives its T0 and R0.
struct MultipleCaptureMultipleReplacement {
    std::vector<UCD::UnicodeSet> first;     // T0: the characters at each position
    std::u32string firstReplacement;        // R0
    const ConversionRule * rule = nullptr;
};

// A replacement at a boundary of the matches of a set (at Ti, for i > 0, or
// at an empty T0), at a fixed offset from the start or the end of each match:
// the position start + offset, or end - offset (end: the last position of the
// match).  Kinds:
//   - Replace: the text at the position (Ti) is replaced by the replacement;
//   - Keep: the character at the position (the first or last character of a
//     capture; text: the characters it may be) is kept, and the replacement
//     inserted around it: before it (keepAt = the length of the replacement),
//     after it (keepAt = 0), or both (the replacements of an empty Ti and Ti+1
//     around a capture of one character).
struct BoundaryReplacement {
    enum class Kind {Replace, Keep};
    Kind kind = Kind::Replace;
    bool fromEnd = false;
    unsigned offset = 0;
    std::vector<UCD::UnicodeSet> text;
    std::u32string replacement;
    unsigned keepAt = 0;
    //  Keep: the capture (1 to k) whose character is kept.  An insertion before
    //  its first character and one after its last coincide where it has one
    //  character (not known in advance if its length varies).
    unsigned capture = 0;
};

// The MultipleCaptureMultipleReplacement rules of a group with the same
// captures, contexts, and texts and replacements other than T0 and R0, matched
// together by the regular expression engine (T0 B1 T1 ... Bk Tk, with the union
// of the sets of T0 at each position, and the captures possessive), which
// gives the spans of the matches and their ends (last positions).  The first
// positions of the matches are found:
//   - if startsByFirst: as the positions of the spans holding characters of
//     T0[0] (or, if T0 is empty, of the set with which B1 begins), which may
//     occur nowhere else in a match (so that any match begins there); the rule
//     applying is the first whose T0 matches there;
//   - otherwise: as the positions of the spans following a position outside
//     them, or the end of a match.  This requires that no match may end within
//     another (endsUnique): Tk's last characters (or, if Tk is empty, the
//     first characters of the after context) may occur nowhere else in a match.
// As information flows forward only, from the start of a match to its end,
// only T0 and R0 may differ between the rules of a set.
struct MultipleCaptureMultipleReplacementSet {
    std::vector<re::RE *> captures;                      // B1 ... Bk
    std::vector<std::vector<UCD::UnicodeSet>> texts;     // T1 ... Tk
    re::RE * before = nullptr;
    re::RE * after = nullptr;
    re::RE * engineBefore = nullptr;
    re::RE * engineAfter = nullptr;
    bool startsByFirst = false;
    //  If startsByFirst: the characters at which the matches begin (those of
    //  T0[0] or, with T0 empty, of the set with which B1 begins).
    UCD::UnicodeSet startChars;
    //  If the matches have one character: their starts are their ends.
    bool singleCharacter = false;
    //  Where the matches of different sets may overlap (captureSelection), and
    //  the starts are not known by the first characters: every position at
    //  which a match begins, found by the regular expression engine (the
    //  pattern with its contexts, as a lookahead), including those beginning
    //  within another match of the set (with the same end), which apply where
    //  that one does not.
    bool exactStarts = false;
    //  With exactStarts, whether the captures are matched greedily in the
    //  lookahead (the engine does not support possessive repetitions of sets
    //  with strings there): the same matches, if what follows each repetition
    //  cannot begin with a character it repeats.
    bool greedyStarts = false;
    std::vector<BoundaryReplacement> replacements;       // other than at a non-empty T0
    std::vector<std::u32string> results;                 // R1 ... Rk (as planned, in the order of the captures)
    std::vector<MultipleCaptureMultipleReplacement> rules;
    //  A rule whose result refers to its two captures in reverse order,
    //  T0 (B1) T1 (B2) T2 → R0 $2 R1 $1 R2, with B1 and B2 of fixed lengths, is
    //  planned as T0 (B1) T1 (B2) T2 → R0 R1 $1 $2 R2, and the region R1 B1 B2 of
    //  each match is then made B2 R1 B1 (see kernel::SwapBack): its last
    //  swapLength = |B2| positions are moved before its first
    //  swapDistance = |R1| + |B1|.  The region begins swapBefore = |R1| positions
    //  before the first character of B1 (after the replacements).
    bool reversed = false;
    unsigned swapLength = 0;
    unsigned swapDistance = 0;
    unsigned swapBefore = 0;
    //  More generally, a rule referring to its captures in order but for one
    //  capture j (0-based) referred to before captures i to j - 1,
    //      ... Q_i $j Q_(i+1) $i ... $(j-1) Q_(j+1) $(j+1) ...,
    //  with captures i and j never empty (of variable lengths, or more than two
    //  captures), is planned in order, with Q_i Q_(i+1) before capture i, the
    //  texts between $i and $(j-1) before their captures, and no text between
    //  capture j - 1 and capture j; the region from swapBefore = |Q_(i+1)|
    //  positions before capture i up to capture j of each match is then moved
    //  after capture j (moveFirst = i, moveCapture = j).
    bool moved = false;
    unsigned moveFirst = 0;
    unsigned moveCapture = 0;
    //  The characters of the region (of B1, B2 and R1), which may be in either
    //  order where a rule of the group sees the converted text: every set of
    //  the before contexts of the group must contain all of them or none.
    UCD::UnicodeSet swapChars;
};

// A conversion rule inserting a text where its contexts meet: A { } C → S, with
// an empty text to replace (W4).  Its before context ends with a character (of a
// set, possibly repeated, not the text boundary) and its after context begins
// with one: the insertion is before a character, which ICU reaches, and after
// one.  At each position, the first rule matching applies; the text inserted
// may not let a rule match there again (ICU would insert repeatedly).
//   - In a group of such rules alone, with before contexts of one character, the
//     converted text that the before contexts see is the text itself (each
//     insertion is before a character that stays in place).
//   - Otherwise (W4 step 2), the before contexts are matched against the
//     converted text, and the rules of the group applying after an insertion see
//     the text inserted.  No rule of the group before the insertion rule may then
//     apply at the position (its text beginning with a character with which the
//     after context may begin), and the group has no string rules (whose texts
//     could cover the position).
struct InsertionRule {
    UCD::UnicodeSet before;         // the characters with which the before context may end
    bool singleBefore = false;      // the before context is that character alone
    re::RE * engineBefore = nullptr;// the before context, as a lookbehind body (see engineContext)
    re::RE * after = nullptr;       // the after context
    UCD::UnicodeSet afterFirst;     // the first character of the after context
    re::RE * engineAfter = nullptr; // as a lookahead body (see engineContext), without captures
    std::u32string replacement;     // S
    const ConversionRule * rule = nullptr;
};

// A rule copying a capture of its before context (a BeforeCaptureCopy rule),
//      P (C) { T } A → R $1 ;
// whose result is a fixed string R followed by the converted text that the
// capture C of the end of its before context matches, and the other rules of
// its group whose texts begin with the same characters (the triggers): these
// rules are deferred (DeferredRule).  As ICU matches before contexts against
// the text it has converted, C copies converted text, and a trigger may see the
// result of a deferred rule before it (in ICU, P X { b → $1 ; turns X b b into
// X X X).  The deferred rules are therefore applied after the other rules of
// the group, which may not change the triggers (or the characters of their
// texts after the first, at the start of a match), to the converted text, in
// rounds: in each round, at the triggers whose before contexts may not reach
// back to a trigger not yet replaced (see TransformStep::deferredReach), the
// first deferred rule matching applies.  The captures are matched as ICU does,
// right to left from the end of the before context, possessively: a capture
// copied is a sequence of sets with at most one unbounded repetition of a set
// (CopyCapture), whose length at a trigger is then known.
struct CopyCapture {
    re::RE * pattern = nullptr;
    std::vector<UCD::UnicodeSet> prefix;    // the sets before the repetition (or all)
    bool star = false;                      // an unbounded repetition of starSet
    UCD::UnicodeSet starSet;
    std::vector<UCD::UnicodeSet> suffix;    // the sets after it
};

struct DeferredRule {
    std::vector<UCD::UnicodeSet> text;  // T: the characters at each position
    std::u32string replacement;         // R (before the copy, if any)
    int capture = -1;                   // the index of C in TransformStep::copyCaptures, or -1 (no copy)
    re::RE * engineBefore = nullptr;    // the before context, without the capture, as a lookbehind body
    re::RE * engineAfter = nullptr;     // the after context, as a lookahead body
    const ConversionRule * rule = nullptr;
    // A copy rule whose replacement R and copy c reproduce its before context
    // (the SelfReproducingContext pattern): P matches R (or, R empty, the end of
    // any copy), and C, matched right to left before R c, matches c again.  At a
    // run of its texts T, the rule applies at each, writing R c every time, as
    // long as its text and after context match there and those of no deferred
    // rule before it do.
    bool selfReproducing = false;
    // For a self-reproducing rule, the deferred rules before it that may apply at
    // the texts of its runs (indexes in TransformStep::deferred): those whose
    // before contexts, matched right to left, may match the block R c written
    // before each text (the others cannot apply there, whatever their texts and
    // after contexts).
    std::vector<size_t> runInterferers;
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
    // The MultipleCaptureMultipleReplacement rules (see MultipleCaptureMultipleReplacementSet), which may not overlap the string
    // rules; the single-character rules apply outside their matches.
    std::vector<MultipleCaptureMultipleReplacementSet> captureReplacements;
    // The insertion rules (see InsertionRule), in order.
    std::vector<InsertionRule> insertions;
    // Whether the matches of different sets may overlap: they are then selected
    // sequentially, from the left (the earlier set at the same position).
    bool captureSelection = false;
    // Whether string rules may overlap MultipleCaptureMultipleReplacement rules
    // (one matching within the other): the matches of both are then selected
    // together, sequentially from the left (sequentialSelection, with
    // captureSelection for the starts of the sets); no rules of both may match
    // at the same position.
    bool jointSelection = false;
    // The deferred rules (see DeferredRule), in order; the distinct captures C
    // they copy; the characters with which their texts begin (the triggers),
    // and those at each further position (the tails: tails[k] at position
    // k + 1); and the characters that the items of their before contexts may
    // match, other than the first (outermost) item: a before context can reach
    // back to a trigger only through these characters.
    std::vector<DeferredRule> deferred;
    std::vector<CopyCapture> copyCaptures;
    UCD::UnicodeSet deferredTriggers;
    std::vector<UCD::UnicodeSet> deferredTails;
    UCD::UnicodeSet deferredReach;
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

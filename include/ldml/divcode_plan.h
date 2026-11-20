/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  DivCode plans: a conversion group planned for the Div32Code / Div64Code
//  kernels (see doc/ldml-divcode-plan.md).
//
//  The characters that the rules of the group mention explicitly (in their
//  texts, and in the sets of their contexts or the complements of those sets)
//  are grouped into blocks of 2^s codepoints (s = 5: div32, s = 6: div64),
//  cp >> s.  The blocks are numbered from 1 in ascending order (their codes);
//  0 is the code of every other character.  The pseudo-codepoint of a
//  character, (code << s) | (cp & (2^s - 1)), identifies each character of
//  the blocks, and every explicit set is compiled as a set of pseudo-codepoints
//  (a set S whose complement is explicit: the complement of the pseudo-
//  codepoints of the complement, including those of code 0).
//
//  The rules of the group, its string rules and its single-character rules
//  (the mappings of its subgroups, as rules of one character), become DivRules:
//  a rule whose first set spans several blocks is split into one rule for each
//  block, so that each DivRule has the block of its first character (the
//  anchor) and lies in the leaf of that code.
//
//  A context is at most one item (a set, possibly with the text boundary).  An
//  item holds at a position of the text where its character is in the set; an
//  item with the text boundary (such as a negated set [^X], which ICU matches
//  at the start or end of the text) is tested as the negation of the
//  complement, ¬Advance(X) or ¬Lookahead(X), which holds at the boundary.  A
//  negated set excluding the boundary ([^X$]) does not hold there: after the
//  text it is guarded by the positions within the text (guardEnd).  The
//  set tested is explicit (in the alphabet) if it, or its complement, spans at
//  most maxSetBlocks blocks; otherwise it is external (a class stream computed
//  apart, as for a Unicode property).
//
//  Phase 1 scope (the problems otherwise): string rules and single-character
//  rules with literal results (no runs, kept characters, captures, insertion
//  rules, deferred rules or revisits); texts of explicit sets; no overlaps of
//  the string rules; contexts of at most one item, with no before context that
//  ICU would match differently in the converted text; at most maxCodeBits bits
//  of codes.

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <ldml/transform_plan.h>
#include <ucd/core/unicode_set.h>

namespace ldml {

struct DivCodeOptions {
    unsigned shift = 5;             // 5: div32, 6: div64
    unsigned maxSetBlocks = 16;     // an explicit context set (or its complement) spans at most this many blocks
    unsigned maxCodeBits = 10;      // at most 2^maxCodeBits - 1 blocks
    // For UTF-8 code units: each position of a text has characters of one
    // UTF-8 length (the first, split by blocks, always does).
    bool utf8 = false;
    // At most this many stages of string rules with one-way overlaps (see
    // DivRule::stage).
    unsigned maxStages = 8;
    // The rounds of matching the before contexts that ICU matches against the
    // converted text (see DivRule::converted); 0: matched against the text
    // itself, as with tconv --converted-context-rounds=0.
    unsigned convertedRounds = 8;
    // Whether a before context (as given for the regular expression engine)
    // may match differently in the text as converted by the group, against
    // which ICU matches it (or is not known not to).  If not given, no before
    // context is accepted.
    std::function<bool (const TransformStep & step, re::RE * engineBefore)> convertedBefore;
};

// A context item: holds at a position whose character is in set, or, if
// negated, whose character is not in set (and at the text boundary).
struct DivContextItem {
    // Stream: a whole after context that is not a fixed sequence of sets,
    // computed by the regular expression engine (DivCodePlan::afterStreams,
    // index external; doc/ldml-divcode-plan.md §12 step 3): holds at the
    // anchors (of a single-character rule) after which it matches.
    enum class Kind {None, Explicit, External, Stream};
    Kind kind = Kind::None;
    UCD::UnicodeSet set;
    bool negated = false;
    size_t external = 0;            // index in DivCodePlan::externalSets (Stream: in afterStreams)
    // An after item whose set includes the characters of code 0 (given by its
    // complement) without the boundary (as [^X$]).  (The translation kernel
    // tests every after item within the text.)
    bool guardEnd = false;
    // The set marks the end of a run of a global filter (see
    // marksFilterRunEnd): the item also holds at the positions outside the
    // filter's mask.
    bool runEnd = false;
    // An after item whose set has its characters outside the global filter
    // only (see marksFilterRunOutside, the mark removed from set): the item
    // holds at the positions outside the filter's mask only (none without a
    // mask).
    bool outside = false;
    // A before item of a context matched against the converted text, S T*
    // (doc/ldml-divcode-plan.md §12 step 2): the characters of through (T,
    // possessively repeated) next to the text are passed over, and the item
    // tests the converted character before them.  Allowed where no conversion
    // of the group moves a character into or out of T, so that the positions
    // of T's characters in the converted text are those of the text's.  The
    // set of through is tested as an item of throughKind (Explicit or External,
    // throughExternal).
    UCD::UnicodeSet through;
    Kind throughKind = Kind::None;
    size_t throughExternal = 0;
    bool hasThrough() const {return throughKind != Kind::None;}
    bool present() const {return kind != Kind::None;}
};

// The character of a result (not empty) that a before item sees after it: its
// last, or with a repeated set (DivContextItem::through), its last not in that
// set (the result's last if all are: a result of characters of the set, passed
// over as a whole).
inline char32_t lastSeen(const DivContextItem & item, const std::u32string & result) {
    if (item.hasThrough()) {
        for (auto i = result.rbegin(); i != result.rend(); ++i) if (!item.through.contains(*i)) return *i;
    }
    return result.back();
}

// A context: a fixed sequence of items (sets), from the one next to the text
// outward.  Only the outermost item may hold at the text boundary (negated):
// beyond an inner one, a character must still match.
struct DivContext {
    std::vector<DivContextItem> items;
    bool present() const {return !items.empty();}
};

// An earlier rule that a rule yields to at its first position (see
// TransformStep::stringPriority and subgroupPriority): a string rule (its
// index in TransformStep::stringRules), or the single-character rule of a
// subgroup matching chars.
struct DivEarlier {
    int stringRule = -1;
    int subgroup = -1;
    UCD::UnicodeSet chars;
};

struct DivRule {
    const ConversionRule * rule = nullptr;  // the string rule's rule, or nullptr for a mapping of a subgroup
    int stringRule = -1;                    // index in TransformStep::stringRules
    int subgroup = -1;                      // index in TransformStep::subgroups
    unsigned anchor = 0;                    // the code of the block of text[0]
    std::vector<UCD::UnicodeSet> text;      // text[0] within the anchor block
    std::u32string result;
    DivContext before, after;
    std::vector<DivEarlier> yieldsTo;
    // The stage of a string rule (from 1; 0 for a single-character rule).  With
    // one-way overlaps (see StringRuleOverlaps), a rule within whose texts a
    // rule may match precedes it: the stage of a rule is one more than the
    // highest stage of the rules preceding it (1 if none), and a match applies
    // unless it begins strictly within an applied match of an earlier stage.
    unsigned stage = 0;
    // A rule whose before context ICU matches against the converted text (it
    // may match differently there: see convertedContextMatters in tconv):
    // matched against the text itself first, then again in rounds against the
    // converted text that the matches of the round before give.  (Phase 1: a
    // single-character rule, with a before context of one item, that no rule
    // yields to.)
    bool converted = false;
    // A rule of a covering family (DivCodePlan::convertedFamilies, phase 2): its
    // index, or -1.  The family's match is certain; which of its rules applies
    // (by its before context, matched against the converted text) is resolved
    // as for the converted rules.
    int family = -1;
    size_t length() const {return text.size();}
    size_t expansion() const {return result.size() > text.size() ? result.size() - text.size() : 0;}
    size_t deletions() const {return text.size() > result.size() ? text.size() - result.size() : 0;}
};

// A family of string rules (doc/ldml-divcode-plan.md §11): the string rules of
// a group with the same text (the same sets at each position), some of whose
// before contexts are matched against the converted text.  It covers its text
// if, wherever the text matches with an after context of its rules, some rule
// of the family applies whatever the converted text before it is: for each
// region of the characters after the text (and the end of the text) that the
// after contexts of its rules distinguish, the before contexts of the rules
// whose after contexts hold there (or which have none) hold for every
// character before it and at the text boundary, or no rule's after context
// holds there.  Then the match is certain, and only its result depends on the
// converted text.
//
// A covering family is applied (DivRule::family) if its rules yield to the same
// rules outside it, and every rule yielding to one of them yields to all.  Its
// outcomes, as for the characters of the converted rules: the family
// propagates if its results differ in what a later before context sees
// (deleted, or the sets of DivCodePlan::convertedItems their last characters
// are in); otherwise its results are all deleted (deleted) or their last
// characters are in the set of item i or not (in[i]).
struct DivConvertedFamily {
    std::vector<int> stringRules;           // indices in TransformStep::stringRules, in rule order
    bool covering = false;
    std::string reason;                     // why it does not cover its text, or is not applied
    // The synthetic rules completing it (§11, fallback 2): for the converted
    // characters its before contexts miss, the result the group's rules give.
    size_t completed = 0;
    bool propagating = false;
    bool deleted = false;
    std::vector<bool> in;
};

// Whether covering families are planned (phase 2; tconv --divcode-families):
// otherwise their groups keep the problem, as before phase 2.
extern bool DivCodeFamilies;

struct DivCodePlan {
    unsigned shift = 5;
    std::vector<UCD::codepoint_t> blocks;   // ascending; the code of blocks[i] is i + 1
    unsigned codeBits = 0;
    std::vector<UCD::UnicodeSet> externalSets;
    // The after contexts computed by the regular expression engine (the
    // engine's forms, see DivContextItem::Kind::Stream), and their rules as
    // written (for the plan's hash).
    std::vector<re::RE *> afterStreams;
    std::vector<std::string> afterStreamKeys;
    std::vector<DivRule> rules;             // by leaf (anchor code), in rule order within a leaf
    std::vector<std::vector<size_t>> leaves;    // leaves[code - 1]: the rules anchored there
    // The context items (before, after) of each subgroup of the step, for the
    // earlier single-character rules that rules yield to.
    std::vector<std::pair<DivContext, DivContext>> subgroupContexts;
    size_t sourceRules = 0;                 // string rules + mappings, before splitting
    unsigned stages = 0;                    // the number of stages of the string rules (see DivRule::stage)
    size_t overlapPairs = 0;                // the one-way overlaps of the string rules
    // The rules with DivRule::converted (doc/ldml-divcode-plan.md §10): the
    // distinct items of their before contexts, and for the characters of their
    // texts, those whose possible outcomes at a dependent position (the results
    // of these rules there, and the character itself if none may apply) differ
    // in what a later before context sees: deleted, or the sets of
    // convertedItems their last character is in.  The others (consumers) share
    // one outcome: deleted (consumerDeleted), or a last character in the set of
    // item i (consumerIn[i]) or not.
    std::vector<DivContextItem> convertedItems;
    // The families of the string rules with before contexts matched against
    // the converted text (phase 2: see DivConvertedFamily).
    std::vector<DivConvertedFamily> convertedFamilies;
    UCD::UnicodeSet propagating;
    UCD::UnicodeSet consumerDeleted;
    std::vector<UCD::UnicodeSet> consumerIn;
    size_t convertedRules = 0;              // the rules with DivRule::converted
    size_t familyRules = 0;                 // the rules with DivRule::family
    // Whether some rule's before context is matched against the converted text
    // (the Converted kernel: doc/ldml-divcode-plan.md §10, §11).
    bool dependent() const {return convertedRules > 0 || familyRules > 0;}
    // Whether some outcome depends on the converted character before it: the
    // characters of propagating, or the matches of a family whose results
    // differ (the Converted kernel's loop evaluates them in order).
    bool propagates() const {
        if (!propagating.empty()) return true;
        for (const DivConvertedFamily & f : convertedFamilies) if (f.propagating) return true;
        return false;
    }
    std::string contentHash;               // of the whole plan
    std::vector<std::string> problems;
    bool eligible() const {return problems.empty();}
    // The code of a codepoint (0 if not in a block of the alphabet).
    unsigned codeOf(UCD::codepoint_t cp) const;
    // The pseudo-codepoints of a set within the alphabet (each of its
    // characters must be in a block of the alphabet).
    UCD::UnicodeSet pseudoSet(const UCD::UnicodeSet & set) const;
    // The pseudo-codepoints testing a set within the alphabet or whose
    // complement is (the complement of the pseudo-codepoints of the complement,
    // including those of code 0).
    UCD::UnicodeSet pseudoTest(const UCD::UnicodeSet & set) const;
};

// The DivCode plan of a conversion group (with the problems that keep it from
// being eligible, if any).
DivCodePlan planDivCodeGroup(const TransformStep & step, const DivCodeOptions & options);

}

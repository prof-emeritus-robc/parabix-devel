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
    // Whether a before context (as given for the regular expression engine)
    // may match differently in the text as converted by the group, against
    // which ICU matches it (or is not known not to).  If not given, no before
    // context is accepted.
    std::function<bool (const TransformStep & step, re::RE * engineBefore)> convertedBefore;
};

// A context item: holds at a position whose character is in set, or, if
// negated, whose character is not in set (and at the text boundary).
struct DivContextItem {
    enum class Kind {None, Explicit, External};
    Kind kind = Kind::None;
    UCD::UnicodeSet set;
    bool negated = false;
    size_t external = 0;            // index in DivCodePlan::externalSets
    // An after item whose test would hold on the zero bits beyond the end of
    // the text (a set of nearly all characters without the boundary, as
    // [^X$]): ANDed with a lookahead of the positions within the text.
    bool guardEnd = false;
    bool present() const {return kind != Kind::None;}
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
    DivContextItem before, after;
    std::vector<DivEarlier> yieldsTo;
    size_t length() const {return text.size();}
    size_t expansion() const {return result.size() > text.size() ? result.size() - text.size() : 0;}
    size_t deletions() const {return text.size() > result.size() ? text.size() - result.size() : 0;}
};

struct DivCodePlan {
    unsigned shift = 5;
    std::vector<UCD::codepoint_t> blocks;   // ascending; the code of blocks[i] is i + 1
    unsigned codeBits = 0;
    std::vector<UCD::UnicodeSet> externalSets;
    std::vector<DivRule> rules;             // by leaf (anchor code), in rule order within a leaf
    std::vector<std::vector<size_t>> leaves;    // leaves[code - 1]: the rules anchored there
    size_t sourceRules = 0;                 // string rules + mappings, before splitting
    std::string contentHash;                // of the whole plan
    std::vector<std::string> problems;
    bool eligible() const {return problems.empty();}
    // The code of a codepoint (0 if not in a block of the alphabet).
    unsigned codeOf(UCD::codepoint_t cp) const;
    // The pseudo-codepoints of a set within the alphabet (each of its
    // characters must be in a block of the alphabet).
    UCD::UnicodeSet pseudoSet(const UCD::UnicodeSet & set) const;
};

// The DivCode plan of a conversion group (with the problems that keep it from
// being eligible, if any).
DivCodePlan planDivCodeGroup(const TransformStep & step, const DivCodeOptions & options);

}

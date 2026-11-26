/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Entity plans: a conversion group planned on its named entities, without
//  expanding them into their members (see doc/ldml-entity-groups-plan.md).
//
//  The rules are taken as written, in their order (the forward rules, before
//  capture elimination, order disambiguation or expansion).  Each set of a
//  rule (in its text or contexts) is an entity of the group:
//    - Property: a Unicode property (a stream of its own, shared across
//      groups);
//    - PropertySet: a set expression over properties (and characters), such
//      as [^[:L:][:M:][:N:]], computed from the property entities (operands);
//    - PureSet: a set of single characters (a class of the group's CC
//      kernel);
//    - StringSet: a set with strings, whose longest match at each position
//      (as ICU matches a set) is given by a StringSet kernel, one stream per
//      length of a match (lengths);
//    - Union: a union of entities of which one at least is a StringSet (such
//      as $Vowel = [$IVowel $UVowel $AVowel $SchwaVowel]), derived from its
//      operands: its longest match is the longest of theirs.
//  Entities are deduplicated by content, and named by the first variable
//  denoting them (or inline_n).  A single character of a text or context is a
//  literal item, not an entity.

#pragma once

#include <string>
#include <vector>
#include <ucd/core/unicode_set.h>

namespace re { class RE; }

namespace ldml {

class Rule;
class ConversionRule;

struct EntityOptions {
    //  The longest text of a rule (in characters).
    unsigned maxTextLength = 16;
};

struct Entity {
    enum class Kind {Property, PropertySet, PureSet, StringSet, Union};
    Kind kind = Kind::PureSet;
    std::string name;                   // the variable (with $) or inline_n
    bool variable = false;              // named by a variable
    const re::RE * re = nullptr;        // the first occurrence
    std::string text;                   // the first occurrence, in rule syntax
    //  The single-character members (Property, PropertySet: the codepoints).
    UCD::UnicodeSet chars;
    //  StringSet: the members of two or more characters, sorted.
    std::vector<std::u32string> strings;
    //  The text boundary is a member (as of a negated set).
    bool boundary = false;
    //  StringSet, Union: the lengths of the longest matches, ascending (1 for
    //  the others).
    std::vector<unsigned> lengths;
    //  Union: the entities united (with the inline remainder, if any);
    //  PropertySet: the property entities.
    std::vector<size_t> operands;
    bool inText = false;
    bool inBefore = false;
    bool inAfter = false;
    //  The characters with which a match may begin, and all the characters
    //  of its matches.
    UCD::UnicodeSet firstChars() const;
    UCD::UnicodeSet allChars() const;
};

const char * kindName(Entity::Kind k);

//  A string in UTF-8 (for reports).
std::string entityUTF8(const std::u32string & s);

struct EntityItem {
    enum class Kind {Literal, Entity, Boundary};
    Kind kind = Kind::Literal;
    char32_t literal = 0;
    size_t entity = 0;
    bool optional = false;              // X? (possessive)
};

struct EntityRule {
    const ConversionRule * rule = nullptr;
    std::string text;                   // the rule, in rule syntax
    std::vector<EntityItem> before;
    std::vector<EntityItem> items;      // the text to replace
    std::vector<EntityItem> after;
    std::u32string result;
    unsigned minLength = 0;             // of the text, in characters
    unsigned maxLength = 0;
    std::vector<std::string> problems;
};

struct EntityGroupPlan {
    size_t number = 0;                  // the group's number in the transform, from 1
    std::vector<Entity> entities;
    std::vector<EntityRule> rules;

    //  The sets of the group's CC kernel (§3.1), distinct, in order of first
    //  use: streamSets, the pure sets, the literal characters and the sets of
    //  characters within property sets (a stream each, without a code);
    //  codedSets, these and, for the string sets, their single-character parts
    //  and each character of their strings (with a code, such as the class
    //  layer's).  The choice of code is the client's.
    std::vector<UCD::UnicodeSet> streamSets;
    std::vector<UCD::UnicodeSet> codedSets;
    //  The sets with which the rules' matches may begin (by rule), for the
    //  leaves of a class layer's criterion: the union of the first characters
    //  of the leading optional items and of the first required item.
    std::vector<UCD::UnicodeSet> firstSets;

    //  The rules that may begin within a match of each rule (a conservative
    //  test on the characters: at an offset of 1 or more of the rule's matches,
    //  a character with which the other rule's match may begin).  Pruned: not
    //  counting a rule whose before context is one item, a set none of whose
    //  characters occur in the group's texts (it cannot hold within a match).
    std::vector<std::vector<size_t>> mayBeginWithin;
    std::vector<std::vector<size_t>> mayBeginWithinPruned;
    //  Of the pruned graph: the rules on cycles, and the depth of the others
    //  (the stages of §5.4), or 0 with cycles.
    std::vector<size_t> cycleRules;
    unsigned depth = 0;
    //  The stages of the rules' selection (§5.4) by the graph itself
    //  (mayBeginWithin, not pruned): the depth of each rule, the longest
    //  chain of rules within whose matches it may begin (0: none), and the
    //  rules on cycles (none: the stages select exactly).
    std::vector<unsigned> stage;
    std::vector<size_t> stageCycleRules;

    //  The before contexts matched against the converted text (§5.3): for each
    //  distinct one-item before context, the conditions of the closed form.
    struct BeforeState {
        size_t entity = 0;              // or the boundary alone (literal: SIZE_MAX)
        std::vector<size_t> rules;      // the rules with this before context
        bool textDisjoint = false;      // (a) no character of a text is in the set
        bool resultsOutside = false;    // (b) no non-empty result ends with a character of the set
        bool singleDeletions = false;   // (c) the deletions under the state are single positions
        std::vector<std::string> notes;
    };
    std::vector<BeforeState> beforeStates;
    //  Whether a before context's state fails the closed form of §5.3, (a)
    //  to (c): the state is then computed by the correction loop (K1/K0).
    bool contextLoop = false;
    std::vector<std::string> contextLoopReasons;

    std::vector<std::string> problems;
    bool eligible() const {return problems.empty();}
};

//  The entity plans of the conversion groups of a transform's rules applying
//  forward (see ExtractForwardRules): the maximal sequences of conversion
//  rules between filter and transform rules.
std::vector<EntityGroupPlan> planEntityGroups(const std::vector<Rule *> & forwardRules,
                                              const EntityOptions & options = EntityOptions());

}

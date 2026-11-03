/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Abstract data types for the rules of LDML transforms, i.e., the content
//  of the <tRule> elements described in UTS #35 Part 2, section "Transforms".
//  https://unicode.org/reports/tr35/tr35-general.html#Transform_Rules_Syntax
//
//  Rules are defined in a class hierarchy supporting llvm::isa, llvm::dyn_cast
//  in the same style as the Parabix RE data types.
//
//      Rule
//        FilterRule              :: [:Latin:] ;       :: ([:Latin:]) ;
//        TransformRule           :: NFD (NFC) ;       :: [a-z] Upper ;
//        VariableDefinitionRule  $vowel = [aeiou] ;
//        ConversionRule          a { b | c } d ↔ e { f | g } h ;
//
//  All patterns (UnicodeSets, variable contents, contexts and texts of
//  conversion rules) are represented using Parabix RE objects:
//
//      literal character        re::CC (single codepoint)
//      literal string           re::Seq of CCs
//      UnicodeSet [a-z]         re::CC, re::Alt (union), re::Diff, re::Intersect,
//                               re::PropertyExpression ([:Lu:], \p{Script=Grek}),
//                               re::Any
//      .                        any character except line separators
//                               (see makeDotSet below)
//      [$] in a UnicodeSet      text boundary: re::Alt{re::Start, re::End}
//                               (see makeTextBoundary below)
//      ^ (before context)       re::Start
//      $ (end of after context) re::End
//      $var                     re::Name, whose definition is the variable content
//      x* x+ x?                 re::Rep (possessive quantifiers)
//      ( ... )                  re::Capture named "1", "2", ...
//      $1 .. $9                 re::Reference to the capture of the other side
//      &Any-Hex($1)             function call: re::Name in the "&" namespace
//                               (see makeFunctionCall below)
//

#pragma once

#include <map>
#include <memory>
#include <string>
#include <set>
#include <vector>
#include <functional>
#include <llvm/Support/Casting.h>
#include <allocator/threadsafe_slaballocator.h>
#include <re/adt/re_re.h>
#include <ucd/core/unicode_set.h>

namespace re { class Name; class Capture; class CC; }

namespace ldml {

// Rules are defined in a class hierarchy supporting llvm::isa, llvm::dyn_cast.
#define LDML_RULE_SUBTYPE(kind) \
static inline bool classof(const Rule * r) {return r->getClassTypeId() == ClassTypeId::kind;}\
static inline bool classof(const void *) {return false;}

// Directions in which a rule applies.  Forward rules apply to the
// normal transform, backward rules to the inverse transform.
enum class Direction : unsigned {
    Forward = 1
    , Backward = 2
    , Both = 3
};

inline bool appliesForward(const Direction d) {
    return (static_cast<unsigned>(d) & static_cast<unsigned>(Direction::Forward)) != 0;
}

inline bool appliesBackward(const Direction d) {
    return (static_cast<unsigned>(d) & static_cast<unsigned>(Direction::Backward)) != 0;
}

class Rule : public SlabAllocatedObject {
public:
    enum class ClassTypeId : unsigned {
        Filter
        , Transform
        , VariableDefinition
        , Conversion
    };
    inline ClassTypeId getClassTypeId() const {
        return mClassTypeId;
    }
protected:

    USE_SLAB_ALLOCATED_OBJECT_MEMORY_OPERATORS

    inline Rule(const ClassTypeId id)
    : mClassTypeId(id) {

    }
    const ClassTypeId mClassTypeId;
};

//  :: [:Latin:] ;       a (forward) filter rule
//  :: ([:Latin:]) ;     an inverse filter rule
//
//  The filter rule, if present, must be the first rule of the list.  The
//  inverse filter rule, if present, must be the last.
class FilterRule : public Rule {
public:
    re::RE * getFilterSet() const {return mFilterSet;}
    bool isInverse() const {return mInverse;}
    Direction getDirection() const {return mInverse ? Direction::Backward : Direction::Forward;}
    static FilterRule * Create(re::RE * filterSet, const bool inverse) {
        return new FilterRule(filterSet, inverse);
    }
    LDML_RULE_SUBTYPE(Filter)
private:
    FilterRule(re::RE * filterSet, const bool inverse)
    : Rule(ClassTypeId::Filter), mFilterSet(filterSet), mInverse(inverse) {}
    re::RE * const mFilterSet;
    const bool mInverse;
};

inline FilterRule * makeFilterRule(re::RE * filterSet) {
    return FilterRule::Create(filterSet, false);
}

inline FilterRule * makeInverseFilterRule(re::RE * filterSet) {
    return FilterRule::Create(filterSet, true);
}

//  A transform identifier, of the form source-target/variant, e.g.,
//  "Latin-Greek", "und_Latn-und_Grek", "Any-Hex/Unicode", "NFD".
//  If the source is omitted, it is "Any".   BCP47 forms such as
//  "und-Latn-t-und-grek" are also recognized.
class TransformID {
public:
    TransformID() = default;
    // Parse a transform identifier.
    explicit TransformID(const std::string & id);
    TransformID(std::string source, std::string target, std::string variant = "");
    bool empty() const {return mText.empty();}
    // The identifier as written.
    const std::string & getText() const {return mText;}
    const std::string & getSource() const {return mSource;}
    const std::string & getTarget() const {return mTarget;}
    const std::string & getVariant() const {return mVariant;}
    bool hasVariant() const {return !mVariant.empty();}
    // The canonical form source-target[/variant].
    std::string getCanonicalName() const;
    // The identifier of the inverse transform.  The inverses of the
    // Unicode-defined transforms are special cases, e.g., the inverse of
    // Any-Upper is Any-Lower and the inverse of Any-NFD is Any-NFC.
    TransformID inverse() const;
    // Is this one of the transforms defined by the Unicode standard
    // (NFC, NFD, NFKC, NFKD, Lower, Upper, Title) or Null or Remove?
    bool isBuiltIn() const;
private:
    std::string mText;
    std::string mSource;
    std::string mTarget;
    std::string mVariant;
};

//  Transform rules invoke another transform on the whole string.
//      :: X ;          forward X, backward the inverse of X
//      :: X (Y) ;      forward X, backward Y
//      :: X () ;       forward X only
//      :: (Y) ;        backward Y only
//  Each transform may optionally be restricted by a UnicodeSet filter:
//      :: [:Latin:] Upper ([:Latin:] Lower) ;
class TransformRule : public Rule {
public:
    bool hasForward() const {return !mForwardID.empty();}
    bool hasBackward() const {return !mBackwardID.empty();}
    const TransformID & getForwardID() const {return mForwardID;}
    // The transform applied for the inverse transform, either given
    // explicitly or as the inverse of the forward transform.
    const TransformID & getBackwardID() const {return mBackwardID;}
    // Was a parenthesized inverse part (possibly empty) given?
    bool hasExplicitInverse() const {return mExplicitInverse;}
    // Optional filters restricting the transforms (nullptr if none).
    re::RE * getForwardFilter() const {return mForwardFilter;}
    re::RE * getBackwardFilter() const {return mBackwardFilter;}
    Direction getDirection() const;
    static TransformRule * Create(TransformID forward, re::RE * forwardFilter,
                                  bool explicitInverse,
                                  TransformID backward, re::RE * backwardFilter) {
        return new TransformRule(std::move(forward), forwardFilter, explicitInverse, std::move(backward), backwardFilter);
    }
    LDML_RULE_SUBTYPE(Transform)
private:
    TransformRule(TransformID forward, re::RE * forwardFilter,
                  bool explicitInverse,
                  TransformID backward, re::RE * backwardFilter)
    : Rule(ClassTypeId::Transform)
    , mForwardID(std::move(forward)), mForwardFilter(forwardFilter)
    , mExplicitInverse(explicitInverse)
    , mBackwardID(std::move(backward)), mBackwardFilter(backwardFilter) {}
    const TransformID mForwardID;
    re::RE * const mForwardFilter;
    const bool mExplicitInverse;
    const TransformID mBackwardID;
    re::RE * const mBackwardFilter;
};

// :: X ;
TransformRule * makeTransformRule(TransformID forward, re::RE * filter = nullptr);
// :: X (Y) ;   :: X () ;   :: (Y) ;    (an empty TransformID denotes an absent part)
TransformRule * makeTransformRule(TransformID forward, re::RE * forwardFilter,
                                  TransformID backward, re::RE * backwardFilter);

//  $variableName = contents ;
//
//  The variable is represented by an re::Name whose definition is the
//  content.   References to the variable in later rules are references
//  to the same re::Name object.
class VariableDefinitionRule : public Rule {
public:
    re::Name * getVariable() const {return mVariable;}
    std::string getName() const;
    re::RE * getDefinition() const;
    static VariableDefinitionRule * Create(re::Name * variable) {
        return new VariableDefinitionRule(variable);
    }
    LDML_RULE_SUBTYPE(VariableDefinition)
private:
    VariableDefinitionRule(re::Name * variable)
    : Rule(ClassTypeId::VariableDefinition), mVariable(variable) {}
    re::Name * const mVariable;
};

VariableDefinitionRule * makeVariableDefinitionRule(const std::string & name, re::RE * definition);

//  One side of a conversion rule:
//
//      before_context { completed_result | result_to_revisit } after_context
//
//  When the side is the source of a conversion (the left side of → or the
//  right side of ←), the text_to_replace is the concatenation of the
//  completed result and the result to revisit, i.e., the cursor is ignored.
//  When the side is the result, the contexts are ignored.
//
//  The cursor offset is the number of '@' fillers placing the cursor before
//  the start (negative, |@@abc) or after the end (positive, abc@@|) of the result.
class RuleSide : public SlabAllocatedObject {
public:
    // Contexts are nullptr when absent.
    re::RE * getBeforeContext() const {return mBeforeContext;}
    re::RE * getAfterContext() const {return mAfterContext;}
    bool hasBeforeContext() const {return mBeforeContext != nullptr;}
    bool hasAfterContext() const {return mAfterContext != nullptr;}
    // text_to_replace / resulting_text, ignoring the cursor.
    re::RE * getText() const {return mText;}
    re::RE * getCompletedResult() const {return mCompletedResult;}
    re::RE * getResultToRevisit() const {return mResultToRevisit;}
    bool hasCursor() const {return mHasCursor;}
    int getCursorOffset() const {return mCursorOffset;}
    static RuleSide * Create(re::RE * before, re::RE * completed, bool hasCursor, re::RE * revisit, int cursorOffset, re::RE * after) {
        return new RuleSide(before, completed, hasCursor, revisit, cursorOffset, after);
    }
protected:
    USE_SLAB_ALLOCATED_OBJECT_MEMORY_OPERATORS
private:
    RuleSide(re::RE * before, re::RE * completed, bool hasCursor, re::RE * revisit, int cursorOffset, re::RE * after);
    re::RE * const mBeforeContext;
    re::RE * const mCompletedResult;
    re::RE * const mResultToRevisit;
    re::RE * const mText;
    re::RE * const mAfterContext;
    const bool mHasCursor;
    const int mCursorOffset;
};

// A side without contexts or cursor.
RuleSide * makeRuleSide(re::RE * text);
RuleSide * makeRuleSide(re::RE * before, re::RE * text, re::RE * after);
RuleSide * makeRuleSide(re::RE * before, re::RE * completed, re::RE * revisit, int cursorOffset, re::RE * after);

//  Conversion rules:
//      before_context { text_to_replace } after_context → completed_result | result_to_revisit ;
//      completed_result | result_to_revisit ← before_context { text_to_replace } after_context ;
//      a { b | c } d ↔ e { f | g } h ;
class ConversionRule : public Rule {
public:
    Direction getDirection() const {return mDirection;}
    RuleSide * getLeftSide() const {return mLeft;}
    RuleSide * getRightSide() const {return mRight;}
    // The side matched against the input text when applying the rule in
    // the given direction (Forward or Backward), and the side giving the result.
    RuleSide * getSourceSide(const Direction d) const {return d == Direction::Backward ? mRight : mLeft;}
    RuleSide * getResultSide(const Direction d) const {return d == Direction::Backward ? mLeft : mRight;}
    static ConversionRule * Create(RuleSide * left, Direction d, RuleSide * right) {
        return new ConversionRule(left, d, right);
    }
    LDML_RULE_SUBTYPE(Conversion)
private:
    ConversionRule(RuleSide * left, Direction d, RuleSide * right)
    : Rule(ClassTypeId::Conversion), mLeft(left), mDirection(d), mRight(right) {}
    RuleSide * const mLeft;
    const Direction mDirection;
    RuleSide * const mRight;
};

inline ConversionRule * makeConversionRule(RuleSide * left, Direction d, RuleSide * right) {
    return ConversionRule::Create(left, d, right);
}

// Helpers for the transform-specific uses of RE objects.

// The text boundary, i.e., "$" within a UnicodeSet (including the boundary
// implied by a negated set [^...]).  In a before context it matches the start
// of the text, and is represented by re::Start; in an after context it
// matches the end of the text, and is represented by re::End.  Within a
// variable definition, whose context is not known, it is represented by the
// text boundary marker made by makeTextBoundary(); each use of such a variable
// in a context is a resolved copy of the variable (see resolveTextBoundary).
re::RE * makeTextBoundary();
// The set [$] of a before context (the text boundary as Start) or an after
// context (as End), printed as [$].
re::RE * makeBoundarySet(bool beforeContext);
// Is the RE the (unresolved) text boundary marker?
bool isTextBoundary(const re::RE * re);
// Is the RE a text boundary: Start, End or the marker?
bool isBoundary(const re::RE * re);
// Does a set include the text boundary (Start, End or the marker as a member)?
bool includesTextBoundary(const re::RE * set);
// The pattern with the text boundary marker resolved as Start (for a before
// context), End (for an after context, or the last of two or more items of a
// text) or None (no match, for the other items of a text),
// including within the variables it uses: a variable whose definition
// includes the marker is replaced by a resolved copy, with the same name.
enum class BoundaryResolution {Start, End, None};
// A segment containing the marker is rebuilt; if captures is given, each
// rebuilt segment is recorded there (for the references to it).
re::RE * resolveTextBoundary(re::RE * re, BoundaryResolution resolution,
                             std::map<re::Capture *, re::Capture *> * captures = nullptr);
// Does a set include the text boundary in some context: Start, End or the
// marker, possibly through a variable whose definition includes the marker
// (although a resolved copy of it may not)?
bool mayIncludeTextBoundary(const re::RE * set);
// The variable of which a variable is a resolved copy (or the variable itself).
const re::Name * originalVariable(const re::Name * n);
// A context for matching with the regular expression engine, as a lookbehind
// (before context) or lookahead (after context): the text boundary is dropped
// from the sets that include it (as negated sets do) wherever a character
// must be matched beyond the set (before it in a before context, after it in
// an after context), since the start or end of the text cannot then be
// matched there.  A negated set [^X] that is the last item of the context
// (the first of a before context) becomes the negative assertion (?!X), or
// (?<!X) in a before context, which also holds at the start or end of the
// text.  Variables (other than function calls) whose definitions change are
// replaced by their changed definitions.
re::RE * engineContext(re::RE * context, bool afterContext);
// A context for the regular expression engine (see engineContext), with its
// repetitions possessive and its sets with strings taking their longest
// strings, as ICU matches them.  An after context is matched forward from the
// position: its repetitions are made possessive (re::Rep::Kind::Possessive),
// which the engine resolves, taking the longest string of a set with strings
// at each repetition, as ICU does (see re::PossessiveChoice); a set with
// strings that is not repeated is made unambiguous, w (?!t'), where w is a
// proper prefix of a longer string w t' (see re::unambiguousAlternatives).  A
// before context is matched backward from the position, so that each
// repetition takes as many repetitions as possible to its left: e{lb,} becomes
// (?<!e) e{lb,}, and e{lb,ub} becomes (?<!e) e{lb,ub-1} | e{ub}; and its sets
// with strings, repeated or not, are made unambiguous backward, (?<!t') w,
// where w is a proper suffix of a longer string t' w, so that each takes its
// longest string ending at the position.  (The engine resolves possessive
// repetitions forward, also within lookbehinds.)
re::RE * possessiveContext(re::RE * context, bool afterContext);
// A capture for the regular expression engine, with its repetitions possessive
// (as an after context, possessiveContext), its sets with strings that are not
// repeated left as they are.
re::RE * possessiveCapture(re::RE * capture);

// The set of "." in rules, matching any character other than line and
// paragraph separators: [^[:Zp:][:Zl:]\r\n$]
re::RE * makeDotSet();
bool isDotSet(const re::RE * re);

// &Any-Hex/Unicode($1) : a function call within a resulting text.
re::Name * makeFunctionCall(const TransformID & id, re::RE * argument);
bool isFunctionCall(const re::RE * re);
TransformID getFunctionID(const re::Name * call);

// Does the rule apply in the given direction?
bool appliesInDirection(const Rule * r, Direction d);

//  Extraction of the rules for one direction, expressed as forward rules
//  (see "Inverse Summary" in UTS #35 Part 2).
//
//  In the extracted rules, each conversion rule is a forward (→) rule whose
//  source side has its contexts but no cursor, and whose result side has
//  its cursor but no contexts.   Transform rules are given with the single
//  transform applying in that direction.   Variable definitions are retained
//  only for the variables used in the extracted rules, directly or within
//  the definitions of other used variables.
//
//  A transform rule not applying in the direction still separates the
//  conversion rules before and after it into two groups; where needed,
//  this is preserved by a ":: Null ;" rule.

//  The rules applying in the forward direction, in the original order,
//  omitting the backward conversion rules, backward-only transform rules
//  and the inverse filter rule.
std::vector<Rule *> ExtractForwardRules(const std::vector<Rule *> & rules);

//  The rules applying in the backward direction, restated as forward rules:
//  the inverse filter rule becomes the (forward) filter rule, the transform
//  rules and groups of conversion rules are in reverse order (the rules
//  within each group keep their order), the sides of each conversion rule
//  are exchanged and each transform rule is replaced by its inverse.
//  Variable definitions are placed after the filter rule, before all
//  other rules.
std::vector<Rule *> ExtractReverseBackwardRules(const std::vector<Rule *> & rules);

//  Character class partition.
//
//  For each group of rules (each maximal sequence of conversion rules and
//  variable definitions, i.e., the rules between filter and transform
//  rules), a partition of the characters of the group into mutually
//  exclusive classes.   The variables of a group are those used by its
//  conversion rules, including the variables used in the definitions of
//  variables that are not sets (wherever defined).  The classes are such
//  that:
//    - every character occurring directly in a rule (in the text of
//      conversion rules or variable definitions, including strings in sets)
//      is a class by itself, denoted by the character or by an existing
//      variable defined as that single character;
//    - every variable used in the group defining a UnicodeSet is the union
//      of classes;
//    - every set written inline is the union of classes: the maximal set
//      expressions in conversion rules and in the definitions of variables
//      that are not sets (e.g., [:Separator:] in $space = [:Separator:]* ;).
//  Filter rules and transform rules (and their filters) are not included.
//  Strings within sets (e.g. {ch}) are not part of the character sets; their
//  characters occur directly.   The partition is the coarsest such
//  partition: two characters are in the same class exactly when they are
//  contained in the same sets and neither occurs directly.   Thus each set
//  is divided into the minimal number of subsets consistent with no subsets
//  intersecting.
//
//  Each class that is not a directly occurring character is denoted by an
//  existing variable defined as that single character or defining exactly
//  that set (the first in definition order), or else by an inline set that
//  is exactly that class (which thus remains as written), or else by a new
//  variable named V_n after the first variable V containing it (or set_n if
//  no variable contains it), distinct from all other variable names,
//  including the new names of other groups.
struct CharacterClass {
    std::string name;       // the variable denoting the class (without '$'), or empty
    bool literal;           // denoted by the character itself (name is empty)
    bool existing;          // denoted by an existing variable
    bool inlineSet;         // denoted by an inline set as written (name is empty)
    std::string text;       // the inline set, if inlineSet
    re::RE * inlineRE;      // the inline set, if inlineSet
    re::CC * chars;
};

struct CharacterClassPartition {
    size_t firstRule;                       // the rules of the group, by index
    size_t lastRule;
    std::vector<CharacterClass> classes;    // in order of their first codepoints
    struct VariableSet {
        std::string name;
        re::CC * chars;                     // the codepoints of the variable
        std::vector<size_t> classes;        // the classes whose union it is
    };
    std::vector<VariableSet> variables;     // in definition order
    struct InlineSet {
        std::string text;                   // the first occurrence, in rule syntax
        re::RE * re;                        // the first occurrence
        re::CC * chars;
        std::vector<size_t> classes;        // the classes whose union it is
        unsigned occurrences;               // the number of occurrences of the set
    };
    std::vector<InlineSet> inlineSets;      // distinct sets, in order of first occurrence
};

std::vector<CharacterClassPartition> partitionCharacterClasses(const std::vector<Rule *> & rules);

//  Mutually exclusive partitioning.
//
//  Transform a rule set so that, within each group, all conversion rules are
//  expressed in terms of the mutually exclusive character classes of the
//  group (see partitionCharacterClasses):
//    - each set in a conversion rule (a variable defining a set, or a set
//      written inline) is replaced by the union of its classes, each denoted
//      by its character, variable or inline set; strings within the set and
//      the text boundary [$] are retained;
//    - the classes with new variable names are defined at the start of the
//      group;
//    - each variable that is not a set but uses sets (e.g. $s = $v+ x ;) is
//      replaced in the conversion rules of a group by a new variable (named
//      after it) with the sets of its definition replaced;
//    - within each group, the variable definitions of the group precede the
//      new definitions, which precede the conversion rules (in their original
//      order).
//  Filter and transform rules, and the original variable definitions, are
//  retained.
struct MutuallyExclusiveStats {
    unsigned groups = 0;
    unsigned setsRewritten = 0;      // set occurrences replaced by unions of classes
    unsigned classDefinitions = 0;   // new class variables defined
    unsigned variableCopies = 0;     // new variables for variables that are not sets
    unsigned mismatches = 0;         // replaced sets with different characters (an internal error)
};
std::vector<Rule *> MutuallyExclusivePartitioning(const std::vector<Rule *> & rules, MutuallyExclusiveStats * stats = nullptr);

//  Overlap analysis of conversion rules.
//
//  Two rules overlap if both may match at the same position of some text:
//  the text before the position may end with a match of each before context,
//  and the text after the position may begin with a match of each text to
//  replace followed by its after context.   At a position where rules
//  overlap, the earlier rule of a group takes precedence.   Rules match
//  against the source side (the left side of forward rules).
//
//  Patterns are analyzed as regular expressions with Unicode properties
//  resolved.  The analysis is conservative: as possessive quantification is
//  not modelled, rules may be reported as overlapping when they do not, but
//  rules reported as not overlapping never match at the same position.
class RuleOverlapAnalysis {
public:
    RuleOverlapAnalysis();
    ~RuleOverlapAnalysis();
    // May the rules both match at the same position of some text?
    bool mayOverlap(const ConversionRule * r1, const ConversionRule * r2);
    // May rule s match at a position strictly within the text matched by
    // the text to replace of rule r, when r matches?
    bool mayMatchWithin(const ConversionRule * s, const ConversionRule * r);
private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

//  The pairs of forward conversion rules of the same group (the rules
//  between transform rules) that may overlap, by index in the rule list.
struct RuleOverlap {
    size_t earlier;
    size_t later;
};
std::vector<RuleOverlap> findRuleOverlaps(const std::vector<Rule *> & rules);

//  Order disambiguation (single character contexts).
//
//  At a position where several conversion rules of a group match, the
//  earliest applies.  DisambiguateOrder rewrites a later rule L so that its
//  replacement rules match exactly where L matches and no earlier
//  overlapping rule E matches; the rules of the group may then be reordered
//  without changing their meaning, as far as the overlaps are resolved.
//
//  The case handled is that of L having a key and contexts of single
//  character items (characters or sets, possibly repeated: x?, x*, x+,
//  with variables that are not sets expanded; the key may have segments,
//  which the replacement rules retain), and each earlier
//  overlapping rule E being a sequence of items on one side of its position,
//  each a set (possibly with strings and the text boundary [$]), the anchor
//  ^ or $, or a repeated set (x?, x*, x+); segments are disregarded, and
//  variables that are not sets (e.g. $v = oa ;) are expanded into their
//  items, in L's key as well.  An E with items on both sides blocks L only
//  where both its following and preceding items match.  E and L may have
//  empty (or optional) texts to replace: an insertion rule E matching where
//  L matches inserts its result first, and L applies after the insertion
//  where E does not match again, as the replacement rules of L do:
//      E:  k c1 c2 ... cn   or   k } c1 ... cn → ...    (after the position)
//      E:  c1 ... cn { k → ...                          (before the position)
//  The earlier rules are explored together: L is replaced by rules for the
//  ways in which all of them fail to match (as ICU's possessive matching
//  does), each with L's key and contexts (restricted to classes of
//  characters) extended by a negated set (which also matches beyond the end
//  of the text) or ending with a set within L's items, e.g., for
//  E1 = k } s? t and E2 = k } u and L = [k x]:
//      x → result ;   k } [^s t u] → result ;   k } s [^t] → result ;
//  and, for E1 = ab and L = a, a } [^b] → result, and for E = a } b* c and
//  L = a, a } b* [^bc] → result; for E = c { a } b and L = a,
//  a } [^b] → result ; [^c] { a } b → result.  The text boundary is a class of its own
//  beyond the key (e.g. E = a } [b$] gives a } [^b$] → result).  Earlier
//  rules with items before the position contribute before contexts.  An L
//  masked by its earlier rules is removed.
//
//  Each item of L is replaced by the classes it matches on each path (and
//  the rest of its repetitions, if the path ends within it), e.g. for
//  E = ab and L = a+, aa a* → result ; a } [^ab] → result ;.
//
//  A later rule with sets with strings or the text boundary that is not
//  otherwise completely resolved is split into rules for the alternatives
//  of its sets (used only if these are all resolved), in ICU's order of
//  preference (the longest strings, the characters, then the boundary),
//  e.g. [{ch}{qu}ckq] → k ; into ch → k ; qu → k ; [ckq] → k ;, if no
//  alternative is a prefix (suffix, preceding the position) of another or
//  the set is the outermost item of its side (as ICU does not backtrack
//  into sets).
//
//  Repeated and optional items are matched possessively, as ICU does: an
//  item takes each character it can, e.g. E = a } [bc]? c matches exactly
//  a [bc] c, and E = a } b* b never matches.  The replacement rules are
//  verified with possessive matching where it differs from the regular
//  expression interpretation.  The text boundary may be the outermost item of each side
//  of L (e.g. [$] { or ^ at the start of its text to replace).
//
//  Not handled (the pairs remain in order): L with items that are not
//  (repeated) single characters, E with items that are not sets or repeated sets,
//  repetitions through several classes
//  (e.g. [{bc}]*), and sets with strings where ICU's matching differs from
//  the regular expression interpretation (strings that are prefixes of one
//  another or start with characters of the set, or repeated or optional
//  items overlapping the following items).
struct DisambiguationStats {
    size_t overlapsBefore = 0;       // overlapping pairs of rules
    size_t rulesSplit = 0;           // later rules replaced by rules for the alternatives of their sets
    size_t splitRules = 0;           // the rules for the alternatives
    size_t pairsResolved = 0;        // pairs (E, L) resolved
    size_t pairsUnresolved = 0;      // pairs (E, L) not handled
    size_t rulesReplaced = 0;        // rules L replaced
    size_t rulesAdded = 0;           // replacement rules
    size_t overlapsAfter = 0;        // overlapping pairs remaining (of the original rules, under ICU matching)
    size_t verificationFailures = 0; // replacement rules still overlapping a resolved E (an internal error)
    size_t deletionClosures = 0;     // before contexts extended by deleted characters (see below)
    std::vector<std::pair<const Rule *, const Rule *>> failedPairs;  // (E, replacement rule) failing verification
    std::map<std::string, size_t> unresolvedReasons;   // the unresolved pairs by reason
    //  The pairs (E, L) left to priority: an earlier rule E with blocking marks
    //  (see DeletedRunBlockMark) that L, at the same position, does not have
    //  where E is blocked, so that L applies after a deleted run there.  The
    //  pipeline applies L only where E does not match (at its first position).
    std::vector<std::pair<const Rule *, const Rule *>> prioritized;
};
//
//  Deletion closure.  Before contexts are matched against the converted text.
//  A rule L deleting the characters of a set D, preceded by an item A and a
//  repeated set S* (the last items of its before context, with no after
//  context), sees the converted text, from which the D characters following
//  A S* have already been deleted (by L itself).  Its context is extended to
//  A [S D]*, which matches the same converted texts, but also matches the
//  unconverted text, as the regular expression engine sees it: e.g.,
//      [:Greek:] [^[:ccc=0:][:ccc=230:]]* { [\u0300\u0301] → ;
//  becomes
//      [:Greek:] [[^[:ccc=0:][:ccc=230:]][\u0300\u0301]]* { [\u0300\u0301] → ;
//  The rewrite applies when no other rule of the group converts D characters
//  or may produce them (so that each D character following A S* in the text
//  is deleted by L), and A is disjoint from D (so that possessive matching of
//  [S D]* takes the character matching A only where that of S* does).  A and S may be
//  negated sets (including the text boundary): S, preceded by A, never matches
//  the boundary, and A may match it only at the start of the text.
std::vector<Rule *> DisambiguateOrder(const std::vector<Rule *> & rules, DisambiguationStats * stats = nullptr);

//  Trivial capture elimination.
//
//  A forward conversion rule whose text to replace is a capture and whose
//  completed result begins with a reference to that capture is transformed
//  by moving the text to replace into the before context and dropping the
//  reference from the completed result:
//      before { (X) } after → $1 rest | revisit ;
//  becomes
//      before X { } after → rest | revisit ;
//  The capture is retained in the before context if other references to it
//  remain in the result; otherwise it is dropped and the remaining captures
//  are renumbered.
//
//  The transformed rule matches at the end of X rather than its start, so
//  other rules may then apply at the positions of X.   A rule is therefore
//  transformed only if, within its group of conversion rules (the rules
//  between transform rules), no later rule may overlap it (match at the
//  start of X) and no other rule may match within X, as determined by
//  RuleOverlapAnalysis.
//
//  Rules other than forward conversion rules are unchanged.  This is
//  intended to be applied before NullableCaptureElimination.
struct TrivialCaptureStats {
    unsigned candidates = 0;             // rules of the trivial capture form
    unsigned blockedByLaterRule = 0;     // not transformed: a later rule may overlap the rule
    unsigned blockedWithinCapture = 0;   // not transformed (otherwise): a rule may match within X
    unsigned endPositionConflicts = 0;   // transformed, but an earlier rule may match at the end of X
    std::vector<const Rule *> transformed;
    struct Blocked {
        const Rule * rule;
        std::vector<const Rule *> laterRules;      // later rules that may match at the start of X
        std::vector<const Rule *> withinRules;     // rules that may match within X
    };
    std::vector<Blocked> blocked;
};
//  With allowEmptyText false, a rule whose text is only the capture (which would
//  become an insertion rule) is not transformed.  With allowFollowingText
//  false, a rule whose text continues after the capture is transformed only if
//  its text is (X) X+: the before context X of the transformed rule is matched
//  against the converted text, which differs from the text where the group's
//  conversions delete or change characters after X.
std::vector<Rule *> TrivialCaptureElimination(const std::vector<Rule *> & rules, TrivialCaptureStats * stats = nullptr,
                                              bool allowEmptyText = true, bool allowFollowingText = true);

//  Nullable capture elimination.
//
//  A nullable capture is a segment whose content consists of optional
//  items: repetitions with a lower bound of 0, such as x* or x?, or
//  variables defined as such repetitions.  The content is either a single
//  optional item, as in (x*), (x?) or ($v) with $v = [ab]*, or a sequence
//  of optional items, as in (x? y*).
//
//  Each forward conversion rule whose source side has a nullable capture
//  with k optional items is replaced by 2^k rules, one for each combination
//  of the items being present or absent, ordered from all items present to
//  none present, with the first item varying slowest.  For example, the
//  rules for (x? y*) have captures (x y+), (x), (y+) and none.   A present
//  item is given a lower bound of 1 (a variable is replaced by its
//  definition); an absent item is deleted, and references to captures
//  within it are replaced by the empty string.  If all items are absent,
//  the capture is deleted and its references replaced by the empty string.
//  If the capture is of a fixed string, e.g., (x) from (x?) or ($m $d)
//  from ($m? $d?) where $m and $d are defined as characters, the capture
//  and all references to it are replaced by that string (including any
//  variables defined as fixed strings).  Rules with present items
//  precede those with absent items, as nonempty matches are preferred.
//  Remaining captures are renumbered and their references updated.  Rules
//  with several nullable captures are split repeatedly.
//
//  Captures within a repetition, e.g. ((x*) y)*, are not eliminated, as
//  each repetition may match differently.  Rules other than forward
//  conversion rules are unchanged; apply ExtractForwardRules or
//  ExtractReverseBackwardRules first.
//
//  As quantifiers are possessive, the rules are equivalent to the original
//  except when the text following an optional item can only match after
//  zero repetitions, e.g., (a*) a → x ; never matches, but its replacement
//  a → x ; does.
std::vector<Rule *> NullableCaptureElimination(const std::vector<Rule *> & rules);

//  Unreferenced capture removal.
//
//  The captures of the source side of a forward conversion rule (in its
//  before context, text or after context) that the result side never
//  references, as $n or in the argument of a function call, are replaced by
//  their contents (re::removeUnneededCaptures); the remaining captures are
//  renumbered in order and their references updated.  For example,
//      ($wspace)+ ([\u102b-\u1032]) → $2 ;
//  becomes
//      $wspace+ ([\u102b-\u1032]) → $1 ;
//  Captures do not affect what a rule matches, so the rules are equivalent.
//  Rules other than forward conversion rules are unchanged.  This is
//  intended to be applied before TrivialCaptureElimination and
//  NullableCaptureElimination, which then see only the captures that matter.
std::vector<Rule *> RemoveUnreferencedCaptures(const std::vector<Rule *> & rules);

//  Fixed capture elimination.
//
//  A capture of a fixed string (a sequence of single characters, or variables
//  defined as such, not within a repetition), as the (̆) of
//      e { (̆) r → r $1 ;
//  is replaced by that string, and so are its references: e { ̆ r → r ̆ ;.
//  The capture matches only that string, so the rules are equivalent, and a
//  rule with no other captures may then be planned as a string rule.  Rules
//  other than forward conversion rules are unchanged.
std::vector<Rule *> FixedCaptureElimination(const std::vector<Rule *> & rules);

//  Expansion of finite texts.
//
//  A forward conversion rule whose text to replace has finitely many lengths
//  (optional items x?, bounded repetitions, and sets with strings such as
//  [{t͡ʃ}ʧ]) is replaced by rules with fixed-length texts, one for each way
//  the text matches.  ICU matches the text possessively: an optional item or
//  repetition takes what it can, and a set with strings takes its longest
//  string matching, without backtracking.  So each alternative excludes the
//  others: where an optional item is absent, the next character is not in its
//  set (x? y becomes x y, and y without the characters of x), and where a
//  shorter string of a set is taken, the text does not continue with a longer
//  one ([{ab}a] c becomes ab c, and a c without b).  What may not follow the
//  end of the text becomes a first item of the after context ([^x], which
//  includes the end of the text).  The alternatives are mutually exclusive, so
//  their order does not matter; they are given with the optional items present
//  and the longest strings first.
//
//  A rule is left unchanged (reported in the stats) if its text has unbounded
//  repetitions, the text boundary, or alternatives that are not single
//  characters or strings; if its result refers to captures; if a string of
//  two or more characters may not follow the text; if its after context does
//  not begin with a set where a character may not follow; or if it has more
//  than 64 alternatives.  Captures not referred to are dropped.
struct FiniteTextStats {
    unsigned candidates = 0;        // rules whose texts are not fixed-length
    unsigned expanded = 0;
    unsigned rules = 0;             // the rules replacing them
    std::vector<std::pair<const Rule *, std::string>> notExpanded;  // with the reason
};
std::vector<Rule *> ExpandFiniteTexts(const std::vector<Rule *> & rules, FiniteTextStats * stats = nullptr);

//  A forward conversion rule whose text begins with a run of at least n
//  characters of a set R, R{n,} T → C (written R ... R R* T or R ... R R+ T),
//  T a text of fixed length (with the captures the result may refer to),
//  without a before context or a cursor, as two rules in its place:
//  R } R{n,} T → ; deleting each character of the run but the last n where T
//  follows, then R{n} T → C, each with the rule's after context.  ICU applies
//  the rule at the first character of the run it reaches, which no rule
//  before it in its group may then begin with (nor, with n = 0, with a first
//  character of T, where the rule would have applied first); T may not begin
//  with a character of R.  (Zawgyi-my's ြ* ႊ → ွှ, ($wspace)+ (X) → $2.)
//  split counts the rules split.
std::vector<Rule *> SplitLeadingRuns(const std::vector<Rule *> & rules, unsigned * split = nullptr);

//  A forward conversion rule whose text has a run of a set R after a fixed
//  text X, X R+ T → C (T fixed, the result referring to captures of T only,
//  no cursor), in a group without before contexts and without a global
//  filter, with a marker M (a noncharacter, from U+FDEF down): X } R+ T → M
//  in its place, M { R } R* T → ; and M { T → C first in its group (they apply
//  only after M, the run then deleted), and M → ; in a group after it.  ICU
//  applies the rule at X, where the rules before it in its group are tried
//  first, as they are at X; the characters of the run and T, which ICU does
//  not reach, are converted after M, as nothing before them in the converted
//  text but M and the run deleted, and no before context sees M.
//  (Zawgyi-my's ေ ့+ ($consonant) → $1 ေ့.)  split counts the rules split.
std::vector<Rule *> SplitMiddleRuns(const std::vector<Rule *> & rules, unsigned * split = nullptr);

//  The anchors of the texts of forward conversion rules (as ICU matches them,
//  in the text being transliterated), moved to their contexts: ^ at the start
//  of a text without a before context becomes its before context, and $ at the
//  end of a text without an after context its after context.  A set including
//  the text boundary as the last of two or more items of a text without an
//  after context (as the parser resolves it to End, e.g. [^a] or [x$]), which
//  ICU matches as a character of the set or, matching nothing, at the end of
//  the text, becomes two rules: the text with the characters of the set, and
//  the text without it, with the after context $ (they cannot both match at a
//  position).  Likewise a capture of such a set, the last item of a text: the
//  text with the capture of the characters of the set (the references to it
//  rebound), and the text without it, with the after context $ and its
//  references (items of the result, not within function calls) removed, as
//  the capture is empty; or, if it is revisited unchanged (its only reference
//  the last item of the text to revisit), its after context, from which the
//  text is revisited alike.  With a global filter and transform rules, as ICU then applies
//  the rules to each run of the characters of the filter (of the text given),
//  such a set also matches nothing at the end of a run, which the text of a
//  rule set does not tell: it is then left as it is.  A text that is the
//  anchor alone is left unchanged.
std::vector<Rule *> MoveTextAnchors(const std::vector<Rule *> & rules);
//  In an after context, a position outside the global filter, as the end of a
//  run of the characters of the filter (a noncharacter, not in the text): with
//  a global filter and transform rules, the text of a rule matching nothing at
//  the end of the text matches also at the end of such a run (see
//  MoveTextAnchors); the pipeline gives its class the positions outside the
//  filter's mask, which it carries through the steps.
constexpr UCD::codepoint_t FilterRunEndMark = 0xFDD2;
//  Whether a set of a context marks the end of a filtered run: it contains
//  FilterRunEndMark, which is added only with the next noncharacter absent (a
//  set complemented, as [^c], contains both, and does not).
inline bool marksFilterRunEnd(const UCD::UnicodeSet & set) {
    return set.contains(FilterRunEndMark) && !set.contains(FilterRunEndMark + 1);
}
//  In a set of the text or the after context of a rule of the group after
//  prefix runs that only delete (see ComposeRevisits), a position that does
//  not follow a run they deleted (a noncharacter, not in the text): there ICU
//  would still have found the run, which blocks the match.  The pipeline
//  gives the class of such a set only its characters at the positions not
//  following a deleted run (as the prefix runs step marks them).  As with
//  FilterRunEndMark, the next noncharacter is absent from such a set.
constexpr UCD::codepoint_t DeletedRunBlockMark = 0xFDD4;
//  A mark run edit P R* T → C | $1 that cannot be moved to a group of its
//  own is left in its group as P } R* T → C M, M a noncharacter (one for each
//  R and T, from RunEditMark on), which a group of its own after it deletes
//  with T, M (R*) T → | $1 (see ComposeRevisits).
constexpr UCD::codepoint_t RunEditMark = 0xFDD8;
constexpr unsigned RunEditMarks = 8;
inline bool marksDeletedRunBlock(const UCD::UnicodeSet & set) {
    return set.contains(DeletedRunBlockMark) && !set.contains(DeletedRunBlockMark + 1);
}
//  The complement of such a set, which an analysis may form (as the context
//  [^r] that keeps a rule from applying where a marked r would follow), has
//  the next noncharacter and not DeletedRunBlockMark: it matches its
//  characters, and any character at the positions that follow a deleted run.
inline bool marksDeletedRunAllow(const UCD::UnicodeSet & set) {
    return set.contains(DeletedRunBlockMark + 1) && !set.contains(DeletedRunBlockMark);
}
inline UCD::UnicodeSet withDeletedRunBlock(const UCD::UnicodeSet & set) {
    UCD::UnicodeSet marked = set;
    marked.insert(DeletedRunBlockMark);
    return marked - UCD::UnicodeSet(DeletedRunBlockMark + 1);
}

//  The rules referring more than once to a capture of a character of a set
//  (([bcd]) ː → $1 $1), each the references items of its result, replaced by
//  a rule for each character of the set (bː → bb, cː → cc, …; for each
//  combination of the characters of such captures, at most 64), in order.
std::vector<Rule *> EnumerateRepeatedCaptures(const std::vector<Rule *> & rules);

//  Composition of revisits.
//
//  A forward conversion rule whose result has text to revisit,
//      before { X } after → A | B ;
//  places B back before the cursor: the rules of its group then apply to
//  B and the text following it, as to any text, with A (and the converted
//  text before X) as the text before the cursor.  Each such rule is replaced
//  by rules without a cursor that give the same results, found by applying
//  the rules of the group to B symbolically: the rules are tried in order at
//  the cursor, a rule matching where the characters of B are members of the
//  sets of its text and its contexts may match the known text around the
//  cursor.  A rule may match beyond B: its remaining items, Z, are appended
//  to the text of the composed rule.   Each complete sequence of rule
//  applications, ending when no text is left to revisit, gives a rule
//      before' { X Z } after' → A A2 ... ;
//  where before' combines the before context with the parts of the contexts
//  of the applied rules lying before X, and after' the after context with
//  those lying beyond X Z.   Where no rule applies, a character of B is
//  passed through.  The composed rules are ordered depth first, by the order
//  of the rules applied, ending with the rule passing all characters through,
//  so that the earliest applicable composed rule gives the result of the
//  original rules; a rule that applies whatever the unknown text is ends the
//  rules tried.  References in A and B to captures of single characters
//  are enumerated (with at most 64 characters each), as are characters of Z
//  referenced by the results of applied rules.  A text anchor (^ or $ in the
//  text of a rule) is matched as a context [$].
//
//  As in ICU, in a result with references to captures the cursor is placed
//  after the last reference preceding it, or at the start of the result (so
//  that X → A | $1 ; revisits A $1).
//
//  A rule whose text to revisit is the text matched by the end of its text,
//      before { P S } after → A | S ;
//  is instead rewritten as before { P } S after → A ; which leaves the same
//  text and cursor (and applies to runs, as kk → x | k ; on kkkk).  The rules
//  are composed with the rewritten rules.
//
//  A rule is left unchanged (reported in the stats) if the cursor is placed
//  beyond the result, a rule whose text is not a fixed-length sequence of
//  characters may apply to the text to revisit, results are not fixed
//  strings, contexts that cannot be combined (other than sequences of sets
//  with no repetitions) apply together, or the revisits do not end within
//  8 rule applications (as with ll → | l ;).
//
//  A prefix run: a rule whose text is a character of a set P followed by
//  characters of sets S = S1 ... Sm (m >= 1), and whose text to revisit is
//  its character of P,
//      (P) S1 ... Sm → A | $1 ;    or    p S1 ... Sm → A | p ;
//  (without contexts, A a fixed string; ICU also revisits A in the first form,
//  which must then not match).  On P S^n, ICU applies it n times, leaving
//  A^n P with P to revisit.  The prefix runs with the same P form a family,
//  whose texts S may follow each other in a run (P Sa Sb → Aa Ab P).  If the
//  other rules of its group cannot see the
//  difference (see prefixRunConflict in transform_rules_revisit.cpp), the
//  prefix runs of a group are moved to a group of their own, before the rest
//  of the group (separated by :: Null ;), where they are implemented together
//  as one step (TransformStep::prefixRuns).  The rules producing a P that ICU
//  then revisits, C → B | p ; (triggers), are applied before them, as
//  C → B p ; in a group of their own, where ICU reaches C (see
//  prefixRunConflict).  Otherwise they are composed as other rules are, or,
//  where that fails on their unbounded runs, with A empty and the other rules
//  matching P only as single characters, replaced by run rules
//  (before { p S+ → f(p) ; see prefixRunAsRun).  Where the prefix runs only
//  delete, the rest of the group may see the difference where ICU does not
//  reach a P: a run after a P within a match, or seen by an after context,
//  blocks the match (it-am's ($consonant) \' → | $1 ; makes ch'a not match
//  cha).  The rules of the rest of the group then have blocking marks (see
//  DeletedRunBlockMark), at the offsets of their matches at which such a run
//  may have been deleted (other than where ICU reaches P and deletes it).
//
//  A rule of a group inserting a fixed string between captures and
//  revisiting all, (T1) (T2+) (T3) → | $1 M $2 $3 ; is applied before the
//  group as an insertion rule, T1 { } T2+ T3 → M ; in a group of its own,
//  where the rest of the group cannot see the difference (see
//  insertionRevisit in transform_rules_revisit.cpp).
//
//  A chain revisit replaces the last character X of its text, after a
//  capture of a character of E, characters of fixed sets M1 ... Mk and a
//  repetition of a set A, by a character Y of A, and revisits all,
//      (E M1 ... Mk A*) X → | $1 Y ;
//  so that ICU applies it again on the same capture, extended by Y, to the
//  next X after A*.  The chain revisits of a group (with the same E, M, A
//  and Y) are applied before the rest of the group, in a group of their
//  own, where the rest of the group cannot see the difference (see
//  chainRevisitSplit in transform_rules_revisit.cpp); E then also has the
//  characters c that the group replaces by a character of E, revisited,
//  where followed by M1 (as c → | e ;).  In a group of their own, they
//  replace each character of X that follows E M1 ... Mk [A X]* (see
//  ChainRevisit).
//
//  A mark run edit replaces a fixed text P before a run of a set R (marks)
//  by C, deletes a fixed text T after the run, and revisits the run,
//      P ($R*) T → C | $1 ;    (or $R+)
//  as Latn-Arab's t ($notAbove+) ̈ → ة | $1 ; does.  The mark run edits of
//  a group are applied before the rest of the
//  group, in a group of their own, where the rest of the group cannot see
//  the difference (see markRunSplit in transform_rules_revisit.cpp); the
//  rules before them that ICU would apply instead at P are kept there as
//  rules leaving their text unchanged, (text) } after → $1 ;.
//
//  The rules of other kinds are unchanged; apply ExtractForwardRules or
//  ExtractReverseBackwardRules first.
//  With records, each rule with text to revisit is also described: how it was
//  handled, and the rules applying to its text to revisit (along any path of
//  the composition, until it was abandoned).
struct RevisitTrigger {
    const Rule * rule;              // as in the group (rewritten if a suffix revisit)
    unsigned depth;                 // its first application along a path (1: on the text to revisit)
    bool conditional = false;       // on characters beyond the text or contexts not implied
    bool extends = false;           // matched characters beyond the text to revisit
    bool revisits = false;          // its own result has text to revisit (originally)
    bool blocking = false;          // the rule composition was abandoned on
};
struct RevisitRecord {
    const Rule * rule;
    enum class Outcome {Suffix, Composed, NotComposed, PrefixRun, PrefixRunTrigger, ChainRevisit, MarkRunEdit, MarkRunLookahead} outcome;
    std::string reason;             // if not composed
    unsigned composedRules = 0;
    std::vector<const Rule *> composed;     // the rules replacing it
    unsigned paths = 0;             // complete paths (before deduplication)
    unsigned maxApplications = 0;   // on a complete path
    std::vector<RevisitTrigger> triggered;
};
struct RevisitStats {
    unsigned candidates = 0;        // rules with text to revisit
    unsigned composed = 0;          // replaced by composed rules
    unsigned composedRules = 0;     // the composed rules replacing them
    std::vector<std::pair<const Rule *, std::string>> notComposed;  // with the reason
    unsigned prefixRuns = 0;        // moved to a group of their own (with their triggers)
    unsigned chainRevisits = 0;     // moved to a group of their own
    unsigned markRunEdits = 0;      // moved to a group of their own
    unsigned markRunLookaheads = 0; // left with a lookahead and a marker (RunEditMark)
    unsigned foldedInsertions = 0;  // insertion rules folded into the rules before them
    unsigned blockingMarks = 0;     // groups after prefix runs given blocking marks
    bool keepRecords = false;
    std::vector<RevisitRecord> records;
};
std::vector<Rule *> ComposeRevisits(const std::vector<Rule *> & rules, RevisitStats * stats = nullptr);

//  The sets P and S and the string A of a prefix run (see ComposeRevisits).
//  A prefix run may also have a text P of several characters, p1 ... pk S →
//  | p1 ... pk ; (literal characters, A empty: ja_Latn-ru's tsu~ → | ts ;):
//  first is then the set of pk, and prefix those of p1 ... pk-1, which
//  precede it.  Such runs are applied after the others, in a group of their
//  own (where the characters of P stay where they are).
struct PrefixRun {
    UCD::UnicodeSet first;          // P (or its last character)
    std::vector<UCD::UnicodeSet> prefix;    // the characters of P before its last
    std::vector<UCD::UnicodeSet> run;   // S: the sets S1 ... Sm
    std::u32string replacement;     // A
    bool revisitsReplacement = false;   // (P) S → A | $1 ;  (ICU revisits A $1)
};
bool prefixRun(const ConversionRule * rule, PrefixRun & run);
//  Whether prefix runs may be applied together (see the definition): the empty
//  string if so, or the reason why not.
std::string prefixRunFamilies(const std::vector<PrefixRun> & runs);

//  The sets and the replacement of a chain revisit (see ComposeRevisits),
//  (E M1 ... Mk A*) X → | $1 Y ;  In a group of chain revisits alone (with
//  the same E, M, A and Y), each character of X that follows
//  E M1 ... Mk [A X]* becomes Y: ICU applies the rule at E, and again on
//  the capture extended by Y, through the run of A and X.  The rule must
//  be such that this holds: Y in A, X not in A, Y not in E or M, M not
//  meeting X, and, if E meets X (whose characters may be replaced before
//  they are reached as E), M within A.
struct ChainRevisit {
    UCD::UnicodeSet first;                  // E
    std::vector<UCD::UnicodeSet> fixed;     // M1 ... Mk
    UCD::UnicodeSet star;                   // A
    UCD::UnicodeSet last;                   // X
    char32_t replacement = 0;               // Y
};
bool chainRevisit(const ConversionRule * rule, ChainRevisit & chain);

//  The sets and the replacement of a mark run edit (see ComposeRevisits),
//  P ($R*) T → C | $1 ;  In a group of its own, with others, it applies at
//  each P followed by a run of R and T, where no rule
//  before it applies (with the same P, the first of them), leaving C, the
//  run and the text after T.  The rule must be such that this holds: C no
//  longer than P, the characters of P after the first within R (so that
//  the run is the same for the rules of a group), T and the first
//  character of P not in R, and the first character of P not in T or the
//  rest of P (no two applications overlap).
struct MarkRunEdit {
    std::vector<UCD::UnicodeSet> prefix;    // P
    UCD::UnicodeSet run;                    // R
    bool nonEmpty = false;                  // R+, or F R* (F within R)
    UCD::UnicodeSet firstRun;               // F (R for R+)
    std::vector<UCD::UnicodeSet> suffix;    // T
    std::u32string replacement;             // C
};
bool markRunEdit(const ConversionRule * rule, MarkRunEdit & edit);
//  A rule leaving its text unchanged, (text) } after → $1 ; with the items
//  of its text and after context (sets, in order), as the mark run edits
//  keep the rules that ICU would apply instead (see ComposeRevisits).
bool keptText(const ConversionRule * rule, std::vector<UCD::UnicodeSet> & items);

//  The groups of mark run edits and the rules kept before them (see
//  ComposeRevisits), set aside while the later passes rewrite the rules
//  (which would change their form), each with the number of transform
//  rules before it, and put back.
using MarkRunGroups = std::vector<std::pair<size_t, std::vector<Rule *>>>;
MarkRunGroups SetAsideMarkRunGroups(std::vector<Rule *> & rules);
void RestoreMarkRunGroups(std::vector<Rule *> & rules, const MarkRunGroups & groups);

//  Expansion of function calls on captures of single characters.
//
//  A forward conversion rule whose result calls a transform on a capture of
//  its text, &T($k), where the capture is a set of single characters S (not
//  within a repetition), is replaced by rules for the characters of S, grouped
//  by the value of T on them: for each group, the rule with the capture
//  restricted to the characters of the group and the call replaced by their
//  value, and for the characters that T leaves unchanged, the rule with the
//  call replaced by $k.  The groups being disjoint, the order of these rules
//  does not matter.  Further calls in the result are expanded in turn.
//
//  evaluate(T, S, groups, unchanged, reason) gives the groups of the
//  characters of S by the value of T on each (as a text of its own), and the
//  characters left unchanged; or false, with the reason, if T cannot be
//  evaluated on them.  A rule whose call cannot be expanded is left unchanged
//  (reported in the stats).
using FunctionEvaluator = std::function<bool (const TransformID & transform, const UCD::UnicodeSet & chars,
                                              std::vector<std::pair<UCD::UnicodeSet, std::u32string>> & groups,
                                              UCD::UnicodeSet & unchanged, std::string & reason)>;
struct FunctionCallStats {
    unsigned candidates = 0;        // rules with function calls on captures
    unsigned expanded = 0;
    unsigned rules = 0;             // the rules replacing them
    std::vector<std::pair<const Rule *, std::string>> notExpanded;  // with the reason
};
std::vector<Rule *> ExpandFunctionCalls(const std::vector<Rule *> & rules, const FunctionEvaluator & evaluate,
                                        FunctionCallStats * stats = nullptr);

//  The nullable captures of a side of a conversion rule, in order.  Unless
//  includeRepeated, captures within repetitions are excluded.
std::vector<re::Capture *> findNullableCaptures(const RuleSide * side, bool includeRepeated = false);

}

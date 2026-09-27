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
#include <vector>
#include <llvm/Support/Casting.h>
#include <allocator/threadsafe_slaballocator.h>
#include <re/adt/re_re.h>

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

// The text boundary, i.e., "$" within a UnicodeSet.  In a before context it
// matches the start of the text, in an after context the end of the text.
re::RE * makeTextBoundary();
bool isTextBoundary(const re::RE * re);
// Does a set (as returned by the parser) include the text boundary?
bool includesTextBoundary(const re::RE * set);

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
//  character items (characters or sets, with variables that are not sets
//  expanded), and each earlier
//  overlapping rule E being a sequence of items on one side of its position,
//  each a set (possibly with strings and the text boundary [$]), the anchor
//  ^ or $, or a repeated set (x?, x*, x+); segments are disregarded, and
//  variables that are not sets (e.g. $v = oa ;) are expanded into their
//  items, in L's key as well:
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
//  L = a, a } b* [^bc] → result.  The text boundary is a class of its own
//  beyond the key (e.g. E = a } [b$] gives a } [^b$] → result).  Earlier
//  rules with items before the position contribute before contexts.  An L
//  masked by its earlier rules is removed.
//
//  Not handled (the pairs remain in order): L with items that are not
//  single characters, E with items on both sides or with items
//  that are not sets or repeated sets, repetitions through several classes
//  (e.g. [{bc}]*), and cases where ICU's possessive matching differs from
//  the regular expression interpretation: repeated or optional items whose
//  sets intersect the sets of the following items, and sets with strings
//  that are prefixes of one another or start with characters of the set.
struct DisambiguationStats {
    size_t overlapsBefore = 0;       // overlapping pairs of rules
    size_t pairsResolved = 0;        // pairs (E, L) resolved
    size_t pairsUnresolved = 0;      // pairs (E, L) not handled
    size_t rulesReplaced = 0;        // rules L replaced
    size_t rulesAdded = 0;           // replacement rules
    size_t overlapsAfter = 0;        // overlapping pairs remaining
    size_t verificationFailures = 0; // replacement rules still overlapping a resolved E (an internal error)
    std::map<std::string, size_t> unresolvedReasons;   // the unresolved pairs by reason
};
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
std::vector<Rule *> TrivialCaptureElimination(const std::vector<Rule *> & rules, TrivialCaptureStats * stats = nullptr);

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

//  The nullable captures of a side of a conversion rule, in order.  Unless
//  includeRepeated, captures within repetitions are excluded.
std::vector<re::Capture *> findNullableCaptures(const RuleSide * side, bool includeRepeated = false);

}

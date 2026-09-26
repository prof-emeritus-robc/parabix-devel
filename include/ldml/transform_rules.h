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

#include <string>
#include <vector>
#include <llvm/Support/Casting.h>
#include <allocator/threadsafe_slaballocator.h>
#include <re/adt/re_re.h>

namespace re { class Name; }

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
//  transform applying in that direction.   Variable definitions are retained.
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

}

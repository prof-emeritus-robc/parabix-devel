/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Shape groups: a conversion group planned as search and replace passes of
//  the regular expression engine, one for each shape of the results of its
//  rules.
//
//  The rules (forward rules, after capture simplification and order
//  disambiguation) are grouped by the shape of their results: the sequence
//  of literal text, references to captures and function calls on captures,
//  as printed (the captures of a rule are numbered in their order, 1 to k).
//  The rules of a shape are matched together: the search pattern is the
//  alternation of their source sides (before context as a lookbehind, text,
//  after context as a lookahead), whose alternatives number their captures
//  alike (a branch reset group, (?|...)), and the replacement is the shape.
//
//  A function call &T($k) is deferred: the replacement writes a mark (a
//  noncharacter, one for each function of the group) before the reference,
//  and the marked characters are replaced by their values after the passes
//  (DeferredCall).  The text between the passes is the text the rules have
//  converted, but for the marked calls.
//
//  At a position of the text, ICU applies the first rule of the group that
//  matches there, and matches are applied from the left (a rule applies at
//  a position not covered by an applied match).  The rules are required to
//  match at different positions (no two of them may match at the same
//  position, as after DisambiguateOrder, see RuleOverlapAnalysis::mayOverlap),
//  so that their order does not matter, and the passes select the leftmost
//  matches:
//    - Within a shape, the engine selects the matches from the left.
//    - A shape X whose matches may begin within a match of another shape Y
//      (but not the converse) yields to Y: X is guarded by (?<!Y) at its end,
//      and applied before Y.  This requires that a match of Y containing the
//      start of a match of X ends where X ends: all the rules of X and Y end
//      with a character of a set D whose characters occur nowhere else in their
//      texts, and have no after contexts.
//    - A shape with before contexts is applied after the shapes without them
//      (a later pass), and its before contexts, which ICU matches against the
//      converted text, are matched against the text of the earlier passes
//      with the deferred calls still marked: a context of one set P becomes
//      P (?: M [E] | [E'])*, where E are the characters a deferred call with
//      the mark M deletes, and E' those that the shape itself deletes (by a
//      deferred call or an empty result), as a character deleted leaves the
//      context to the character before it.  This requires that no other
//      result (of a call, or of a rule of the shape) ends with a character of
//      P, nor is a mark one; that the shape's matches may not contain the
//      start of a match of an earlier pass; and that its texts may not begin
//      with a character that the earlier passes write unmarked (literal text,
//      or the characters of the captures they refer to).
//  At most one shape may have before contexts.
//
//  The rules of a group mapping a character of a set (without strings) to
//  literal text, without contexts, form one pass of their own, a character
//  map, whatever their results (a map, rather than a pass for each result).
//  As a character cannot contain the start of another match, the map never
//  takes precedence over another shape; where a match of the map may lie
//  within a match of another shape, the map is applied after that shape (not
//  guarded), which requires that it may not match the text that shape writes.

#pragma once

#include <map>
#include <string>
#include <vector>
#include <ldml/transform_rules.h>
#include <ucd/core/unicode_set.h>

namespace ldml {

// A function call deferred: written as its mark before the reference to its
// argument, a capture of single characters (arguments), and replaced by the
// values of the function after the passes (if evaluated).
struct DeferredCall {
    TransformID function;
    UCD::codepoint_t mark;
    UCD::UnicodeSet arguments;
    bool evaluated = false;
    std::map<UCD::codepoint_t, std::u32string> values;  // for each argument, if evaluated
};

// The first mark of deferred calls (and the number of marks: U+FDD9 to U+FDDF).
constexpr UCD::codepoint_t DeferredCallMark = 0xFDD9;
constexpr unsigned DeferredCallMarks = 7;

struct ShapePass {
    std::string shape;                          // the result shape, as printed
    std::vector<const ConversionRule *> rules;  // in order
    re::RE * search = nullptr;
    re::RE * replacement = nullptr;
    std::vector<size_t> guards;                 // the passes (later) it yields to
    bool contexts = false;                      // before contexts matched as in the converted text
    // A character map: the rules of the group replacing a character of a set
    // (without strings) by literal text, without contexts, applied together
    // as a mapping of characters (search: the alternation of their sets, for
    // the guards of other passes; no replacement).
    bool map = false;
};

struct ShapeGroupPlan {
    std::vector<ShapePass> passes;              // in the order to apply them
    std::vector<DeferredCall> calls;
    std::vector<std::string> problems;
    bool implementable() const {return problems.empty();}
};

// The plan of a conversion group (its conversion rules, in order).  The
// deferred calls are evaluated by evaluate, if given; the before contexts of
// a later pass need their values.
ShapeGroupPlan planShapeGroup(const std::vector<const ConversionRule *> & rules, const FunctionEvaluator * evaluate);

// The plans of the conversion groups of the rules (the maximal sequences of
// conversion rules and variable definitions, between other rules).
std::vector<ShapeGroupPlan> planShapeGroups(const std::vector<Rule *> & rules, const FunctionEvaluator * evaluate);

// A search pattern in the syntax of the regular expression parser (with
// branch reset groups for alternatives with captures, and the captures in
// their order), or "" (with the reason) if it cannot be written.
std::string regexSyntax(re::RE * re, std::string * reason = nullptr);
// A replacement (literal characters and references) in the syntax of
// re::parseReplacement, or "".
std::string replacementSyntax(re::RE * replacement);

}

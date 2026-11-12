#pragma once

namespace re {

class RE;

//  Which alternative a possessive repetition of alternatives (or of a set
//  with strings) takes at each repetition: the longest that matches, as ICU
//  matches a set with strings, or the first in order, as PCRE does.
enum class PossessiveChoice {Longest, First};

//
// Eliminate Possessive Quantifiers, transforming to equivalent
// repetitions using standard quantifiers.
RE * resolvePossessiveQuantifiers(RE * re, PossessiveChoice choice = PossessiveChoice::Longest);

//  A class of literal strings (an alternation of strings and sets of single
//  characters, with a string of several characters), made unambiguous: the
//  alternative chosen where several match (the longest, or the first) is the
//  only one that matches there, with negative lookaheads, w (?!t'), or
//  backward (for matching from the right, as ICU matches a before context),
//  with negative lookbehinds, (?<!t') w; or nullptr, if e is not such a class.
//  Forward, at most one alternative of the result begins at each position;
//  backward, at most one ends at each position.
RE * unambiguousAlternatives(RE * e, PossessiveChoice choice, bool backward);

}

#pragma once

#include <string>
#include <vector>
#include <ucd/core/unicode_set.h>

namespace cc { class Alphabet; }

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

//  The structure of unambiguousAlternatives: a class of literal strings e
//  (with sets of single characters) as the alternatives that remain, at most
//  one of which matches at each position (backward: ends there), the one the
//  choice prefers.  others: the single characters that begin (backward: end)
//  no string; literals: the strings (and single characters beginning strings)
//  kept, in order, each with its guards, the rests of the preferred strings it
//  begins (backward: their beginnings, before it): the literal matches only
//  where none of its guards follows (backward: precedes) it.  firsts: the
//  characters with which a match begins (backward: ends); singles: those at
//  which a match of one character always matches.  All in e's alphabet.
//  False if e is not such a class (or a set of single characters with more
//  than maxInvolved characters beginning strings).
struct UnambiguousStrings {
    const cc::Alphabet * alphabet = nullptr;
    UCD::UnicodeSet others;
    struct Literal {
        std::u32string text;
        std::vector<std::u32string> guards;
    };
    std::vector<Literal> literals;
    UCD::UnicodeSet firsts;
    UCD::UnicodeSet singles;
};
bool unambiguousStringClass(RE * e, PossessiveChoice choice, bool backward, UnambiguousStrings & out,
                            size_t maxInvolved = 64);

}

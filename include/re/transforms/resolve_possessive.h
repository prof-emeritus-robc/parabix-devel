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

}

#pragma once

#include <utility>
namespace re { class RE; class Name; class CC; class Capture; class Reference;}
namespace cc { class Alphabet;}

namespace re {

std::pair<int, int> getLengthRange(const RE * re, const cc::Alphabet * indexingAlphabet);

// Attempt to parse a regular expression into a prefix-suffix pair
// such that any match to the prefix cannot be matched at any
// other position within the RE.   If no such parse is found,
// return a pair consisting of the empty Sequence and the original RE.
std::pair<RE *, RE *> ParseUniquePrefix(RE * r);

unsigned maxLookaheadLength(const RE * re, const cc::Alphabet * lengthAlphabet);

int minMatchLength(const RE * re);

/* Validate that the given RE can be compiled in UTF-8 mode
   without variable advances. */
bool validateFixedUTF8(const RE * r);

bool hasReference(const RE * r);

bool hasPropertyReference(const RE * r);

    
    
bool byteTestsWithinLimit(RE * re, unsigned limit);

// Returns true if the given RE must match the
// end anchor "$" in all cases.
bool hasEndAnchor(const RE * r);
    
// Returns true if the given RE has at least one
// alternative requiring a match to the end anchor "$".
bool anyEndAnchor(const RE * r);
    
    
unsigned grepOffset(const RE * re);
}


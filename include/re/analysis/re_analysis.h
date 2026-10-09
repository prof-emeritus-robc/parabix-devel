#pragma once

#include <utility>
#include <vector>
namespace re { class RE; class Name; class CC; class Capture; class Reference; class Assertion;}
namespace cc { class Alphabet;}

namespace re {

std::pair<int, int> getLengthRange(const RE * re, const cc::Alphabet * indexingAlphabet);

// Does every string matched by re (an RE of UTF-8 code unit CCs) encode
// exactly one character?  getLengthRange counts such an RE in code units.
bool isUTF8EncodedCharacter(const RE * re);

// Attempt to parse a regular expression into a prefix-suffix pair
// such that any match to the prefix cannot be matched at any
// other position within the RE.   If no such parse is found,
// return a pair consisting of the empty Sequence and the original RE.
std::pair<RE *, RE *> ParseUniquePrefix(RE * r);

unsigned maxLookaheadLength(const RE * re, const cc::Alphabet * lengthAlphabet);

// The character class matched by r, which is a CC or a combination of
// character classes (Any, Alt, Diff, Intersect), or nullptr.
CC * resolveCharClass(RE * r);

// The class of characters that can begin a match of r (which matches at
// least one character), or nullptr if it cannot be determined.
CC * firstCharClass(RE * r);

// Parse r as an alternation of strings, each a sequence of items resolving to
// character classes, with at least one string of two or more characters.
bool parseStringClass(RE * r, std::vector<std::vector<CC *>> & strings);

// Can (s1|s2|...)* be matched as a run of the string class: from each
// starting position of a string, by MatchStar over the positions within
// occurrences of the strings (the fill), stopping just after the end of an
// occurrence?  This requires that (A) no proper suffix of any string matches
// a proper prefix of any string (itself included), and (B) no string matches
// strictly within another (other than as a prefix or suffix).
bool isRepeatableStringClass(const std::vector<std::vector<CC *>> & strings);

// A segment of a lookahead chain: X{lb,} for a character class X (star),
// a fixed-length RE of at least one character, or the end of the text (End,
// directly after a star).
struct LookaheadSegment {
    bool star;
    bool end;
    CC * cc;        // star: the class X
    RE * re;        // star: the repeated RE as written; fixed: the segment; end: End
    int lb;         // star: the minimum number of repetitions
    int length;     // fixed: the length (in the length alphabet)
    // A star of a string class (an alternation of strings, lb <= 1): the strings,
    // as sequences of character classes, and the class of their first characters
    // (cc is then the class of all their characters).
    std::vector<std::vector<CC *>> strings = {};
    CC * first = nullptr;
    // An end segment may also be final one-character lookaheads (?=Y) or
    // (?!Y): cc is then the class of the characters at which they all hold,
    // and negated is true if all are negative, so that they also hold at the
    // end of the text.
    bool negated = false;
    std::vector<Assertion *> assertions = {};   // the final lookaheads
};

// Parse a lookahead body as a chain of segments, e.g. B*C D{2,}E or B*C D*$,
// with at least one star segment and ending with a fixed segment (which ends
// with a character, or with End) or with the end of the text.  Each star class
// X must be disjoint from the characters that can begin the rest of the body,
// so that a run of X is always maximal: the rest must then match at the first
// position after it not in X.  The end of the text is the end of a match
// region (the RE compiler's region follow), where runs end in any case.
// The body may also end with one-character lookaheads (?=Y) or (?!Y), for
// character classes Y, holding where all of them hold: at the characters of
// Y or not of Y, and at the end of the text if all are negative.
// A star segment may also be (s1|s2|...){lb,} (lb <= 1) for a class of strings
// (sequences of character classes) satisfying isRepeatableStringClass, with
// no condition on the rest of the body; a star of a class that is not
// disjoint from the rest is such a star, of the one-character strings of
// the class (preceded by a fixed segment, for a lower bound above 1).
// With requireStar false, a chain need not have a star segment (for the
// coverage of match spans).
bool parseLookaheadChain(RE * body, const cc::Alphabet * lengthAlpha, std::vector<LookaheadSegment> & segments,
                         bool requireStar = true);

// Can the lookahead body be split by ParseUniquePrefix?  The unique prefix
// method is used for such a body, in preference to a lookahead chain.
bool hasUniquePrefix(RE * body);

// Does the RE contain a lookahead that is compiled as a lookahead chain
// (accepted by parseLookaheadChain, with no unique prefix)?
bool hasLookaheadChain(const RE * r);

int minMatchLength(const RE * re);

/* Validate that the given RE can be compiled in UTF-8 mode
   without variable advances. */
bool validateFixedUTF8(const RE * r);

bool hasReference(const RE * r);

// Does the RE contain a lookahead or lookbehind assertion?
bool hasAssertion(const RE * r);

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


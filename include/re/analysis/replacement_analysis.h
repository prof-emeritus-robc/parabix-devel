/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cc { class Alphabet; }

namespace re {

class RE;
class Capture;
class Reference;

//
// Analyses of the replacement pattern of a search and replace (see
// re::parseReplacement): a sequence of literal characters (CCs of one Unicode
// codepoint each) and references to the captures of the search pattern.  The
// search pattern given is the one whose captures the references refer to, by
// name: for lengths, the search pattern as prepared (its properties and
// names resolved), whose captures are new objects with the same names.
//

// The captures of an RE, by name.
std::map<std::string, Capture *> capturesByName(RE * re);

// The references of a replacement to captures that the search pattern does
// not have (a precondition of search and replace: there must be none).
std::vector<Reference *> unresolvedReferences(RE * replacement, RE * search);

// An item of a replacement: a literal string, or a reference to a capture.
struct ReplacementItem {
    std::u32string literal;             // if reference is nullptr
    Reference * reference = nullptr;
};

// The replacement as a sequence of items, with adjacent literal characters
// folded into strings, and each reference to a capture of the search pattern
// that matches one string only (characters, possibly repeated a fixed number of
// times) folded into that string.  Returns false if the replacement is not a
// sequence of literal characters and references.
bool foldReplacement(RE * replacement, RE * search, std::vector<ReplacementItem> & items);

// The range of the lengths of the replacement text, in characters (alphabet
// &cc::Unicode) or UTF-8 code units (&cc::UTF8), with each reference sized by
// its capture in the search pattern.  A fixed length gives a constant number
// of positions for every match.
std::pair<int, int> replacementLengthRange(RE * replacement, RE * search, const cc::Alphabet * alphabet);

// A literal string in UTF-8 code units.
std::string toUTF8String(const std::u32string & s);

}

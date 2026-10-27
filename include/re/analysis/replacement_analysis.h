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
class CC;
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
// sequence of literal characters and references.  Only a capture on every
// path of the search pattern is folded (another may be unset in a match).
bool foldReplacement(RE * replacement, RE * search, std::vector<ReplacementItem> & items);

// The range of the lengths of the replacement text, in characters (alphabet
// &cc::Unicode) or UTF-8 code units (&cc::UTF8), with each reference sized by
// its capture in the search pattern.  A fixed length gives a constant number
// of positions for every match.
std::pair<int, int> replacementLengthRange(RE * replacement, RE * search, const cc::Alphabet * alphabet);

// A literal string in UTF-8 code units.
std::string toUTF8String(const std::u32string & s);

//
// The boundaries of captures in the matches of a search pattern (U3, unique
// captures): each capture is on every path of the search pattern (not within
// a repetition, an alternation or an assertion), and each of its boundaries
// (its start, and its follow: the position after it) is found from the
// matches, in characters:
//   - forward from the start of the match, by a walk over the items before
//     the boundary, each of fixed length, or of variable length matching
//     characters of a class X only, whose runs are maximal (the characters
//     that can follow it within the match are not of X, or it is followed by
//     (?!Y) for Y including X): the walk goes through the run of X within the
//     match;
//   - or back from the follow of the match (the position after its last
//     character), when the items after the boundary have a fixed length;
//   - or, if backward walks are allowed, by a walk back from the last
//     character of the match, over the items after the boundary taken in
//     reverse, each of fixed length or a run of a class X that is maximal
//     backward (the characters that can precede it within the match are not
//     of X, or it is preceded by (?<!Y) for Y including X), when the items
//     before the boundary are not nullable.  The walk is a forward walk in
//     the reversed regions of the text (see RE_PipelineBuilder::
//     captureBoundaries): meeting the forward walk in the middle.
//
struct CaptureWalk {
    struct Step {
        int length = 0;             // advance by a fixed length, or
        CC * run = nullptr;         // through the run of a class within the match
    };
    struct Boundary {
        bool fromEnd = false;
        bool backward = false;
        unsigned at = 0;            // forward: after steps[0, at); fromEnd: the length back from the follow;
                                    // backward: after backSteps[0, at), back from the last character
    };
    struct CaptureBoundaries {
        Boundary start;
        Boundary follow;
        unsigned open = 0;          // the positions of the capture among the items of the
        unsigned close = 0;         // search pattern (for the order of captures)
        int prefixMinLength = 0;    // the minimum length of the match before the capture
    };
    std::vector<Step> steps;
    std::vector<Step> backSteps;                // from the end of the match, back
    std::vector<CaptureBoundaries> captures;    // as requested
    bool hasBackward() const {
        for (const CaptureBoundaries & c : captures) {
            if (c.start.backward || c.follow.backward) return true;
        }
        return false;
    }
};

// The walk for the captures named (by capture name, see capturesByName) in
// the search pattern as prepared.  Returns false if a capture has no unique
// boundaries found this way, with the construct and the reason.  Walks back
// from the end of the match are planned only if backward (they need the
// reversal of the regions of the text, see RE_ModeOptions::reversal).
bool planCaptureWalk(RE * search, const std::vector<std::string> & captures, CaptureWalk & walk,
                     RE *& construct, std::string & reason, bool backward = false);

}

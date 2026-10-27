/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once
#include <vector>
#include <cstdint>
#include <re/alphabet/alphabet.h>

namespace re {

class RE;

typedef std::vector<uint64_t> (*GrepLinesFunctionType)(re::RE *, const char * buf, size_t bufSize);

// The goal for which an RE is prepared: what the regular expression engine
// computes from its matches.  The passes applied to an RE depend on the goal:
// each preserves what its goals compute (see doc/re-engine-modes-plan.md).
//   MatchEnds:     the ends of the matches (the search mode);
//   MatchSpans:    the positions covered by nonempty matches;
//   MatchStarts:   the starts of the matches;
//   SearchReplace: the search pattern of a search and replace, whose captures
//                  referenced by the replacement must be kept.
enum class RE_Goal {MatchEnds, MatchSpans, MatchStarts, SearchReplace};

const char * goalName(RE_Goal goal);

// For the SearchReplace goal, replacement is the replacement pattern (nullptr
// if not known: all captures are then kept).
RE * resolveModesAndExternalSymbols(RE * r, GrepLinesFunctionType grep = nullptr,
                                    RE_Goal goal = RE_Goal::MatchEnds, RE * replacement = nullptr);

RE * regular_expression_passes(RE * r, const cc::Alphabet * lengthAlpha = &cc::Unicode,
                               RE_Goal goal = RE_Goal::MatchEnds);

}

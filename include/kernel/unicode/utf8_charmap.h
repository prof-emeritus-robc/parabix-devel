/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <string>
#include <utility>
#include <vector>
#include <ucd/core/unicode_set.h>

namespace kernel { class StreamSet; class PipelineBuilder; }

namespace kernel {

//
//  A character map applied to UTF-8 text in its basis bits (8 streams, one
//  position per code unit): each character of a set is replaced by a string
//  (of any length, empty to delete the character), without decoding the text
//  to codepoints.  The sets must be disjoint; the other characters are
//  copied.  The text must be well-formed (see WellFormedUTF8).
//
//  The characters mapped are marked at their first bytes (character classes
//  compiled on the UTF-8 basis by lookahead).  Each keeps, in place, as many
//  of its bytes as its replacement has (all of them, for a replacement of the
//  same length), their bits changed by XOR to those of the replacement; the
//  bytes of the character beyond are deleted (all of them, for an empty
//  replacement), or the bytes of the replacement beyond (its tail) inserted
//  after the character's last byte.  The distinct tails are numbered, and
//  only the basis bits, the lengths and the numbers of the tails are carried
//  through the deletions and insertions; the bytes of each tail are written
//  where it is inserted.  A map of characters of the same lengths as their
//  replacements takes two kernels: the classes and the
//  changes.
//
using UTF8_CharMap = std::vector<std::pair<UCD::UnicodeSet, std::u32string>>;

//  With Applies, the characters of contextual (characters of rules with
//  contexts) are mapped only where Applies marks their last bytes: the matches
//  of the rules with their contexts, as the regular expression engine marks
//  them on UTF-8 code units.
//  Within filters (Masks: the bytes of the characters of each filter in effect,
//  the innermost last), the characters are mapped only within the innermost,
//  and the masks are kept in step with the text: the bytes inserted after a
//  character have its mask.
StreamSet * UTF8_CharMapPipeline(PipelineBuilder & P, const UTF8_CharMap & map, StreamSet * Basis,
                                 const UCD::UnicodeSet & contextual = UCD::UnicodeSet(), StreamSet * Applies = nullptr,
                                 std::vector<StreamSet *> * Masks = nullptr);

}

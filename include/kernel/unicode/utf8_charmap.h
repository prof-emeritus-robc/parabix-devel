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
//  copied.  Unless the text is known to be well-formed (wellFormed: e.g. the
//  result of an earlier character map), the ill-formed code units are handled
//  as when UTF-8 is decoded (see codegen::UTF8Errors): each maximal ill-formed
//  subpart replaced by U+FFFD or deleted (with Abort, copied: the caller
//  checks the text).
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
//  replacements, on well-formed text, takes two kernels: the classes and the
//  changes.
//
using UTF8_CharMap = std::vector<std::pair<UCD::UnicodeSet, std::u32string>>;

StreamSet * UTF8_CharMapPipeline(PipelineBuilder & P, const UTF8_CharMap & map, StreamSet * Basis, bool wellFormed = false);

}

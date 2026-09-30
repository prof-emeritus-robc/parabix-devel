/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/core/streamset.h>
#include <string>

using StreamSet = kernel::StreamSet;
using PipelineBuilder = kernel::PipelineBuilder;

// Pre-tokenizer modes (mirrors HuggingFace pre-tokenizer types)
enum PreTokenizerMode {
    uax29,
    whitespace,
    whitespacesplit,
    digits,
    punctuation,
    simpleWordBoundaries,
    bytelevel,
    chardelimiter,
    bert,
    sequence_whitespace_punctuation
};

// Split behavior modes for delimiter handling
enum SplitBehaviorMode {
    removed,
    isolated,
    mergedwithprevious,
    mergedwithnext,
    contiguous
};

// Results returned from the boundary-computation stage.
// All StreamSets are in the original (un-spread) codepoint domain
// EXCEPT when PreTokenizer == bytelevel, where U21codepoints and
// WhitespaceMask have already been switched to the byte domain.
struct PreTokenizerResult {
    StreamSet * insertionBoundaries;   // cleaned boundary marks (after RemoveFirstMark)
    StreamSet * U21tokenBoundaries;    // raw word boundaries before behavior
    StreamSet * WhitespaceMask;        // whitespace positions (or zero mask for bytelevel)
    StreamSet * U21codepoints;         // codepoints (GPT2-mapped for bytelevel)
    StreamSet * AlphanumericMask;
    StreamSet * PunctuationStream;
    SplitBehaviorMode effectiveBehavior;
};

// Stage 2a: compute token boundaries and apply split-behavior rules.
// Inputs are already in the codepoint domain (post-normalization, post-decode).
// Returns a PreTokenizerResult ready for separator insertion.
PreTokenizerResult buildPreTokenizerBoundaries(
    PipelineBuilder & P,
    StreamSet * BasisBits,
    StreamSet * U21codepoints,
    PreTokenizerMode preTokenizer,
    SplitBehaviorMode splitBehavior,
    const std::string & delimiterString);

// Stage 2b: spread codepoints, insert LF separators, filter whitespace.
// Takes the PreTokenizerResult and returns the final U21 codepoint stream
// ready for UTF-8 re-encoding.
StreamSet * applyTokenSeparatorInsertion(
    PipelineBuilder & P,
    const PreTokenizerResult & result);

// ByteLevel encoding: maps every input byte to a unique printable Unicode codepoint
// using the GPT-2 byte alphabet. Returns a 21-bit U21 stream with one codepoint
// per input byte position (suitable for U21_to_UTF8).
StreamSet * applyByteLevelEncoding(PipelineBuilder & P, StreamSet * BasisBits);

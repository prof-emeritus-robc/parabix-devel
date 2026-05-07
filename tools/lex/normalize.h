/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/unicode/normalization.h>
#include <vector>

using StreamSet = kernel::StreamSet;
using PipelineBuilder = kernel::PipelineBuilder;

enum NormalizationMode {
    NormNone,
    NormNFC,           // Unicode NFC:  canonical decomposition + canonical composition.
    NormNFD,           // Unicode NFD:  canonical decomposition.
    NormNFKC,          // Unicode NFKC: compatibility decomposition + canonical composition.
    NormNFKD,          // Unicode NFKD: compatibility decomposition.
    NormByteLevel,     // GPT-2 byte alphabet: map every byte to a unique printable Unicode char.
    NormStripAccents,  // Remove all Mn (Mark, Nonspacing) codepoints.
                       // Should be preceded by NFD so accents are isolated.
    NormStripLeft,     // Remove leading Unicode White_Space characters.
    NormStripRight,    // Remove trailing Unicode White_Space characters.
    NormStrip,         // Remove both leading and trailing whitespace.
    NormLowercase,      // Map all uppercase codepoints to lowercase (SLC
    NormNmt,              // Google NMT preprocessing: control char cleanup + whitespace → space.
    NormBertCleanText,    // BERT _clean_text: drop \p{C}-{\t\n\r} + U+FFFD; \p{Zs}+\t\n\r → space.
    NormBertChineseChars  // BERT handle_chinese_chars: surround each CJK char with spaces.
};

// Apply a sequence of normalizations and return the result as UTF-8 BasisBits.
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               const std::vector<NormalizationMode> & modes);

// Same as applyNormalization but returns U21_focus (one codepoint per slot)
StreamSet * applyNormalizationU21(PipelineBuilder & P,
                                   StreamSet * BasisBits,
                                   const std::vector<NormalizationMode> & modes);

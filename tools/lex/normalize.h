/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/unicode/normalization.h>

using StreamSet = kernel::StreamSet;
using PipelineBuilder = kernel::PipelineBuilder;

enum NormalizationMode {
    NormNone,
    NormNFC,
    NormNFD,
    NormByteLevel,     // GPT-2 byte alphabet: map every byte to a unique printable Unicode char.
    NormStripAccents,  // Remove all Mn (Mark, Nonspacing) codepoints.
                       // Should be preceded by NFD so accents are isolated.
    NormStripLeft,     // Remove leading Unicode White_Space characters.
    NormStripRight,    // Remove trailing Unicode White_Space characters.
    NormStrip,         // Remove both leading and trailing whitespace.
    NormLowercase      // Map all uppercase codepoints to lowercase (SLC).
};

// Dispatches to the appropriate normalization step based on mode.
// Returns BasisBits unchanged if mode is NormNone.
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               NormalizationMode mode);

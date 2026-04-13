/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/unicode/normalization.h>

using StreamSet = kernel::StreamSet;
using PipelineBuilder = kernel::PipelineBuilder;

enum NormalizationMode { NormNone, NormNFC, NormNFD };

// Dispatches to the appropriate library normalization function based on mode.
// Returns BasisBits unchanged if mode is NormNone.
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               NormalizationMode mode);

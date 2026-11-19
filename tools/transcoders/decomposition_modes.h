/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/focused_transform.h>
#include <kernel/unicode/normalization/normalization.h>
#include <ucd/algo/normalization.h>

//  The masked and 21-bit modes of nfd and nfkd (-nfd-mask, -nfd-mask-select,
//  -nfd-u21), as nfc's -nfc-mask, -nfc-mask-select and -nfc-u21: the output
//  bytes of the decomposition of the source in the mode selected, or nullptr if
//  no mode option is given (the tool's own focused transform applies).
kernel::StreamSet * DecompositionModes(kernel::PipelineBuilder & P, NFD_PipelineBuilder & builder, UCD::DecompositionOptions form,
                                       const kernel::FocusOptions & options, kernel::StreamSet * Bytes, kernel::StreamSet * Basis);

llvm::cl::OptionCategory & DecompositionModeOptions();

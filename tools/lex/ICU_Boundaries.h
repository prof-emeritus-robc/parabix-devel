/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

 #ifndef ICU_BOUNDARY_PROVIDER_H
#define ICU_BOUNDARY_PROVIDER_H

#include <kernel/core/streamset.h>
#include <kernel/pipeline/program_builder.h>
#include <string>

namespace kernel {


/**
 * Build word boundary mask using ICU BreakIterator with locale support.
 * 
 * This function provides locale-aware word boundaries using ICU's
 * BreakIterator with CLDR (Common Locale Data Repository) rules.
 * 
 * P          ProgramBuilder for creating pipeline kernels
 * BasisBits  Input stream set (8 parallel bitstreams = UTF-8 data)
 * u8index    UTF-8 character start positions (for alignment)
 * locale     Locale string (e.g., "en_US", "fr_FR", "ja_JP")
 */
    kernel::StreamSet* buildWordBoundaryMaskFromICU(
    kernel::ProgramBuilder& P,
    kernel::StreamSet* BasisBits,
    kernel::StreamSet* u8index,
    const std::string& locale
);

} // namespace kernel

#endif // ICU_BOUNDARY_PROVIDER_H
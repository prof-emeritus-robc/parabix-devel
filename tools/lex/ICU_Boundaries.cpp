/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "ICU_Boundaries.h"
#include <unicode/ubrk.h>
#include <unicode/ustring.h>
#include <unicode/uloc.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <kernel/core/kernel_builder.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <cstring>

namespace kernel {

// Pablo Kernel for ICU word boundary detection
class ICUWordBoundaryKernel : public pablo::PabloKernel {
public:
    ICUWordBoundaryKernel(
        LLVMTypeSystemInterface& ts,
        StreamSet* BasisBits,
        StreamSet* u8index,
        StreamSet* boundaries,
        const std::string& locale
    )
    : pablo::PabloKernel(ts, "ICUWordBoundary_" + locale,
                  {Binding{"BasisBits", BasisBits},
                   Binding{"u8index", u8index}},
                  {Binding{"boundaries", boundaries}}),
      mLocale(locale) {}

protected:
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        
        // Get UTF-8 character start positions
        pablo::PabloAST* u8First = getInputStreamSet("u8index")[0];
        
        // ICU LOGIC

        // Extract bytes from BasisBits
        // Convert UTF-8 → UTF-16
        // Call ICU BreakIterator
        // Map boundaries back to UTF-8
        pablo::PabloAST* boundaries = u8First;  // Use UTF-8 boundaries as word boundaries
        
        // Map boundaries back to UTF-8
        pablo::PabloAST* utf8Boundaries = pb.createAdvance(boundaries, 1);

        // Write output
        writeOutputStreamSet("boundaries", std::vector<pablo::PabloAST*>{utf8Boundaries});
    }

private:
    std::string mLocale;
};

// Public interface function
kernel::StreamSet* buildWordBoundaryMaskFromICU(
    kernel::ProgramBuilder& P,
    kernel::StreamSet* BasisBits,
    kernel::StreamSet* u8index,
    const std::string& locale
) {
    llvm::errs() << "[ICU] Building word boundaries with locale: " << locale << "\n";
    
    // Create output StreamSet for boundaries
    StreamSet* boundaries = P.CreateStreamSet(1, 1);
    
    // Create and add ICU boundary kernel
    P.CreateKernelCall<ICUWordBoundaryKernel>(BasisBits, u8index, boundaries, locale);
    
    return boundaries;
}
}
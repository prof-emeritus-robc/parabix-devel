/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

 
/*
 * Simple ICU-based locale-aware word boundary detector.
 * This file provides a minimal runtime function `find_icu_word_boundaries`
 * that marks byte offsets in a UTF-8 buffer where ICU reports word boundaries.
*/

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <cctype>

#include <unicode/ubrk.h>  // ICU Break Iterator , BreakIterator API 
#include <unicode/utext.h> // UText API; used to wrap/operate on UTF‑8 data without converting to UTF‑16
#include <unicode/uclean.h> // cleanup utilities for ICU 
#include <iostream> // used for debug printing.


// a small runtime function find_icu_word_boundaries that:
// Uses ICU BreakIterator (word mode) to find word break positions for a UTF‑8 buffer.
// Marks the byte offsets that ICU considers word boundaries into a provided boundaryBits byte array (one byte per input byte).
// Is wired into Parabix as a kernel ICUWordBoundaryKernel that will be called from generated IR; there is also a builder buildWordBoundaryMaskFromICU used by the tokenizer.

// C linkage for the runtime function. Expose this definition with C linkage
// so generated LLVM IR can call it by name without C++ name mangling.
extern "C" size_t find_icu_word_boundaries(const uint8_t* utf8Text, int32_t utf8Length, const char* localeName, uint8_t* boundaryBits) {
    if (!utf8Text || utf8Length <= 0 || !boundaryBits) return 0;
    
    // return on invalid inputs
    UErrorCode status = U_ZERO_ERROR;

    // Wrap the UTF-8 buffer with a UText so ICU can operate directly on bytes.
    UText* ut = utext_openUTF8(NULL, reinterpret_cast<const char*>(utf8Text), utf8Length, &status);
    if (U_FAILURE(status) || !ut) return 0;  // if failed to create UText

    std::cerr << "Finding ICU word boundaries for locale: " << (localeName ? localeName : "(default)") << "\n";

    // Create a word break iterator for the requested locale.
    UBreakIterator* bi = ubrk_open(UBRK_WORD, localeName, NULL, 0, &status);
    if (U_FAILURE(status) || !bi) {
        
        std::cerr << "ubrk_open failed: " << u_errorName(status) << "\n";

        utext_close(ut);
        return 0;
    }

        ubrk_setUText(bi, ut, &status);
        if (U_FAILURE(status)) {

        std::cerr << "ubrk_setUText failed: " << u_errorName(status) << "\n";

        ubrk_close(bi);
        utext_close(ut);
        return 0;
    }

    // Zero output
    memset(boundaryBits, 0, (size_t)utf8Length);

    int32_t totalCount = 0;

    // Iterate boundaries. When operating on a UText backed by UTF-8,
    // the break positions are byte offsets into the UTF-8 buffer.
        int32_t pos = ubrk_first(bi);  // pos is a byte offset (index) reported by ICU for a word boundary.
        while (pos != UBRK_DONE) { // iterate through all boundaries, there are no more boundaries

            // Map rule statuses to readable names for debug output.
            // We'll only print statuses that correspond to real word
            // boundary categories (LETTER/NUMBER/KANA/IDEO).
            /*
              UBRK_WORD_NONE — candidate break but NOT a word boundary (ignore)
              UBRK_WORD_LETTER — boundary between alphabetic words (Latin, Cyrillic, etc.)
              UBRK_WORD_NUMBER — boundary that involves numbers/digits
              UBRK_WORD_KANA — boundary around Japanese kana sequences (hiragana/katakana)
              UBRK_WORD_IDEO — boundary involving ideographic characters (CJK Han)
            */
    
        // Advance iterator exactly once per loop and save the next position.
        int32_t next = ubrk_next(bi);  

        // Mark the byte offset reported by ICU as a word boundary.
        boundaryBits[pos] = 1;   // Mark the byte offset as a boundary
        ++totalCount;
        pos = next;
    }

    // Cleanup

    std::cerr << "ICU: total boundaries set in this call: " << totalCount << "\n";

    ubrk_close(bi);
    utext_close(ut);

    return totalCount;
}
    


#ifndef ICU_BOUNDARIES_TEST_MAIN
#include <kernel/core/kernel_builder.h>
#include <kernel/basis/p2s_kernel.h> 
#include <kernel/pipeline/program_builder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>

namespace kernel {

// BlockOriented Kernel for ICU word boundary detection
class ICUWordBoundaryKernel : public BlockOrientedKernel {
public:
    ICUWordBoundaryKernel(
        LLVMTypeSystemInterface& ts,
        StreamSet* ByteStream,
        StreamSet* boundaries,
        const std::string& locale
    )
    : BlockOrientedKernel(ts, "ICUWordBoundary_" + locale,
                  {Binding{"byteStream", ByteStream}},
                  {Binding{"boundaries", boundaries, BoundedRate(0, 1)}},
                  {}, {}, {}),
      mLocale(locale) {}

protected:
    void generateDoBlockMethod(KernelBuilder& b) override {
        // Get byte stream pointer (UTF-8 text)
        llvm::Value* byteStreamPtr = b.getInputStreamBlockPtr("byteStream", b.getInt32(0));
        
        // Get block size (number of bytes to process)
        // Note: getAvailableItemCount returns an i64 in the kernel IR; the
        // runtime `find_icu_word_boundaries` expects an i32 length parameter.
        // Keep the original i64 value for the memset call, but truncate to i32
        // when passing to the runtime to avoid an LLVM type-mismatch.
        llvm::Value* blockSize = b.getAvailableItemCount("byteStream");
        llvm::Value* blockSize32 = b.CreateTrunc(blockSize, b.getInt32Ty());
        
        // Get output boundary bitstream pointer
        llvm::Value* boundariesPtr = b.getOutputStreamBlockPtr("boundaries", b.getInt32(0));
        
        // generate the number of produced items/boundries 
        llvm::Value* producedItems = b.getProducedItemCount("boundaries");

        // Initialize output to zeros (no boundaries yet)
        b.CreateMemSet(boundariesPtr, b.getInt8(0), blockSize, 1);
        
        // Declare external function
        llvm::Module* module = b.getModule();
        llvm::FunctionType* funcType = llvm::FunctionType::get(
            b.getInt32Ty(), // return int32_t: number of boundaries set
            {b.getInt8PtrTy(), b.getInt32Ty(), b.getInt8PtrTy(), b.getInt8PtrTy()},
            false
        );
        // how many break ? 
        llvm::FunctionCallee icuFunc = module->getOrInsertFunction(
            "find_icu_word_boundaries", funcType);
        
        // Prepare locale string argument
        llvm::Value* localeStr = b.GetString(mLocale);
        
        // Call the runtime function; it returns number of boundaries set in this block
        // Call the runtime with the 32-bit block size
        llvm::Value* newCount = b.CreateCall(icuFunc, {byteStreamPtr, blockSize32, localeStr, boundariesPtr});

        // The runtime returns an i32; producedItems is an i64. Zero-extend the
        // returned count to i64 before adding to the produced item counter.
        llvm::Value* newCount64 = b.CreateZExt(newCount, b.getInt64Ty());
        b.setProducedItemCount("boundaries", b.CreateAdd(producedItems, newCount64));

    }

private:
    std::string mLocale;
};

// the builder function called by the tokenizer when a locale is specified
StreamSet* buildWordBoundaryMaskFromICU(
    ProgramBuilder& P,
    StreamSet* BasisBits,
    StreamSet* u8index,
    const std::string& locale
) {
    llvm::errs() << "[ICU] word boundaries with locale: " << locale << "\n";
    
    // Convert parallel bitstreams to sequential bytes
    StreamSet* ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(BasisBits, ByteStream);
    
    // Apply ICU boundary detection
    StreamSet* boundaries = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<ICUWordBoundaryKernel>(ByteStream, boundaries, locale);
    
    return boundaries;
}

} 
#endif


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

#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <unicode/uclean.h>

// C Linkage for the runtime function, make the runtime function available with unmangled linkage so generated LLVM IR can call it easily by name
extern "C" {
// Mark boundaryBits[offset] = 1 for byte offsets that are considered word boundaries.
void find_icu_word_boundaries(const uint8_t* utf8Text, int32_t utf8Length, const char* localeName, uint8_t* boundaryBits);
}

void find_icu_word_boundaries(const uint8_t* utf8Text, int32_t utf8Length, const char* localeName, uint8_t* boundaryBits) {
    if (!utf8Text || utf8Length <= 0 || !boundaryBits) return;

    UErrorCode status = U_ZERO_ERROR;

    // Wraping the UTF-8 buffer with a UText so ICU can operate directly on bytes.
    UText* ut = utext_openUTF8(NULL, reinterpret_cast<const char*>(utf8Text), utf8Length, &status);
    if (U_FAILURE(status) || !ut) return;

    // Create a word break iterator for the requested locale.
    UBreakIterator* bi = ubrk_open(UBRK_WORD, localeName, NULL, 0, &status);
    if (U_FAILURE(status) || !bi) {
        utext_close(ut);
        return;
    }

    ubrk_setUText(bi, ut, &status);
    if (U_FAILURE(status)) {
        ubrk_close(bi);
        utext_close(ut);
        return;
    }

    // Zero output
    memset(boundaryBits, 0, (size_t)utf8Length);

    // Iterate boundaries. When operating on a UText backed by UTF-8,
    // the break positions are byte offsets into the UTF-8 buffer.
    int32_t pos = ubrk_first(bi);
    while (pos != UBRK_DONE) {
        int32_t rule = ubrk_getRuleStatus(bi);

        // Accept statuses that generally represent words.??
        // UBRK_WORD_LETTER, UBRK_WORD_NUMBER, UBRK_WORD_KANA, UBRK_WORD_IDEO
        bool accept = true;
        }

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
                  {Binding{"boundaries", boundaries}},
                  {}, {}, {}),
      mLocale(locale) {}

protected:
    void generateDoBlockMethod(KernelBuilder& b) override {
        // Get byte stream pointer (UTF-8 text)
        llvm::Value* byteStreamPtr = b.getInputStreamBlockPtr("byteStream", b.getInt32(0));
        
        // Get block size (number of bytes to process)
        llvm::Value* blockSize = b.getAvailableItemCount("byteStream");
        
        // Get output boundary bitstream pointer
        llvm::Value* boundariesPtr = b.getOutputStreamBlockPtr("boundaries", b.getInt32(0));
        
        // Initialize output to zeros (no boundaries yet)
        b.CreateMemSet(boundariesPtr, b.getInt8(0), blockSize, 1);
        
        // Declare external function
        llvm::Module* module = b.getModule();
        llvm::FunctionType* funcType = llvm::FunctionType::get(
            b.getVoidTy(),
            {b.getInt8PtrTy(), b.getInt32Ty(), b.getInt8PtrTy(), b.getInt8PtrTy()},
            false
        );
        
        llvm::FunctionCallee icuFunc = module->getOrInsertFunction(
            "find_icu_word_boundaries", funcType);
        
        // Prepare locale string argument
        llvm::Value* localeStr = b.GetString(mLocale);
        
        // Call the runtime function
        b.CreateCall(icuFunc, {byteStreamPtr, blockSize, localeStr, boundariesPtr});
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
    llvm::errs() << "[ICU] Building word boundaries with locale: " << locale << "\n";
    
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

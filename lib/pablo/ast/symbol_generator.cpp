/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <pablo/ast/symbol_generator.h>

#include <pablo/ast/pe_string.h>
#include <pablo/ast/pe_integer.h>
#include <idisa/idisa_builder.h>

namespace pablo {

String * SymbolGenerator::makeString(llvm::LLVMContext &ctx, const llvm::StringRef prefix) noexcept {
    auto f = mPrefixMap.find(prefix);
    if (f == mPrefixMap.end()) {
        char * const data = ThreadSafeSlabAllocator::allocate_array_of<char>(prefix.size() + 1);
        std::memcpy(data, prefix.data(), prefix.size());
        data[prefix.size()] = '\0';
        llvm::StringRef name(data, prefix.size());
        mPrefixMap.insert(std::make_pair(name, 1));
        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
        llvm::PointerType * const ptrTy = llvm::PointerType::getUnqual(ctx);
        #else
        llvm::PointerType * const ptrTy = llvm::IntegerType::getInt8PtrTy(ctx);
        #endif
        return new String(ptrTy, name);
    } else { // this string already exists; make a new string using the given prefix

        // TODO: check FormatInt from "https://github.com/fmtlib/fmt/blob/master/fmt/format.h" for faster integer conversion

        size_t count = f->second++;
        size_t length = prefix.size() + 2;
        size_t digits = 10;
        while (LLVM_UNLIKELY(digits <= count)) {
            digits *= 10;
            length += 1;
        }

        llvm::SmallVector<char, 256> name(length);
        std::memcpy(name.data(), prefix.data(), prefix.size());
        char * p = name.data() + length - 1;
        while (count) {
            *p-- = (count % 10) + '0';
            count /= 10;
        }
        *p = '_';
        return makeString(ctx, llvm::StringRef(name.data(), length));
    }
}

Integer * SymbolGenerator::getInteger(llvm::LLVMContext & ctx, const IntTy value, unsigned intWidth) noexcept {
    auto key = std::make_pair(value, intWidth);
    auto f = mIntegerMap.find(key);
    Integer * result;
    if (f == mIntegerMap.end()) {
        result = new Integer(value, llvm::IntegerType::getIntNTy(ctx, intWidth));
        assert (result->value() == value);
        mIntegerMap.emplace(key, result);
    } else {
        result = f->second;
    }
    return result;
}

}

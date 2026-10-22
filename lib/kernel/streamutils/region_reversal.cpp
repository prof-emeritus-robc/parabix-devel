/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/region_reversal.h>

#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <cstring>
#include <string>
#include <vector>

using namespace llvm;

namespace kernel {

namespace {

// The words of the part of a region kept in the state, for each stream.
unsigned pendingWords(unsigned limit) {
    return (limit + 63) / 64;
}

// The bits of a stream set of `streams` streams, from the block of position 0
// (the stream sets of a kernel's inputs and outputs are blocks of BlockWidth
// positions of each stream in turn).
struct StreamWords {
    uint64_t * words;
    uint64_t streams;
    uint64_t wordsPerBlock;     // of one stream
    uint64_t * word(uint64_t j, uint64_t r) const {
        const uint64_t blockBits = wordsPerBlock * 64;
        return words + (r / blockBits) * streams * wordsPerBlock + j * wordsPerBlock + (r % blockBits) / 64;
    }
    bool bit(uint64_t j, uint64_t r) const {
        return (*word(j, r) >> (r % 64)) & 1;
    }
    void set(uint64_t j, uint64_t r) const {
        *word(j, r) |= uint64_t(1) << (r % 64);
    }
};

}

// The regions of the items [0, items) of an invocation (relative to its
// processed position base), reversed into reversed: the region open at its
// start began m = *pendingLength positions before it, and its bits are in
// pending; positions [items, items + ahead) are available beyond (none on the
// final invocation, whose items end the text).  The region open at the end
// of the items, if any, is kept in pending for the next invocation.
extern "C" void parabix_reverse_regions(uint64_t * source, uint64_t * barriers, uint64_t * reversed, uint64_t * pending,
                                        uint64_t * pendingLength, uint64_t streams, uint64_t blockWidth, uint64_t items,
                                        uint64_t isFinal, uint64_t limit, uint64_t base) {
    const uint64_t wordsPerBlock = blockWidth / 64;
    const StreamWords src{source, streams, wordsPerBlock};
    const StreamWords bar{barriers, 1, wordsPerBlock};
    const StreamWords out{reversed, streams, wordsPerBlock};
    const uint64_t W = pendingWords(static_cast<unsigned>(limit));
    const int64_t m = static_cast<int64_t>(*pendingLength);
    const int64_t n = static_cast<int64_t>(items);
    // The end of the positions available (relative to base).
    const int64_t end = isFinal ? n : n + static_cast<int64_t>(limit);
    auto bit = [&](uint64_t j, int64_t r) -> bool {
        if (r < 0) {
            const uint64_t i = static_cast<uint64_t>(r + m);
            return (pending[j * W + i / 64] >> (i % 64)) & 1;
        }
        return src.bit(j, static_cast<uint64_t>(r));
    };
    auto isBarrier = [&](int64_t r) -> bool {
        return (r >= 0) && bar.bit(0, static_cast<uint64_t>(r));
    };
    std::memset(reversed, 0, ((items + blockWidth - 1) / blockWidth) * streams * (blockWidth / 8));
    int64_t open = -1, openFollow = 0;     // the region open at the end of the items
    for (int64_t cur = -m; cur < n; ) {
        if (isBarrier(cur)) {
            for (uint64_t j = 0; j < streams; j++) {
                if (bit(j, cur)) out.set(j, static_cast<uint64_t>(cur));
            }
            cur++;
            continue;
        }
        const int64_t s = cur;
        int64_t f = s;
        while ((f < end) && !isBarrier(f)) {
            f++;
            if (static_cast<uint64_t>(f - s) > limit) {
                report_fatal_error(StringRef("RegionReversal: a region (between barriers) of more than ") +
                                   std::to_string(limit) + " positions, from position " +
                                   std::to_string(static_cast<int64_t>(base) + s) +
                                   " (see --reverse-region-limit)");
            }
        }
        if ((f == end) && !isFinal) {
            report_fatal_error("RegionReversal: a region beyond the lookahead (see --reverse-region-limit)");
        }
        for (int64_t i = std::max<int64_t>(s, 0); i < std::min(f, n); i++) {
            for (uint64_t j = 0; j < streams; j++) {
                if (bit(j, s + f - 1 - i)) out.set(j, static_cast<uint64_t>(i));
            }
        }
        if (f > n) {
            open = s;
            openFollow = f;
        }
        cur = f;
    }
    // The region open at the end of the items: [open, n), for the next invocation.
    uint64_t kept = 0;
    if (!isFinal && (open >= -m) && (open < n) && (openFollow > n)) {
        kept = static_cast<uint64_t>(n - open);
        std::vector<uint64_t> next(streams * W, 0);
        for (uint64_t j = 0; j < streams; j++) {
            for (uint64_t i = 0; i < kept; i++) {
                if (bit(j, open + static_cast<int64_t>(i))) next[j * W + i / 64] |= uint64_t(1) << (i % 64);
            }
        }
        std::memcpy(pending, next.data(), next.size() * sizeof(uint64_t));
    }
    *pendingLength = kept;
}

RegionReversal::RegionReversal(LLVMTypeSystemInterface & ts, StreamSet * Barriers, StreamSet * Source,
                               StreamSet * Reversed, unsigned limit)
: MultiBlockKernel(ts, "RegionReversal" + std::to_string(Source->getNumElements()) + "_" + std::to_string(limit),
                   {Binding{"Barriers", Barriers, FixedRate(1), LookAhead(limit)},
                    Binding{"Source", Source, FixedRate(1), LookAhead(limit)}},
                   {Binding{"Reversed", Reversed}}, {}, {},
                   {InternalScalar{ArrayType::get(ts.getInt64Ty(), Source->getNumElements() * pendingWords(limit)), "pending"},
                    InternalScalar{ts.getInt64Ty(), "pendingLength"}}),
  mStreams(Source->getNumElements()), mLimit(limit) {
    if (limit == 0) report_fatal_error("RegionReversal: the region limit must be positive");
    if (Reversed->getNumElements() != mStreams) report_fatal_error("RegionReversal: the result must have the streams of the source");
    setStride(ts.getBitBlockWidth());
}

void RegionReversal::linkExternalMethods(KernelBuilder & b) {
    b.LinkFunction("parabix_reverse_regions", parabix_reverse_regions);
}

void RegionReversal::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    Type * const i64Ty = b.getInt64Ty();
    auto toI64 = [&](Value * v) {return b.CreateZExtOrTrunc(v, i64Ty);};
    Value * const strideItems = toI64(b.CreateMul(numOfStrides, b.getSize(getStride())));
    Value * const accessible = toI64(b.getAccessibleItemCount("Source"));
    Value * const isFinal = b.isFinal();
    Value * const items = b.CreateSelect(isFinal, b.CreateUMin(accessible, strideItems), strideItems);
    Function * const reverse = b.getModule()->getFunction("parabix_reverse_regions");
    b.CreateCall(reverse->getFunctionType(), reverse,
                 {b.getInputStreamBlockPtr("Source", b.getInt32(0)),
                  b.getInputStreamBlockPtr("Barriers", b.getInt32(0)),
                  b.getOutputStreamBlockPtr("Reversed", b.getInt32(0)),
                  b.getScalarFieldPtr("pending").first,
                  b.getScalarFieldPtr("pendingLength").first,
                  b.getInt64(mStreams), b.getInt64(b.getBitBlockWidth()), items,
                  toI64(isFinal), b.getInt64(mLimit), toI64(b.getProcessedItemCount("Source"))});
}

}

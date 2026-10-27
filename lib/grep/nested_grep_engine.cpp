#include <grep/nested_grep_engine.h>
#include <grep/grep_kernel.h>
#include <kernel/bitwise/bixlogic.h>
#include <re/unicode/regex_passes.h>
#include <re/unicode/casing.h>
#include <re/unicode/re_name_resolve.h>
#include <kernel/io/source_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <re/cc/cc_kernel.h>
#include <kernel/scan/scanmatchgen.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/Support/raw_ostream.h>
#include <grep/grep_toolchain.h>
#include <kernel/unicode/utf8_support.h>

#include <re/printer/re_printer.h>

using namespace kernel;
using namespace llvm;

namespace grep {

class CopyBreaksToMatches final : public MultiBlockKernel {
public:

    CopyBreaksToMatches(LLVMTypeSystemInterface & ts,
               StreamSet * const BasisBits,
               StreamSet * const u8index,
               StreamSet * const breaks,
               StreamSet * const matches)
    : MultiBlockKernel(ts
                       , "gitignoreC"
                       // inputs
                       , {{"BasisBits", BasisBits}, {"u8index", u8index}, {"breaks", breaks}}
                       // outputs
                       , {{"matches", matches, FixedRate(), Add1()}}
                       // scalars
                       , {}, {}, {}) {

    }

protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        Value * const processed = b.getProcessedItemCount("breaks");
        Value * const source = b.getRawInputPointer("breaks", processed);
        Value * const produced = b.getProducedItemCount("matches");
        Value * const target = b.getRawOutputPointer("matches", produced);
        Value * const toCopy = b.CreateMul(numOfStrides, b.getSize(getStride()));
        b.CreateMemCpy(target, source, toCopy, b.getBitBlockWidth() / 8);
    }

};

NestedInternalSearchEngine::NestedInternalSearchEngine(BaseDriver & driver)
: mGrepRecordBreak(GrepRecordBreakKind::LF)
, mGrepDriver(driver)
, mNested(1, nullptr)
, mIsFilter(1, false) {


}

void NestedInternalSearchEngine::push(const re::PatternVector & patterns, bool filter) {
    // If we have no patterns and this is the "root" pattern,
    // we'll still need an empty gitignore kernel even if it
    // just returns the record break stream for input.
    // Otherwise just reuse the parent kernel.

    const auto preserve = mGrepDriver.getPreservesKernels();
    mGrepDriver.setPreserveKernels(true);

    auto P = CreatePipeline(mGrepDriver, Input<const char *>{"buffer"}, Input<size_t>{"length"}, Input<MatchAccumulator &>{"accumulator"} );

    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");
    Scalar * const accumulator = P.getInputScalar("accumulator");

    StreamSet * const byteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<MemorySourceKernel>(buffer, length, byteStream);
    StreamSet * const basisBits = P.CreateStreamSet(8);
    P.CreateKernelCall<S2PKernel>(byteStream, basisBits);
    StreamSet * const breaks = P.CreateStreamSet();

    re::CC * breakCC = nullptr;

    if (mGrepRecordBreak == GrepRecordBreakKind::Null) {
        breakCC = re::makeCC(0x0, &cc::UTF8);
    } else {// if (mGrepRecordBreak == GrepRecordBreakKind::LF)
        breakCC = re::makeCC(0x0A, &cc::UTF8);
    }
    P.CreateKernelCall<CharacterClassKernelBuilder>(std::vector<re::CC *>{breakCC}, basisBits, breaks);
    StreamSet * const U8index = P.CreateStreamSet();
    P.CreateKernelCall<UTF8_index>(basisBits, U8index);

    StreamSet * const matches = P.CreateStreamSet();

    assert (mNested.size() > 0 && mNested[0] == nullptr);
    assert (mNested.size() == 1 || mNested[1] != nullptr);
    assert (mIsFilter.size() == mNested.size());

    Kernel * kernel = nullptr;

    // The level whose selection this level modifies: the nearest enclosing
    // level that is not a filter (none, for a filter level).
    Kernel * enclosing = nullptr;
    if (!filter) {
        for (size_t k = mNested.size() - 1; k > 0; --k) {
            if (!mIsFilter[k]) {
                enclosing = mNested[k];
                break;
            }
        }
    }

    if (LLVM_UNLIKELY(patterns.empty())) {

        if (LLVM_LIKELY(enclosing != nullptr)) {
            // Reuse the enclosing level's kernel, bound to this pipeline's streams.
            kernel = enclosing;
            assert (kernel->getNumOfStreamInputs() == 3);
            kernel->setInputStreamSetAt(0, basisBits);
            kernel->setInputStreamSetAt(1, U8index);
            kernel->setInputStreamSetAt(2, breaks);
            kernel->setOutputStreamSetAt(0, matches);
        } else {
            kernel = new CopyBreaksToMatches(P.getTypeSystem(),
                                             basisBits, U8index, breaks,
                                             matches);
        }

    } else {

        auto E = CreatePipeline(mGrepDriver,
            Input<streamset_t>{"basis", basisBits},
                                Input<streamset_t>{"u8index", U8index},
                                Input<streamset_t>{"breaks", breaks},
            Output<streamset_t>{"matches", matches, Add1(), ManagedBuffer()},
            InternallySynchronized());

        E.setStride(E.getTypeSystem().getBitBlockWidth());

        const auto n = patterns.size();
        assert (n > 0);

        Kernel * const outerKernel = enclosing;
        StreamSet * resultSoFar = breaks;
        if (outerKernel) {
            Kernel * const chained = E.AddKernelFamilyCall(outerKernel);
            assert (chained->getNumOfStreamInputs() == 3);
            chained->setInputStreamSetAt(0, basisBits);
            chained->setInputStreamSetAt(1, U8index);
            chained->setInputStreamSetAt(2, breaks);
            assert (chained->getNumOfStreamOutputs() > 0);
            resultSoFar = chained->getOutputStreamSet(0); assert (resultSoFar);
        }

        StreamSet * matchStarts = E.CreateStreamSet(1, 1);
        E.CreateKernelCall<LineStartsKernel>(breaks, matchStarts);

        for (unsigned i = 0; i != n; ++i) {
            StreamSet * MatchResults = nullptr;
            if (LLVM_UNLIKELY(i == (n - 1UL))) {
                assert (E.getNumOfStreamOutputs() > 0);
                MatchResults = E.getOutputStreamSet(0);
            } else {
                MatchResults = E.CreateStreamSet();
            }
            // check if we need to combine the current result with the new set of matches
            // (a filter level starts from no records, others from all records)
            const bool exclude = (patterns[i].first == re::PatternKind::Exclude);
            if (i || outerKernel || exclude || !filter) {
                StreamSet * const matchedRecords = E.CreateStreamSet();
                matchingRecords(E, patterns[i].second, basisBits, U8index, breaks, matchStarts, matchedRecords);
                if (exclude) {
                    StreamSet * const unmatchedRecords = E.CreateStreamSet();
                    E.CreateKernelCall<InvertMatchesKernel>(matchedRecords, breaks, unmatchedRecords);
                    AndCombine(E, resultSoFar, unmatchedRecords, MatchResults);
                } else {
                    OrCombine(E, resultSoFar, matchedRecords, MatchResults);
                }
            } else {
                matchingRecords(E, patterns[i].second, basisBits, U8index, breaks, matchStarts, MatchResults);
            }
            resultSoFar = MatchResults;

        }
        assert (resultSoFar == E.getOutputStreamSet(0));

        // The default signature identifies each kernel of the nested pipeline
        // (family calls by family name, others, including the RE kernels, by
        // signature), so nested pipelines that differ never share an object
        // cache entry.
        kernel = E.makeKernel();
    }

    P.AddKernelFamilyCall(kernel);

    // Restrict the selection to the records selected by the filter levels.
    StreamSet * selected = matches;
    for (size_t k = 1; k < mNested.size(); ++k) {
        if (!mIsFilter[k]) continue;
        Kernel * const f = mNested[k];
        assert (f->getNumOfStreamInputs() == 3);
        StreamSet * const filtered = P.CreateStreamSet();
        f->setInputStreamSetAt(0, basisBits);
        f->setInputStreamSetAt(1, U8index);
        f->setInputStreamSetAt(2, breaks);
        f->setOutputStreamSetAt(0, filtered);
        P.AddKernelFamilyCall(f);
        StreamSet * const combined = P.CreateStreamSet();
        AndCombine(P, selected, filtered, combined);
        selected = combined;
    }

    if (MatchCoordinateBlocks > 0) {
        StreamSet * const MatchCoords = P.CreateStreamSet(3, sizeof(size_t) * 8);
        P.CreateKernelCall<MatchCoordinatesKernel>(selected, breaks, MatchCoords, MatchCoordinateBlocks);
        Kernel * const matchK = P.CreateKernelCall<MatchReporter>(byteStream, MatchCoords, accumulator);
        P.LinkFunction(matchK, "accumulate_match_wrapper", accumulate_match_wrapper);
        P.LinkFunction(matchK, "finalize_match_wrapper", finalize_match_wrapper);
    } else {
        Kernel * const scanMatchK = P.CreateKernelCall<ScanMatchKernel>(selected, breaks, byteStream, accumulator, ScanMatchBlocks);
        P.LinkFunction(scanMatchK, "accumulate_match_wrapper", accumulate_match_wrapper);
        P.LinkFunction(scanMatchK, "finalize_match_wrapper", finalize_match_wrapper);
    }

    mNested.push_back(kernel);
    mIsFilter.push_back(filter);

    mMainMethod.push_back(P.compile());
    assert (mMainMethod.size() + 1 == mNested.size());

    mGrepDriver.setPreserveKernels(preserve);
}

void NestedInternalSearchEngine::pop() {
    assert (mNested.size() > 1);
    mNested.pop_back();
    mIsFilter.pop_back();
    assert (mMainMethod.size() > 0);
    mMainMethod.pop_back();
    assert (mMainMethod.size() + 1 == mNested.size());
}

void NestedInternalSearchEngine::doGrep(const char * search_buffer, size_t bufferLength, MatchAccumulator & accum) {
    assert (mMainMethod.size() > 0);
    auto f = mMainMethod.back(); assert (f);
    assert ((((uintptr_t)search_buffer) % (512 / 8)) == 0);
    f(search_buffer, bufferLength, accum);
}

NestedInternalSearchEngine::~NestedInternalSearchEngine() { }


}

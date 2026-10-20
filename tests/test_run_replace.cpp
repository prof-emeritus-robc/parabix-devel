/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <testing/testing.h>
#include <kernel/streamutils/run_replace.h>
#include <kernel/bitwise/bixlogic.h>
#include <kernel/pipeline/program_builder.h>
#include <string>

using namespace kernel;
using namespace testing;

// Short cases, as in the documentation of RunReplace.
auto work_i = BinaryStream("...11..1..");
auto placement_i = BinaryStream("...111..1..");
auto source_i = TextStream("abcDEfgHij");
auto transformed_i = TextStream("XYZw");
auto unchanged_e = TextStream("abcfgij");
auto replaced_e = TextStream("abcXYZfgwij");

TEST_CASE(run_filter_short, work_i, source_i, unchanged_e) {
    auto Unchanged = P.CreateStreamSet(1, 1);
    Invert(P, Input<0>(T), Unchanged);
    auto Result = P.CreateStreamSet(1, 8);
    RunFilter(P, Unchanged, Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(run_merge_short, placement_i, transformed_i, unchanged_e, replaced_e) {
    auto Result = P.CreateStreamSet(1, 8);
    RunMerge(P, Input<0>(T), Input<1>(T), Input<2>(T), Result);
    AssertEQ(P, Result, Input<3>(T));
}

TEST_CASE(run_replace_short, work_i, placement_i, source_i, transformed_i, replaced_e) {
    auto Result = P.CreateStreamSet(1, 8);
    RunReplace(P, Input<0>(T), Input<1>(T), Input<2>(T), Input<3>(T), Result);
    AssertEQ(P, Result, Input<4>(T));
}

// Long cases: runs of lengths 1..150 cross 64-bit words and blocks; the mask starts
// and ends within runs.
struct LongCase {
    std::string mask, marked, unmarked, merged, filteredMarked;
    LongCase() {
        unsigned n = 0;
        bool value = true;
        for (unsigned len = 1; n < 1200; len = (len * 7 + 3) % 151 + 1) {
            for (unsigned i = 0; i < len; ++i, ++n) {
                const char c = 'a' + (n % 26);
                mask += value ? '1' : '.';
                merged += value ? char(c - 32) : c;
                (value ? marked : unmarked) += value ? char(c - 32) : c;
            }
            value = !value;
        }
        // filtering the merged stream by the mask gives the marked bytes
        filteredMarked = marked;
    }
};
static const LongCase L;

// Sparse cases: short runs of 1 bits far apart, the last close to the end of the
// final partial block.
struct SparseCase {
    std::string mask, source, filtered;
    SparseCase() {
        const unsigned n = 1000;
        for (unsigned i = 0; i < n; ++i) {
            const char c = 'a' + (i % 26);
            const bool marked = (i % 97 == 40) || (i % 97 == 41) || (i == n - 7) || (i == n - 6);
            mask += marked ? '1' : '.';
            source += c;
            if (marked) filtered += c;
        }
    }
};
static const SparseCase SP;

auto long_mask_i = BinaryStream(L.mask.c_str());
auto long_marked_i = TextStream(L.marked.c_str());
auto long_unmarked_i = TextStream(L.unmarked.c_str());
auto long_merged_i = TextStream(L.merged.c_str());

TEST_CASE(run_filter_long, long_mask_i, long_merged_i, long_marked_i) {
    auto Result = P.CreateStreamSet(1, 8);
    RunFilter(P, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(run_merge_long, long_mask_i, long_marked_i, long_unmarked_i, long_merged_i) {
    auto Result = P.CreateStreamSet(1, 8);
    RunMerge(P, Input<0>(T), Input<1>(T), Input<2>(T), Result);
    AssertEQ(P, Result, Input<3>(T));
}

auto sparse_mask_i = BinaryStream(SP.mask.c_str());
auto sparse_source_i = TextStream(SP.source.c_str());
auto sparse_filtered_e = TextStream(SP.filtered.c_str());

TEST_CASE(run_filter_sparse, sparse_mask_i, sparse_source_i, sparse_filtered_e) {
    auto Result = P.CreateStreamSet(1, 8);
    RunFilter(P, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

// Gap filling: gaps of at most 2 between 1 bits are filled; gaps at either end are not.
auto gaps_i = BinaryStream("..11.111...1....11.1..");
auto gaps_e = BinaryStream("..111111...1....1111..");

TEST_CASE(fill_run_gaps, gaps_i, gaps_e) {
    auto Result = P.CreateStreamSet(1, 1);
    FillRunGaps(P, Input<0>(T), 2, Result);
    AssertEQ(P, Result, Input<1>(T));
}

RUN_TESTS(
    CASE(run_filter_short),
    CASE(run_merge_short),
    CASE(run_replace_short),
    CASE(run_filter_long),
    CASE(run_merge_long),
    CASE(run_filter_sparse),
    CASE(fill_run_gaps)
)

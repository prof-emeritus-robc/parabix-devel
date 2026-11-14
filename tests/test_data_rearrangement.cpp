/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <testing/testing.h>
#include <kernel/scan/data_rearrangement.h>
#include <kernel/pipeline/program_builder.h>
#include <string>
#include <vector>

using namespace kernel;
using namespace testing;

//  A rearrangement case: the separator stream marks each ',' and '\n' of the text,
//  and the expected output is computed as specified for DataRearrangementKernel.
struct Rearrangement {
    std::string text, separators, expected;
    Rearrangement(const std::string & t, const unsigned N, const std::vector<unsigned> & V) : text(t) {
        std::vector<size_t> seps;
        for (size_t i = 0; i < text.size(); ++i) {
            const bool sep = (text[i] == ',' || text[i] == '\n');
            separators += sep ? '1' : '.';
            if (sep) seps.push_back(i);
        }
        const size_t avail = text.size();
        auto emit = [&](const std::vector<size_t> & sp, const size_t start) {
            for (size_t i = 0; i < V.size(); ++i) {
                const size_t k = V[i] - 1;
                const size_t end = sp[k];
                const size_t s = std::min((k == 0) ? start : sp[k - 1] + 1, end);
                expected += text.substr(s, end - s);
                const size_t p = sp[(i == V.size() - 1) ? (N - 1) : i];
                if (p < avail) expected += text[p];
            }
        };
        size_t start = 0;
        size_t i = 0;
        for (; i + N <= seps.size(); i += N) {
            std::vector<size_t> sp(seps.begin() + i, seps.begin() + i + N);
            emit(sp, start);
            start = sp.back() + 1;
        }
        if (i < seps.size() || start < avail) {
            std::vector<size_t> sp(seps.begin() + i, seps.end());
            sp.resize(N, avail);
            emit(sp, start);
        }
    }
};

static std::string longRecords() {
    std::string t;
    for (unsigned r = 0; r < 40; ++r) {
        // field 3 of every 7th record is longer than a segment
        const size_t longLength = (r % 7 == 3) ? 100000 + r : (r % 5) * 37;
        t += "r" + std::to_string(r) + ",";
        t += std::string(r % 3, 'b') + ",";
        t += std::string(longLength, char('c' + (r % 20))) + ",";
        t += "d" + std::to_string(r * r) + "\n";
    }
    return t;
}

static const Rearrangement reorder("a,b,c\n1,22,333\n,,\nx,,z\n", 3, {3, 1});
static const Rearrangement subset("a,b,c\n1,22,333\n,,\nx,,z\n", 3, {2});
static const Rearrangement permute("a,b,c\n1,22,333\n,,\nx,,z\n", 3, {2, 3, 1});
static const Rearrangement unterminated("a,b,c\n1,22", 3, {3, 1});
static const Rearrangement longFields(longRecords(), 4, {3, 4, 1});

#define REARRANGEMENT_INPUTS(R) \
auto R##_sep_i = BinaryStream(R.separators.c_str()); \
auto R##_text_i = TextStream(R.text.c_str()); \
auto R##_e = TextStream(R.expected.c_str());

REARRANGEMENT_INPUTS(reorder)
REARRANGEMENT_INPUTS(subset)
REARRANGEMENT_INPUTS(permute)
REARRANGEMENT_INPUTS(unterminated)
REARRANGEMENT_INPUTS(longFields)

TEST_CASE(rearrange_reorder, reorder_sep_i, reorder_text_i, reorder_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{3, 1}, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_subset, subset_sep_i, subset_text_i, subset_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{2}, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_permute, permute_sep_i, permute_text_i, permute_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{2, 3, 1}, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_unterminated, unterminated_sep_i, unterminated_text_i, unterminated_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{3, 1}, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

//  Records longer than a segment: their output is written in a later invocation than
//  most of their input, in the space the pipeline provides for held-back deferred input.
TEST_CASE(rearrange_long_fields, longFields_sep_i, longFields_text_i, longFields_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(4, std::vector<unsigned>{3, 4, 1}, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

RUN_TESTS(
    CASE(rearrange_reorder),
    CASE(rearrange_subset),
    CASE(rearrange_permute),
    CASE(rearrange_unterminated),
    CASE(rearrange_long_fields)
)

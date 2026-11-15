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

//  A rearrangement case: the separator stream marks the first code unit of each
//  delimiter (a string of one or more code units) and each '\n' of the text, and
//  the expected output is computed as specified for DataRearrangementKernel.
struct Rearrangement {
    std::string text, separators, expected;
    Rearrangement(const std::string & t, const unsigned N, const std::vector<unsigned> & V,
                  const std::string & delim = ",", const int prefix = -1) : text(t) {
        const size_t L = delim.size();
        std::vector<size_t> seps;
        for (size_t i = 0; i < text.size(); ) {
            if (text.compare(i, L, delim) == 0) {
                separators += '1' + std::string(L - 1, '.');
                seps.push_back(i);
                i += L;
            } else {
                const bool sep = (text[i] == '\n');
                separators += sep ? '1' : '.';
                if (sep) seps.push_back(i);
                i += 1;
            }
        }
        const size_t avail = text.size();
        auto emit = [&](const std::vector<size_t> & sp, const size_t start, const bool terminated) {
            //  The record separator, with its prefix code unit if any.
            const size_t lastStart = (N == 1) ? start : sp[N - 2] + L;
            const bool hasPrefix = terminated && prefix >= 0 && sp[N - 1] > lastStart && text[sp[N - 1] - 1] == char(prefix);
            const size_t recordSepStart = sp[N - 1] - (hasPrefix ? 1 : 0);
            for (size_t i = 0; i < V.size(); ++i) {
                const size_t k = V[i] - 1;
                const size_t end = (k == N - 1) ? recordSepStart : sp[k];
                const size_t s = std::min((k == 0) ? start : sp[k - 1] + L, end);
                expected += text.substr(s, end - s);
                const bool recordSep = (i == V.size() - 1);
                const size_t p = sp[recordSep ? (N - 1) : i];
                if (recordSep && hasPrefix) {
                    expected += text.substr(recordSepStart, 2);
                } else if (p < avail) {
                    expected += text.substr(p, recordSep ? 1 : L);
                }
            }
        };
        size_t start = 0;
        size_t i = 0;
        for (; i + N <= seps.size(); i += N) {
            std::vector<size_t> sp(seps.begin() + i, seps.begin() + i + N);
            emit(sp, start, true);
            start = sp.back() + 1;
        }
        if (i < seps.size() || start < avail) {
            std::vector<size_t> sp(seps.begin() + i, seps.end());
            sp.resize(N, avail);
            emit(sp, start, false);
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

//  Records with a 3-code-unit delimiter, at varied offsets, so that delimiters
//  straddle scan word, block and stride boundaries.
static std::string delimitedRecords(const std::string & d) {
    std::string t;
    for (unsigned r = 0; r < 400; ++r) {
        t += "r" + std::to_string(r) + d;
        t += std::string((r * 13) % 71, 'b') + d;
        t += std::string((r * 7) % 29, char('c' + (r % 20))) + d;
        t += "d" + std::to_string(r * r) + "\n";
    }
    return t;
}

static const Rearrangement reorder("a,b,c\n1,22,333\n,,\nx,,z\n", 3, {3, 1});
static const Rearrangement subset("a,b,c\n1,22,333\n,,\nx,,z\n", 3, {2});
static const Rearrangement permute("a,b,c\n1,22,333\n,,\nx,,z\n", 3, {2, 3, 1});
static const Rearrangement unterminated("a,b,c\n1,22", 3, {3, 1});
static const Rearrangement longFields(longRecords(), 4, {3, 4, 1});
static const Rearrangement multiUnitDelim("a~|~b~|~c\n1~|~22~|~333\n~|~~|~\nx~|~~|~z\n", 3, {3, 1, 2}, "~|~");
static const Rearrangement multiUnitLast("a~|~b~|~c\n1~|~22~|~333\n", 3, {1, 3}, "~|~");
static const Rearrangement multiUnitUnterminated("a~|~b~|~c\n1~|~22", 3, {2, 3, 1}, "~|~");
//  CRLF record separators, with the CR as the record separator prefix: also a
//  CR that is not before the LF, an empty last field, and an unterminated record.
static const Rearrangement crlf("a,b,c\r\n1,22,333\r\nx,\r,\r\n,,\n4,5,6\r", 3, {3, 1}, ",", '\r');
static const Rearrangement crlfMultiUnit("a~|~b~|~c\r\n1~|~22~|~333\r\n~|~~|~\r\nx~|~y~|~z", 3, {2, 3}, "~|~", '\r');
static const Rearrangement multiUnitBoundaries(delimitedRecords("~|~"), 4, {4, 2, 1}, "~|~");

#define REARRANGEMENT_INPUTS(R) \
auto R##_sep_i = BinaryStream(R.separators.c_str()); \
auto R##_text_i = TextStream(R.text.c_str()); \
auto R##_e = TextStream(R.expected.c_str());

REARRANGEMENT_INPUTS(reorder)
REARRANGEMENT_INPUTS(subset)
REARRANGEMENT_INPUTS(permute)
REARRANGEMENT_INPUTS(unterminated)
REARRANGEMENT_INPUTS(longFields)
REARRANGEMENT_INPUTS(multiUnitDelim)
REARRANGEMENT_INPUTS(multiUnitLast)
REARRANGEMENT_INPUTS(multiUnitUnterminated)
REARRANGEMENT_INPUTS(multiUnitBoundaries)
REARRANGEMENT_INPUTS(crlf)
REARRANGEMENT_INPUTS(crlfMultiUnit)

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

//  Delimiters of several code units (as multibyte UTF-8 delimiters), marked at
//  their first code unit and copied whole.
TEST_CASE(rearrange_multi_unit_delim, multiUnitDelim_sep_i, multiUnitDelim_text_i, multiUnitDelim_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{3, 1, 2}, Input<0>(T), Input<1>(T), Result, 3);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_multi_unit_last, multiUnitLast_sep_i, multiUnitLast_text_i, multiUnitLast_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{1, 3}, Input<0>(T), Input<1>(T), Result, 3);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_multi_unit_unterminated, multiUnitUnterminated_sep_i, multiUnitUnterminated_text_i, multiUnitUnterminated_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{2, 3, 1}, Input<0>(T), Input<1>(T), Result, 3);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_multi_unit_boundaries, multiUnitBoundaries_sep_i, multiUnitBoundaries_text_i, multiUnitBoundaries_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(4, std::vector<unsigned>{4, 2, 1}, Input<0>(T), Input<1>(T), Result, 3);
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_crlf, crlf_sep_i, crlf_text_i, crlf_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{3, 1}, Input<0>(T), Input<1>(T), Result, 1, '\r');
    AssertEQ(P, Result, Input<2>(T));
}

TEST_CASE(rearrange_crlf_multi_unit, crlfMultiUnit_sep_i, crlfMultiUnit_text_i, crlfMultiUnit_e) {
    auto Result = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(3, std::vector<unsigned>{2, 3}, Input<0>(T), Input<1>(T), Result, 3, '\r');
    AssertEQ(P, Result, Input<2>(T));
}

RUN_TESTS(
    CASE(rearrange_reorder),
    CASE(rearrange_subset),
    CASE(rearrange_permute),
    CASE(rearrange_unterminated),
    CASE(rearrange_long_fields),
    CASE(rearrange_multi_unit_delim),
    CASE(rearrange_multi_unit_last),
    CASE(rearrange_multi_unit_unterminated),
    CASE(rearrange_multi_unit_boundaries),
    CASE(rearrange_crlf),
    CASE(rearrange_crlf_multi_unit)
)

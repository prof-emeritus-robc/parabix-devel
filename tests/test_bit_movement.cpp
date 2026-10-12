/*
 * Copyright (c) 2019 International Characters.
 * This software is licensed to the public under the Open Software License 3.0.
 */

#include <testing/testing.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/bitwise/bixlogic.h>
#include <kernel/pipeline/program_builder.h>

using namespace kernel;
using namespace testing;

auto insert_mask_i = BinaryStream({"11100001"});
auto insert_before_e = BinaryStream({"010101111101"});

TEST_CASE(insert_before1, insert_mask_i, insert_before_e) {
    auto Result = P.CreateStreamSet(1);
    UnitInsertionSpreadMask(T, Input<0>(T), Result, InsertPosition::Before);
    AssertEQ(P, Result, Input<1>(T));
}

auto insert_after_e = BinaryStream({"101010111110"});

TEST_CASE(insert_after1, insert_mask_i, insert_after_e) {
    auto Result = P.CreateStreamSet(1);
    UnitInsertionSpreadMask(T, Input<0>(T), Result, InsertPosition::After);
    AssertEQ(P, Result, Input<1>(T));
}

auto insert_counts = BinaryStreamSet({"001000101", "001000000", "100000000"});
auto insert_mult_before_e = BinaryStream("000011000111101101");
auto insert_mult_after_e = BinaryStream("100001100011110110");

TEST_CASE(insert_mult_after, insert_counts, insert_mult_after_e) {
    auto Result = P.CreateStreamSet(1);
    InsertionSpreadMask(T, Input<0>(T), Result, InsertPosition::After);
    AssertEQ(P, Result, Input<1>(T));
}

TEST_CASE(insert_mult_before, insert_counts, insert_mult_before_e) {
    auto Result = P.CreateStreamSet(1);
    InsertionSpreadMask(T, Input<0>(T), Result, InsertPosition::Before);
    AssertEQ(P, Result, Input<1>(T));
}

auto to_filter = BinaryStreamSet({"01010101011", "11110000111", "11010011000"});
auto filter_mask = BinaryStream({"11110000111"});
auto filtered = BinaryStreamSet({"0101011", "1111111", "1101000"});

TEST_CASE(filter1, filter_mask, to_filter, filtered) {
    auto Result = P.CreateStreamSet(3);
    FilterByMask(T, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

auto spread = BinaryStreamSet({"01010000011", "11110000111", "11010000000"});

TEST_CASE(spread1, filter_mask, filtered, spread) {
    auto Result = P.CreateStreamSet(3);
    SpreadByMask(T, Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

auto marker =    BinaryStreamSet({"1011101001010000"});
auto indexStrm = BinaryStreamSet({"1011101101011000"});
auto advmarker = BinaryStreamSet({"0011101100011000"});

TEST_CASE(indexedadvance1, indexStrm, marker, advmarker) {
    auto Result = P.CreateStreamSet(1);
    P.CreateKernelCall<IndexedAdvance>(Input<0>(T), Input<1>(T), Result, 1);
    AssertEQ(P, Result, Input<2>(T));
}

auto bakmarker = BinaryStreamSet({"1011100101000000"});

TEST_CASE(indexedshiftback1, indexStrm, marker, bakmarker) {
    auto Result = P.CreateStreamSet(1);
    P.CreateKernelCall<IndexedShiftBack>(Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

auto longmarker =    BinaryStreamSet({"..0.{25000}11001.{3277}1.01",
                                      "..1.{25000}11111.{3277}1.11",
                                      "..1.{25000}00110.{3277}1.10"});
auto longindexStrm = BinaryStreamSet({"..1.{25000}11111.{3277}1.11"});
auto longbakmarker = BinaryStreamSet({"..1.{25000}10011.{3277}0.10",
                                      "..1.{25000}11111.{3277}1.10",
                                      "..0.{25000}01101.{3277}1.00"});

TEST_CASE(longindexedshiftback, longindexStrm, longmarker, longbakmarker) {
    auto Result = P.CreateStreamSet(3);
    P.CreateKernelCall<IndexedShiftBack>(Input<0>(T), Input<1>(T), Result);
    AssertEQ(P, Result, Input<2>(T));
}

// A repeating mask: 2 positions inserted before each group of 4 items.  The
// output ends immediately after the last item, here within the third group.
auto repeat_spread_partial_i = BinaryStreamSet({"1101000011"});
auto repeat_spread_partial_e = BinaryStreamSet({"0011010000000011"});

TEST_CASE(repeat_spread_partial, repeat_spread_partial_i, repeat_spread_partial_e) {
    auto Mask = P.CreateRepeatingStreamSet(1, std::vector<uint64_t>{0, 0, 1, 1, 1, 1});
    auto Result = P.CreateStreamSet(1);
    SpreadByMask(T, Mask, Input<0>(T), Result);
    AssertEQ(P, Result, Input<1>(T));
    // AssertEQ zero-extends; the inverted streams also check the output length.
    auto NotResult = P.CreateStreamSet(1);
    Invert(P, Result, NotResult);
    auto NotExpected = P.CreateStreamSet(1);
    Invert(P, Input<1>(T), NotExpected);
    AssertEQ(P, NotResult, NotExpected);
}

// Many blocks of a repeating mask whose period (6) does not divide the block width.
auto repeat_spread_long_i = BinaryStreamSet({"0100001101000100000000110000110110010110101111101100101101110100000111110110100110110111000110001001111000110001001100110101111111100101100111010111111100111111111111011100011101011100010000100000101001111000101111010101111011110011000101010111101111101111101111010010100101101001101111000111101110001100011111101101011110011101000100011011000000110001000001010010010111110010101001100111000100001011110010010001001101111101111000100000000111011111110000011101010100101110011001110000000111011101000001101011000100010100011001111000010001000000111110011000110111000100101010001100111010010001101010100111100000001110010101100001001110111011010011000100000111100101101100111000110001010111001000101011111001100110001110100001101000111011001110100011100010001101101110101000101010100100101101000000101011001000011010011100100111011001101101100111010010100101101001100001001110000101011000110001111001010101011010111000100101000111010010001010110010100111111001001001000010010001101110011111001001011101"});
auto repeat_spread_long_e = BinaryStreamSet({"000100000011000100000100000000000011000000001101001001000110001011001110001100001011000111000100000001001111000110001001001011000111000001001000001001001110000011000001000011000011000101001111001110000101001001001101000111001111000011001111001111001101001100000111000101001100000100000010000000001010000111001000001011001101000101001110001111000011000001000101000111001011001110001111001011001101000010001001000110001001001011001100000111001011001000001100000111001110001101000111001001001101000001000001001011000000000011000001000000000101000010000101001111000010001010000110000111000001000000001011001100001001000001000011000111001101001110000010000000000001001101001111001100000001001101000101000010001110000110000111000000000001001101001101000000000110001011000001000001000100000110000111001000000100000100000000001111001001001000001101001100000100001010001000001100001110001001000001001010001010000111001000000000001110000101000110000001000011001011001011000100001100000100000001001110000101001011000011001000001100000101000111000010000010001011001110000110000110000011001010000001001010000011001011000011001010000011001000001000001101001011001010001000001010001010000100001011000100000000001010001100001000000110001001001100001001001101001001001011000110000111000100001010000101001010000110000001000011001000000101000110000011000001001110000101000101000110001011001000001001000100000111000100001000001010001100001010000111001110000100001001000000001001000001001011001001001111000010000101001101"});

TEST_CASE(repeat_spread_long, repeat_spread_long_i, repeat_spread_long_e) {
    auto Mask = P.CreateRepeatingStreamSet(1, std::vector<uint64_t>{0, 0, 1, 1, 1, 1});
    auto Result = P.CreateStreamSet(1);
    SpreadByMask(T, Mask, Input<0>(T), Result);
    AssertEQ(P, Result, Input<1>(T));
    // AssertEQ zero-extends; the inverted streams also check the output length.
    auto NotResult = P.CreateStreamSet(1);
    Invert(P, Result, NotResult);
    auto NotExpected = P.CreateStreamSet(1);
    Invert(P, Input<1>(T), NotExpected);
    AssertEQ(P, NotResult, NotExpected);
}

// Insertion after each pair: the output ends after the last item, without the
// positions that would follow it.
auto repeat_spread_after_i = BinaryStreamSet({"1010010"});
auto repeat_spread_after_e = BinaryStreamSet({"1000100001000"});

TEST_CASE(repeat_spread_after, repeat_spread_after_i, repeat_spread_after_e) {
    auto Mask = P.CreateRepeatingStreamSet(1, std::vector<uint64_t>{1, 1, 0, 0});
    auto Result = P.CreateStreamSet(1);
    SpreadByMask(T, Mask, Input<0>(T), Result);
    AssertEQ(P, Result, Input<1>(T));
    // AssertEQ zero-extends; the inverted streams also check the output length.
    auto NotResult = P.CreateStreamSet(1);
    Invert(P, Result, NotResult);
    auto NotExpected = P.CreateStreamSet(1);
    Invert(P, Input<1>(T), NotExpected);
    AssertEQ(P, NotResult, NotExpected);
}

RUN_TESTS(
          CASE(insert_before1),
          CASE(insert_after1),
          CASE(insert_mult_after),
          CASE(insert_mult_before),
          CASE(filter1),
          CASE(spread1),
          CASE(indexedadvance1),
          CASE(indexedshiftback1),
          CASE(longindexedshiftback),
          CASE(repeat_spread_partial),
          CASE(repeat_spread_long),
          CASE(repeat_spread_after)
)

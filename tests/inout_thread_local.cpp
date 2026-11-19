/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  An in-place stream set (an InOut binding) whose base is a thread-local
//  stream set of a partition: one produced and consumed within the partition,
//  in memory that the partition root lays out, and reallocates, only when the
//  number of its strides grows.  The replacement shares the base's memory, so
//  it must have the base's offsets wherever they are merged across the root's
//  loop (after a reallocation, after termination), not only where they are
//  computed, in the reallocation block, which does not dominate the kernels
//  that set up their buffers from them.  The base must not be the first stream
//  set placed in the thread-local memory, whose offset is the constant 0.  The
//  result is then filtered by a mask (FilterByMask), which compares the whole
//  data through another partition.
//
//  Run with -VerifyIR (see tests/CMakeLists.txt): before the fix, the
//  pipeline's IR did not verify ("Instruction does not dominate all uses!"),
//  with one thread or several.

#include <testing/testing.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/bitwise/bixlogic.h>

using namespace kernel;
using namespace testing;

auto mask = BinaryStream({"0110100101000100010111001010111110100101010111111011100111000111"
                          "1110100000111111100011011111111011011010011101111010010001111111"
                          "1100110101001000111011111110011010011110110111110010011101110000"
                          "0110000110011011001110110011110111011111100111011101101100001101"
                          "0111000101100111011010111101110011011110011111111101101111100111"
                          "0100100101111011010011010000100010111001111011101100111111101101"
                          "1110010111100011110111111000011110010111110011111000110011010011"
                          "0111111001011011011101010100110000011111101101011100101111111111"});
auto data = BinaryStreamSet({"1111011101110000111010100000111011111011100100100000001111100100"
                              "0101011010111101011001010010001111100101011010110011111111110111"
                              "0111111101101011111110101100010101001110111010001011001010011101"
                              "0011011000110010000010000010011000000101000111001111000011100111"
                              "1101100100100110001000110001111001011011010100110101111101100100"
                              "1001111100110001001001111100010001111111001100100001101101101010"
                              "0011110011011110010100001111110010100010010011111111000110101111"
                              "1100010001100100100010101110000111001110000110000011000000010111",
                             "1101101001110110000101110111000111110001011010000101001010010000"
                              "1111111101001110000010010100100101011100001101110000010001110011"
                              "0101111001001101110111000111100011010111100000011000011111011011"
                              "1100110100100010001100101101000011100010111110100110000011111011"
                              "1101111001000001011001101110101101111011001011110100110011011101"
                              "1010001001111100001001111000100010011110011101001111001000011100"
                              "0100101100100110111110100111101100101101101010001100111110101001"
                              "1111010100101110100011010110101000111011100100010011000011010011"});
auto filtered = BinaryStreamSet({"1101101010001110110101001000001111000100111101001100100011100011"
                                  "0011011111011101111111111010110100011111010001010001010010100010"
                                  "0100100000101011101110000111011011100101100111011101101001101111"
                                  "1011100011011001001100111100100100101101110000110110100110000110"
                                  "01001001111110010011100010100000000010001110001000000000010111",
                                 "1010110101010001110110100000100100001111001110010101001000111001"
                                  "1111001111001101110111101100011001101110000010111101101000101101"
                                  "0010001100010111000100001011010100011101011010011101010111101010"
                                  "0110101000111100001111011001101011001000011001001001101111010001"
                                  "10010110100011110001111010001100001111011011101010000011010011"});

TEST_CASE(inout_thread_local, mask, data, filtered) {
    StreamSet * const D = Input<1>(T);
    //  Copies of the data (thread-local: produced and consumed in one
    //  partition).  W is live while X is, so X is placed after W in the
    //  thread-local memory: its offset is computed where the memory is
    //  (re)allocated, not the constant 0 of the first stream set placed ...
    StreamSet * const W = P.CreateStreamSet(2);
    AndCombine(P, D, D, W);
    StreamSet * const X = P.CreateStreamSet(2);
    AndCombine(P, W, D, X);
    //  ... and X is modified in place: X | W, which is D ...
    StreamSet * const Y = P.CreateStreamSet(2);
    OrCombine(P, X, W, Y, CombiningKind::InOut);
    //  ... and read in the same partition (so neither X nor Y is read in
    //  another), then filtered.
    StreamSet * const Z = P.CreateStreamSet(2);
    AndCombine(P, Y, Y, Z);
    StreamSet * const F = P.CreateStreamSet(2);
    FilterByMask(P, Input<0>(T), Z, F);
    AssertEQ(P, F, Input<2>(T));
}

RUN_TESTS(
    CASE(inout_thread_local),
)

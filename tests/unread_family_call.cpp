/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  A family call whose output is never read: the pipeline removes the kernel,
//  but the arguments of its family call are still passed to the pipeline, and
//  must be consumed without being stored (the removed kernel has no state to
//  store them in).  Before the fix, the pipeline failed to compile with
//  "Scalar map for ... does not contain <n>", where <n> was the id of the
//  removed kernel.  The family call after it checks that the arguments of the
//  remaining family calls are still taken in order.

#include <testing/testing.h>
#include <kernel/streamutils/stream_shift.h>

using namespace kernel;
using namespace testing;

// (The data ends with 0: a 1 shifted past the end would be compared too.)
auto data = BinaryStream({"1010001000011000100001000011001000100001111111000011111001010110"
                          "0111110011001111101100100100111001110111110000000010110011100111"
                          "1101100001001000001000101111001111100011100010010110101000100110"
                          "0111011110000101010110010101101110000001011000000100010101110010"});
auto shifted = BinaryStream({"0101000100001100010000100001100100010000111111100001111100101011"
                             "0011111001100111110110010010011100111011111000000001011001110011"
                             "1110110000100100000100010111100111110001110001001011010100010011"
                             "0011101111000010101011001010110111000000101100000010001010111001"});

TEST_CASE(unread_family_call, data, shifted) {
    StreamSet * const D = Input<0>(T);
    StreamSet * const unread = P.CreateStreamSet(1);
    P.CreateKernelFamilyCall<ShiftForward>(D, unread, 2);
    StreamSet * const S = P.CreateStreamSet(1);
    P.CreateKernelFamilyCall<ShiftForward>(D, S, 1);
    AssertEQ(P, S, Input<1>(T));
}

RUN_TESTS(
    CASE(unread_family_call),
)

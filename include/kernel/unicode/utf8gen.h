/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/pablo.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/driver/driver.h>


//
// U21_to_UTF8 transforms a basis set of 21 Unicode bit streams
// into a corresponding UTF-8 representation in the form 8 parallel
// bit streams.  Precondition:  The output streamset U8 has been
// created (using CreateStreamSet(8, 1)), but not initialized.
//

void U21_to_UTF8(kernel::PipelineBuilder & P, kernel::StreamSet * U21, kernel::StreamSet * U8);

//
// UTF-8 encoding requires one to four bytes per Unicode character.
// To generate UTF-8 encoded output from sets of basis bit streams
// representing Unicode characters (that is, codepoint-indexed streams
// having one bit position per codepoint), deposit masks are needed
// to identify the positions at which bits for each character are
// to be deposited.   A UTF-8 deposit mask will have one to four bit
// positions per character depending on the character being encoded, that is,
// depending on the number of bytes needed to encode the character.   Within
// each group of one to four positions for a single character, a deposit mask
// must have exactly one 1 bit set.  Different deposit masks are used for
// depositing bits, depending on the destination byte position within the
// ultimate byte sequence.
//
// The following deposit masks (shown in little-endian representation) are
// used for depositing bits.
//
//  UTF-8 sequence length:          1     2     3       4
//  Unicode bit position:
//  Unicode codepoint bits 0-5      1    10   100    1000    u8final
//  Bits 6-11                       1    01   010    0100    u8mask6_11
//  Bits 12-17                      1    01   001    0010    u8mask12_17
//  Bits 18-20                      1    01   001    0001    u8initial
//
//  To compute UTF-8 deposit masks, we begin by constructing an extraction
//  mask having 4 bit positions per character, but with the number of
//  1 bits to be kept dependent on the sequence length.  When this extraction
//  mask is applied to the repeating constant 4-bit mask 1000, u8final above
//  is produced.
//
//  UTF-8 sequence length:             1     2     3       4
//  extraction mask                 1000  1100  1110    1111
//  constant mask                   1000  1000  1000    1000
//  final position mask             1     10    100     1000
//  From this mask, other masks may subsequently computed by
//  bitwise logic and shifting.
//
//  The UTF8fieldDepositMask kernel produces this deposit mask
//  within 64-bit fields.

class UTF8fieldDepositMask final : public kernel::BlockOrientedKernel {
public:
    UTF8fieldDepositMask(LLVMTypeSystemInterface & ts, kernel::StreamSet * u32basis, kernel::StreamSet * u8fieldMask, kernel::StreamSet * u8unitCounts,
                         unsigned depositFieldWidth = sizeof(size_t) * 8);
private:
    void generateDoBlockMethod(kernel::KernelBuilder & b) override;
    void generateFinalBlockMethod(kernel::KernelBuilder & b, llvm::Value * const remainingBytes) override;
    const unsigned mDepositFieldWidth;
};

//
// Given a u8-indexed bit stream marking the final code unit position
// of each UTF-8 sequence, this kernel computes the deposit masks
// u8initial, u8mask12_17, and u8mask6_11.
//
class UTF8_DepositMasks : public pablo::PabloKernel {
public:
    UTF8_DepositMasks(LLVMTypeSystemInterface & ts, kernel::StreamSet * u8final, kernel::StreamSet * u8initial,
                      kernel::StreamSet * u8mask12_17, kernel::StreamSet * u8mask6_11);
protected:
    void generatePabloMethod() override;
};

// This kernel assembles the UTF-8 basis bit data, given four sets of deposited
// bits bits 18-20, 11-17, 6-11 and 0-5, as weil as the marker streams u8initial,
// u8final, u8prefix3 and u8prefix4.
//
class UTF8assembly : public pablo::PabloKernel {
public:
    UTF8assembly(LLVMTypeSystemInterface & ts,
                 kernel::StreamSet * deposit18_20, kernel::StreamSet * deposit12_17, kernel::StreamSet * deposit6_11, kernel::StreamSet * deposit0_5,
                 kernel::StreamSet * u8initial, kernel::StreamSet * u8final, kernel::StreamSet * u8mask6_11, kernel::StreamSet * u8mask12_17,
                 kernel::StreamSet * u8basis);
protected:
    void generatePabloMethod() override;
};


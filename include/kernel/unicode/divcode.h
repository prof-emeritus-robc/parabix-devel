/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  DivCode kernels: the Div32Code / Div64Code of each character, its number
//  in an alphabet of blocks of 2^shift codepoints (cp >> shift: shift 5 for
//  div32, 6 for div64), as a bixnum.  The blocks are numbered from 1, in the
//  order given; code 0 is that of every other character.  With the low shift
//  bits of the character, the code identifies each character of the blocks
//  (its pseudo-codepoint, (code << shift) | (cp & (2^shift - 1))), so that a
//  small pseudo-basis replaces the full basis for the character classes of
//  these characters.  (See doc/ldml-divcode-plan.md, where tconv applies the
//  conversion rules of LDML transforms by these codes.)

#pragma once

#include <string>
#include <vector>
#include <pablo/pablo.h>
#include <pablo/toolchain/pablo_toolchain.h>
#include <ucd/core/unicode_set.h>

namespace kernel {

//  The code bixnum (Code: as many streams as the bits of the highest code,
//  the number of blocks, or at least 1) of each position of a 21-bit codepoint
//  basis (one position per character), or of UTF-8 basis bits, where the code
//  is marked at the first code unit of each character (LookAhead) or at its
//  last (Advance).
//
//  The kernel depends only on the shift, the blocks and the basis: its name is
//  Div32Code or Div64Code, the alphabet (the blocks in hexadecimal, or a hash
//  of them if more than 8), the basis (U21, or U8 and the mode) and the UTF
//  compiler's annotation, so that uses of the same alphabet share it.
class DivCodeKernel final : public pablo::PabloKernel {
public:
    DivCodeKernel(LLVMTypeSystemInterface & ts, unsigned shift, std::vector<UCD::codepoint_t> blocks,
                  StreamSet * Basis, StreamSet * Code,
                  pablo::BitMovementMode mode = pablo::BitMovementMode::LookAhead);
    // The name of the alphabet: its blocks (x3_x21_x22), or "h" and a hash.
    static std::string alphabetName(const std::vector<UCD::codepoint_t> & blocks);
    // The kernel name, as above (basis: "U21", or "U8" and the mode).
    static std::string kernelName(unsigned shift, const std::vector<UCD::codepoint_t> & blocks, const std::string & basis);
protected:
    void generatePabloMethod() override;
private:
    const unsigned mShift;
    const std::vector<UCD::codepoint_t> mBlocks;
    const pablo::BitMovementMode mMode;
};

}

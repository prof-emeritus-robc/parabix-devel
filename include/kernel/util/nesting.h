/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/pablo.h>
#include <boost/intrusive/detail/math.hpp>
using boost::intrusive::detail::ceil_log2;

namespace kernel {

/*  The NestingDepth kernel computes a BixNum representation of the
    nesting depth of bracketted items at each data position.
    The brackets input is a streamset of 2 bitstreams L and R
    representing left and right bracketting symbols respectively.
    The depth output is a BixNum representing the depth at
    each position.   Calculations are done up to a maximum depth
    maxDepth.  The number of streams in the depth BixNum must be
    sufficient to encode the number of bits required to represent
    depth values up to maxDepth.  Bracketting errors are reported
    in the errs bitstream.
 
    Errors are of three types:
      (1) an unclosed bracket marked by an err bit at the EOF position.
      (2) a right bracket without a correspond left bracket, marked by
          a 1 bit in the err stream corresponding to a 1 bit in the R stream.
      (3) a left bracket which would exceed maxDepth, marked by a 1 bit
          in the err stream corresponding to a 1 bit in the L stream.
 
    Example:
    
    Bracket Stream      ...[..{.}..{...[.[]..]..}...]...
    L                   ...1..1....1...1.1..............
    R                   ........1.........1..1..1...1...
    nestingDepth[3]     ................................
    nestingDepth[2]     .................11.............
    nestingDepth[1]     ......111..111111..111111.......
    nestingDepth[0]     ...111...11....11..111...1111...

 */

class NestingDepth final: public pablo::PabloKernel {
public:
    NestingDepth(LLVMTypeSystemInterface & ts,
                 StreamSet * brackets,
                 StreamSet * depth, StreamSet * errs,
                 unsigned maxDepth = 15);
protected:
    void generatePabloMethod() override;
private:
    unsigned mMaxDepth;
    unsigned mNestingDepthBits;
};

}

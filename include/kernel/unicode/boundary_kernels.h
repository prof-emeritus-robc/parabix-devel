/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once
#include <pablo/pablo.h>


namespace kernel {
    
/*  Given a set of 1 or more property basis streams and an
    index stream marking final code unit positions, determine
    a boundary stream at each index position such that a
    boundary occurs at that position if any of the basis
    bits at that position is different from any of the basis
    bits at the prior position within the index stream.
    If the boolean flag invert is true, return the those index
    stream positions at which all the basis bits are the same as
    those at the prior index stream position.  */
class BoundaryKernel : public pablo::PabloKernel {
public:
    BoundaryKernel(LLVMTypeSystemInterface & ts, StreamSet * PropertyBasis, StreamSet * IndexStream, StreamSet * BoundaryStream, bool invert = false);
protected:
    void generatePabloMethod() override;
    bool mHasIndex;
    bool mInvert;
};

}

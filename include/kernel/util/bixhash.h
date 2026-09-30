/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once
#include <pablo/pablo.h>


namespace kernel {

/*  The BixHash kernel computes hash values for symbols in a source stream.
    Inputs: basis: source stream represented as a set of 8 basis bits.
            run: bit stream marking the nonstart bytes of symbols.
            steps: the number of steps to apply, which determines the
                   max length (1<<steps) of fully hashed values.
    Outputs:   hashes: hash values represented as n-bit bixnums.
 
    Note:   hash values are computed at every position within a symbol,
            based on the prefix of the symbol up to and including the
            given position.   This may be useful if symbol prefixes as
            well as symbols are to be indexed.
 */

class BixHash final: public pablo::PabloKernel {
public:
    BixHash(LLVMTypeSystemInterface & ts,
            StreamSet * basis, StreamSet * run, StreamSet * hashes, unsigned steps=4, unsigned seed = 179321)
    : PabloKernel(ts, "BixHash" + std::to_string(hashes->getNumElements()) + "_" + std::to_string(steps) + "_" + std::to_string(seed),
                  {Binding{"basis", basis}, Binding{"run", run}},
                  {Binding{"hashes", hashes}}),
    mHashBits(hashes->getNumElements()), mHashSteps(steps), mSeed(seed) {}
protected:
    void generatePabloMethod() override;
private:
    unsigned mHashBits;
    unsigned mHashSteps;
    unsigned mSeed;
};

}

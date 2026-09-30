#pragma once
#include <pablo/pablo.h>


namespace kernel {
    
/*  Given a stream with zero or more runs of 1 bits, this kernel
    generator enumerates the positions of each run (mod 2^K where
    K is the number of output streams).   The output is represented
    as a K-bit bixnum R, such that for each bit 0 <= i < K and each
    position p in a run from positions m to n,  R[i][p] = (p - m)/2^i mod 2.
 
    The runIndex parameter must be created and passed in as a stream
    set of K bitstreams.
 
    An overflow bit stream may also be added as an output parameter, in which
    case this stream will mark all those positions within runs that are
    indexed above 2^K - 1.
 
    If the invert parameter is set to true, then the run indexes are
    calculated for runs of 0 bits.
*/


class RunIndex : public pablo::PabloKernel {
public:
    enum class Kind {RunOf0, RunOf1};
    enum class Numbering {RunOnly, RunPlus1};
    RunIndex(LLVMTypeSystemInterface & ts,
               StreamSet * const runMarks, StreamSet * runIndex, StreamSet * overflow = nullptr, Kind k = Kind::RunOf1, Numbering n = Numbering::RunOnly);
    void generatePabloMethod() override;
private:
    unsigned mIndexCount;
    bool mOverflow;
    Kind mRunKind;
    Numbering mNumbering;
};

}


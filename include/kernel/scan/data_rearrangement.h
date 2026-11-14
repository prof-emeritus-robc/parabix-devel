/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <vector>
#include <kernel/core/kernel.h>

namespace kernel {

//  DataRearrangementKernel: rearranges the fields of records.
//
//  The input is divided into records of N fields each.  A single bit stream
//  of separators marks the position of each field separator, every Nth of
//  which is a record separator.  The data streamset <K x iM> (M a power of 2,
//  at least 8) holds the fields: a field is the data between one separator
//  and the next (the first field of a record begins after the previous
//  record separator), and each separator position holds a separator code unit.
//
//  The rearrangement specification is N and a vector V of distinct field
//  numbers (1-based), |V| <= N.  Each input record produces an output record
//  of |V| fields, in which output field i is a copy of input field V[i].  Output
//  field i is followed by the separator code unit at the position of input
//  separator i, except that the last output field is followed by the record
//  separator of the input record.  So separators keep their input slots,
//  and the record separator ends the record.
//
//  A record is written when its record separator is reached.  The data input
//  is deferred to the start of an incomplete record, so a record may extend
//  over any number of strides and segments.  As V is without repetitions, an
//  output record is no longer than its input record, and the output has
//  BoundedRate(0,1); the pipeline provides output space for the input held
//  back by the deferred data input.
//
//  At the end of the input, an unterminated final record is completed by
//  treating its missing separators as positioned at the end of the input;
//  no output is written for them.
//
class DataRearrangementKernel : public MultiBlockKernel {
public:
    DataRearrangementKernel(LLVMTypeSystemInterface & ts,
                            unsigned numFields, const std::vector<unsigned> & fieldOrder,
                            StreamSet * const Separators, StreamSet * const Data,
                            StreamSet * const Rearranged, unsigned strideBlocks = 8);
private:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
    const unsigned mNumFields;
    const std::vector<unsigned> mFieldOrder;
    const unsigned mStreamCount;
    const unsigned mFieldWidth;
};

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <map>
#include <tuple>
#include <vector>
#include <pablo/pablo.h>

namespace kernel {

class PipelineBuilder;
class StreamSet;

//
//  The changes a stage makes to the positions of a text, in order: positions
//  deleted (keep: 1 at the positions kept), or inserted (spread: 0 at the
//  positions inserted, as by InsertionSpreadMask).  A stage that inserts or
//  deletes positions returns its map, by which its client keeps its other
//  streams of one bit per position (masks, marks) in step with the text
//  (PositionMapper), instead of the stage carrying them itself.
//
//  The bit of an inserted position, in a stream mapped:
//    - Inherit: that of the position before it (it belongs to the run of the
//      character it follows);
//    - InheritNext: that of the position after it (as text inserted before a
//      character); the runs of inserted positions before a position are of at
//      most run positions;
//    - In: always 1;
//    - Out: always 0.
//
struct PositionMap {
    enum class Inserted {Inherit, InheritNext, In, Out};
    struct Edit {
        StreamSet * keep = nullptr;
        StreamSet * spread = nullptr;
        Inserted inserted = Inserted::Inherit;
        unsigned run = 0;       // InheritNext: the longest run of inserted positions
    };
    std::vector<Edit> edits;
    void remove(StreamSet * keep) {edits.push_back(Edit{keep, nullptr, Inserted::Inherit, 0});}
    void insert(StreamSet * spread, Inserted inserted = Inserted::Inherit, unsigned run = 0) {
        edits.push_back(Edit{nullptr, spread, inserted, run});
    }
    void append(const PositionMap & m) {edits.insert(edits.end(), m.edits.begin(), m.edits.end());}
    bool empty() const {return edits.empty();}
};

//
//  Streams of one bit per position mapped by the edits of position maps, and
//  their complements, each computed once: the kernels of a stream mapped by
//  an edit (or complemented) are shared by every request for it.
//
class PositionMapper {
public:
    explicit PositionMapper(PipelineBuilder & P) : mP(P) {}
    StreamSet * mapped(StreamSet * stream, const PositionMap & map);
    StreamSet * mapped(StreamSet * stream, const PositionMap::Edit & e);
    StreamSet * complement(StreamSet * stream);
private:
    enum Op {Complement, Filter, Inherit, InheritNext, In, Out};
    PipelineBuilder & mP;
    std::map<std::tuple<StreamSet *, StreamSet *, Op, unsigned>, StreamSet *> mCache;
};

//  A stream spread by a spread mask (Spread), each inserted position (0 in
//  SpreadMask) given the bit of the position before it.
class InheritPrevious final : public pablo::PabloKernel {
public:
    InheritPrevious(LLVMTypeSystemInterface & ts, StreamSet * SpreadMask, StreamSet * Spread, StreamSet * Inherited);
protected:
    void generatePabloMethod() override;
};

//  A stream spread by a spread mask (Spread), each inserted position given the
//  bit of the position after its run of inserted positions (of at most run).
class InheritNext final : public pablo::PabloKernel {
public:
    InheritNext(LLVMTypeSystemInterface & ts, StreamSet * SpreadMask, StreamSet * Spread, unsigned run, StreamSet * Inherited);
protected:
    void generatePabloMethod() override;
private:
    const unsigned mRun;
};

}

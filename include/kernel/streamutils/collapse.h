/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/core/kernel.h>
namespace kernel { class PipelineBuilder; }
namespace kernel { class StreamSet; }

namespace kernel {

/**
 * A simple kernel which collapses a set of N streams into a single stream.
 * 
 * kernel CollapseStreamSet :: [<i1>[N] input] -> [<i1>[1] output]
 */
class CollapseStreamSet : public BlockOrientedKernel {
public:
    CollapseStreamSet(LLVMTypeSystemInterface & ts, StreamSet * input, StreamSet * output);
protected:
    void generateDoBlockMethod(KernelBuilder & b) override;
};

namespace streamutils {

    /**
     * Merges the contents of `i` into a single bitstream.
     * 
     * Streamset `i` may contain an arbitrary number of streams but must have
     * a field width of 1 (i.e., <i1>[N]).
     * 
     * The operation is equivalent to `or`ing together the contents of `i`.
     * 
     * Example:
     *      i[0]: 1...1...
     *      i[1]: 1.1.....
     * 
     *      out:  1.1.1...
     * 
     * This function is a wrapper for a kernel call to `CollapseStreamSet`.
     */
    StreamSet * Collapse(PipelineBuilder & P, StreamSet * i);

}

}


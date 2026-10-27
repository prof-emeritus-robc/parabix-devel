/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/streamutils/collapse.h>

#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>

using namespace llvm;

namespace kernel {

void CollapseStreamSet::generateDoBlockMethod(KernelBuilder & b) {
    const size_t n = b.getInputStreamSet("input")->getNumElements();
    Value * accum = b.loadInputStreamBlock("input", b.getInt32(0));
    for (size_t i = 1; i < n; ++i) {
        accum = b.CreateOr(accum, b.loadInputStreamBlock("input", b.getInt32(i)));
    }
    b.storeOutputStreamBlock("output", b.getInt32(0), accum);
}

CollapseStreamSet::CollapseStreamSet(LLVMTypeSystemInterface & ts, StreamSet * input, StreamSet * output)
: BlockOrientedKernel(ts, "CollapseStreamSet_x" + std::to_string(input->getNumElements()), {{"input", input}}, {{"output", output}}, {}, {}, {})
{}

namespace streamutils {

StreamSet * Collapse(PipelineBuilder & P, StreamSet * i) {
    StreamSet * const result = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CollapseStreamSet>(i, result);
    return result;
}

}
}

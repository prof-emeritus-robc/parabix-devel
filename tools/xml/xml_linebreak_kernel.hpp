/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <re/adt/re_cc.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <pablo/pablo.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>

using namespace pablo;
using namespace cc;

namespace kernel {

/**
 * Marks all unix linebreak ('\n') positions in `basis`. Always places a bit at
 * EOF regardless of whether the input file ends in a linebreak of not. 
 */
class XmlLineBreakKernel : public PabloKernel {
public:

    XmlLineBreakKernel(LLVMTypeSystemInterface & ts, StreamSet * basis, StreamSet * out)
    : PabloKernel(ts, "XmlLineBreakKernel", {{"basis", basis}}, {{"out", out, FixedRate(), Add1()}})
    {
        assert(basis->getFieldWidth() == 8 && basis->getNumElements() == 1);
        assert(out->getFieldWidth() == 1 && out->getNumElements() == 1);
    }

    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::unique_ptr<CC_Compiler> ccc;
        ccc = std::make_unique<cc::Direct_CC_Compiler>(pb.createExtract(getInputStreamVar("basis"), pb.getInteger(0)));
        PabloAST * breaks = ccc->compileCC(re::makeByte('\n'), pb);;
        PabloAST * const eofBit = pb.createAtEOF(llvm::cast<PabloAST>(pb.createOnes()));
        Var * const output = pb.createExtract(getOutputStreamVar("out"), 0);
        pb.createAssign(output, pb.createOr(breaks, eofBit));
    }
};

}

inline kernel::StreamSet * XmlLineBreaks(kernel::PipelineBuilder & P, kernel::StreamSet * basis) {
    assert(basis->getFieldWidth() == 8 && basis->getNumElements() == 1);
    auto out = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<kernel::XmlLineBreakKernel>(basis, out);
    return out;
}

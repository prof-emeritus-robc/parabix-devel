/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/divcode.h>
#include <kernel/core/kernel_builder.h>
#include <ucd/utf/utf_compiler.h>
#include <cstdio>

using namespace pablo;

namespace kernel {

static bool isU21(const StreamSet * Basis) {
    return Basis->getNumElements() == 21;
}

static std::string basisName(const StreamSet * Basis, BitMovementMode mode) {
    return isU21(Basis) ? "U21" : "U8" + BitMovementMode_string(mode);
}

std::string DivCodeKernel::alphabetName(const std::vector<UCD::codepoint_t> & blocks) {
    std::string list;
    for (UCD::codepoint_t b : blocks) {
        char hex[16];
        snprintf(hex, sizeof(hex), "%s%x", list.empty() ? "x" : "_x", b);
        list += hex;
    }
    return blocks.size() <= 8 ? list : "h" + Kernel::getStringHash(list);
}

std::string DivCodeKernel::kernelName(unsigned shift, const std::vector<UCD::codepoint_t> & blocks, const std::string & basis) {
    return std::string(shift == 5 ? "Div32Code" : shift == 6 ? "Div64Code" : "Div" + std::to_string(1u << shift) + "Code")
        + "_" + alphabetName(blocks) + "_" + basis + UTF::kernelAnnotation();
}

DivCodeKernel::DivCodeKernel(LLVMTypeSystemInterface & ts, unsigned shift, std::vector<UCD::codepoint_t> blocks,
                             StreamSet * Basis, StreamSet * Code, BitMovementMode mode)
: PabloKernel(ts, kernelName(shift, blocks, basisName(Basis, mode)), {}, {Binding{"code", Code}})
, mShift(shift)
, mBlocks(std::move(blocks))
, mMode(isU21(Basis) ? BitMovementMode::Advance : mode) {
    if (!isU21(Basis) && mMode == BitMovementMode::LookAhead) {
        mInputStreamSets.push_back(Binding{"basis", Basis, FixedRate(), LookAhead(3)});
    } else {
        mInputStreamSets.push_back(Binding{"basis", Basis});
    }
}

void DivCodeKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const unsigned bits = getOutputStreamSet(0)->getNumElements();
    //  The characters of the codes with bit j set.
    std::vector<UCD::UnicodeSet> sets(bits);
    const UCD::codepoint_t low = (1u << mShift) - 1;
    for (size_t i = 0; i < mBlocks.size(); i++) {
        const unsigned code = static_cast<unsigned>(i) + 1;
        const UCD::UnicodeSet block(mBlocks[i] << mShift, (mBlocks[i] << mShift) | low);
        for (unsigned j = 0; j < bits; j++) {
            if ((code >> j) & 1) sets[j] = sets[j] + block;
        }
    }
    UTF::UTF_Compiler compiler(getInput(0), pb, mMode);
    std::vector<Var *> targets;
    for (unsigned j = 0; j < bits; j++) targets.push_back(pb.createVar("code" + std::to_string(j), pb.createZeroes()));
    compiler.compile(targets, sets);
    for (unsigned j = 0; j < bits; j++) {
        pb.createAssign(pb.createExtract(getOutput(0), pb.getInteger(j)), pb.createInFile(targets[j]));
    }
}

}

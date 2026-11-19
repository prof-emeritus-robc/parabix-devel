/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/charclasses.h>

#include <re/toolchain/toolchain.h>
#include <kernel/core/kernel_builder.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/re_name.h>
#include <ucd/utf/utf_compiler.h>
#include <pablo/pablo.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <map>

using namespace cc;
using namespace kernel;
using namespace pablo;
using namespace re;
using namespace llvm;
using namespace UTF;

std::string makeSignature(const StreamSet * const basis, std::vector<re::CC *> & ccs) {
    std::string tmp;
    raw_string_ostream out(tmp);
    out << basis->getNumElements() << 'x' << basis->getFieldWidth();
    if (LLVM_LIKELY(!ccs.empty())) {
        char joiner = '[';
        for (const auto & set : ccs) {
            out << joiner;
            set->print(out);
            joiner = ',';
        }
        out << ']';
    }
    out << UTF::kernelAnnotation();
    out.flush();
    return tmp;
}

static llvm::cl::opt<bool> MaskedBlockSkip("masked-block-skip",
    llvm::cl::desc("Compute the streams of the kernels with a mask (class and property kernels) within an If on the "
                   "mask, skipping the blocks without mask bits (default on)"),
    llvm::cl::init(true));

std::string kernel::maskSuffix(const StreamMask & mask) {
    if (mask.mask == nullptr) return "";
    return std::string(mask.kind == MaskKind::Zeroing ? "+MaskZ" : "+MaskP") + (MaskedBlockSkip ? "If" : "");
}

void kernel::computeMasked(PabloBuilder & pb, PabloKernel & kernel, bool hasMask, const std::function<void (PabloBuilder &)> & compute) {
    if (!hasMask || !MaskedBlockSkip) {
        compute(pb);
        return;
    }
    PabloBuilder body = pb.createScope();
    pb.createIf(kernel.getInputStreamSet("mask")[0], body);
    compute(body);
}

void kernel::addMaskInputs(const StreamMask & mask, Bindings & inputs) {
    if (mask.mask == nullptr) return;
    inputs.emplace_back("mask", mask.mask);
    if (mask.kind == MaskKind::PassThru) {
        if (LLVM_UNLIKELY(mask.base == nullptr)) report_fatal_error("a PassThru mask requires a base stream set");
        inputs.emplace_back("base", mask.base);
    }
}

Binding kernel::maskedOutput(const StreamMask & mask, std::string name, StreamSet * output) {
    if (mask.mask != nullptr && mask.kind == MaskKind::PassThru) return Binding{std::move(name), output, FixedRate(), InOut("base")};
    return Binding{std::move(name), output};
}

void kernel::assignMasked(PabloBuilder & pb, PabloKernel & kernel, const std::vector<PabloAST *> & computed,
                          const std::string & name, bool hasMask, MaskKind kind) {
    Var * const out = kernel.getOutputStreamVar(name);
    PabloAST * const mask = hasMask ? kernel.getInputStreamSet("mask")[0] : nullptr;
    std::vector<PabloAST *> base;
    if (hasMask && kind == MaskKind::PassThru) base = kernel.getInputStreamSet("base");
    for (unsigned i = 0; i < computed.size(); i++) {
        PabloAST * v = computed[i];
        if (mask) {
            v = pb.createAnd(v, mask);
            if (kind == MaskKind::PassThru) v = pb.createOr(v, pb.createAnd(base[i], pb.createNot(mask)));
        }
        pb.createAssign(pb.createExtract(out, pb.getInteger(i)), v);
    }
}

CharClassesKernel::CharClassesKernel(LLVMTypeSystemInterface & ts, 
                                     std::vector<re::CC *> ccs,
                                     StreamSet * BasisBits,
                                     StreamSet * CharClasses,
                                     BitMovementMode mode)
: CharClassesKernel(ts, makeSignature(BasisBits, ccs), std::move(ccs), BasisBits, StreamMask(), CharClasses, mode) {

}

CharClassesKernel::CharClassesKernel(LLVMTypeSystemInterface & ts,
                                     std::vector<re::CC *> ccs,
                                     StreamSet * BasisBits,
                                     StreamMask mask,
                                     StreamSet * CharClasses,
                                     BitMovementMode mode)
: CharClassesKernel(ts, makeSignature(BasisBits, ccs) + maskSuffix(mask), std::move(ccs), BasisBits, mask, CharClasses, mode) {

}

CharClassesKernel::CharClassesKernel(LLVMTypeSystemInterface & ts, std::string signature, std::vector<re::CC *> && ccs, StreamSet * BasisBits,
                                     StreamMask mask, StreamSet * CharClasses, BitMovementMode mode)
: PabloKernel(ts, "cc_" + getStringHash(signature) + UTF::kernelAnnotation() +
              pablo::BitMovementMode_string(mode) + maskSuffix(mask)
, {}, {maskedOutput(mask, "charclasses", CharClasses)})
, mCCs(ccs)
, mSignature(signature)
, mBitMovement(mode)
, mHasMask(mask.mask != nullptr)
, mMaskKind(mask.kind) {
    if (mode == pablo::BitMovementMode::LookAhead) {
        mInputStreamSets.push_back(Binding{"basis", BasisBits, FixedRate(), LookAhead(3)});
    } else {
        mInputStreamSets.push_back(Binding{"basis", BasisBits});
    }
    addMaskInputs(mask, mInputStreamSets);
}

llvm::StringRef CharClassesKernel::getSignature() const {
    return mSignature;
}

void CharClassesKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    unsigned n = mCCs.size();

    std::vector<Var *> mpx;
    for (unsigned i = 0; i < n; i++) {
        mpx.push_back(pb.createVar("mpx_basis" + std::to_string(i), pb.createZeroes()));
    }
    computeMasked(pb, *this, mHasMask, [&](PabloBuilder & body) {
        UTF::UTF_Compiler unicodeCompiler(getInput(0), body, mBitMovement);
        unicodeCompiler.compile(mpx, mCCs);
    });
    std::vector<PabloAST *> classes;
    for (unsigned i = 0; i < mpx.size(); i++) classes.push_back(pb.createInFile(mpx[i]));
    assignMasked(pb, *this, classes, "charclasses", mHasMask, mMaskKind);
}

ByteClassesKernel::ByteClassesKernel(LLVMTypeSystemInterface & ts,
                                     std::vector<re::CC *>  ccs,
                                     StreamSet * inputStream,
                                     StreamSet * CharClasses)
: ByteClassesKernel(ts, makeSignature(inputStream, ccs), std::move(ccs), inputStream, CharClasses) {

}

ByteClassesKernel::ByteClassesKernel(LLVMTypeSystemInterface & ts,
                                     std::string signature,
                                     std::vector<re::CC *> && ccs,
                                     StreamSet * inputStream,
                                     StreamSet * CharClasses)
: PabloKernel(ts, "bcc_" + getStringHash(signature)
, {Binding{"basis", inputStream}}, {Binding{"charclasses", CharClasses}})
, mCCs(ccs)
, mSignature(signature) {

}

StringRef ByteClassesKernel::getSignature() const {
    return mSignature;
}

void ByteClassesKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::unique_ptr<cc::CC_Compiler> ccc;

    auto basisBits = getInputStreamSet("basis");
    if (basisBits.size() == 1) {
        ccc = std::make_unique<cc::Direct_CC_Compiler>(basisBits[0]);
    } else {
        ccc = std::make_unique<cc::Parabix_CC_Compiler_Builder>(basisBits);
    }
    unsigned n = mCCs.size();

    std::vector<PabloAST *> byte_class;
    for (unsigned i = 0; i < n; i++) {
        byte_class.push_back(ccc->compileCC(mCCs[i], pb));
    }
    for (unsigned i = 0; i < byte_class.size(); i++) {
        Extract * const r = pb.createExtract(getOutput(0), pb.getInteger(i));
        pb.createAssign(r, pb.createInFile(byte_class[i]));
    }
}

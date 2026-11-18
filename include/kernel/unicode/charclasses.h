/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/toolchain/pablo_toolchain.h>
#include <pablo/pablo.h>
#include <re/alphabet/alphabet.h>

namespace kernel { class KernelBuilder; }
namespace IDISA { class IDISA_Builder; }
namespace re { class RE; class CC; }
namespace kernel {

//  How a kernel computing streams from a basis treats the positions outside a
//  mask (the 0 bits of a Mask input), where its streams are not computed:
//  Zeroing, its outputs are 0 there; PassThru, its outputs are those of a Base
//  stream set there, which the kernel modifies in place (an InOut binding: Base
//  is no longer available after it).  At the mask's 1 bits, the outputs are
//  the streams computed.
enum class MaskKind {Zeroing, PassThru};

//  The streams of a mask input, and how it applies (see MaskKind); with no
//  mask, the streams are computed at every position.
struct StreamMask {
    StreamSet * mask = nullptr;
    MaskKind kind = MaskKind::Zeroing;
    StreamSet * base = nullptr;         // with PassThru
};

class CharClassesKernel final : public pablo::PabloKernel {
public:
    CharClassesKernel(LLVMTypeSystemInterface & ts, 
                      std::vector<re::CC *> ccs,
                      StreamSet * BasisBits,
                      StreamSet * CharClasses,
                      pablo::BitMovementMode mode = pablo::BitMovementMode::Advance);
    //  The classes within a mask (see MaskKind).
    CharClassesKernel(LLVMTypeSystemInterface & ts,
                      std::vector<re::CC *> ccs,
                      StreamSet * BasisBits,
                      StreamMask mask,
                      StreamSet * CharClasses,
                      pablo::BitMovementMode mode = pablo::BitMovementMode::Advance);
protected:
    bool hasSignature() const override { return true; }
    llvm::StringRef getSignature() const override;
    CharClassesKernel(LLVMTypeSystemInterface & ts, 
                      std::string signature,
                      std::vector<re::CC *> && ccs,
                      StreamSet * BasisBits,
                      StreamMask mask,
                      StreamSet * CharClasses,
                      pablo::BitMovementMode mode = pablo::BitMovementMode::Advance);
    void generatePabloMethod() override;
protected:
    const std::vector<re::CC *> mCCs;
    const std::string mSignature;
    pablo::BitMovementMode mBitMovement;
    const bool mHasMask;
    const MaskKind mMaskKind;
};

//  The suffix of the name of a kernel with a mask (see MaskKind): none without.
std::string maskSuffix(const StreamMask & mask);

//  The outputs of a kernel with a mask, from the streams computed (see
//  MaskKind), assigned to its output stream set "name"; the mask is the input
//  stream set "mask", and the base the input stream set "base".
void assignMasked(pablo::PabloBuilder & pb, pablo::PabloKernel & kernel, const std::vector<pablo::PabloAST *> & computed,
                  const std::string & name, bool hasMask, MaskKind kind);

//  The bindings of a kernel's mask: its inputs ("mask", and "base" with
//  PassThru), and its output stream set (in place of the base, with PassThru).
void addMaskInputs(const StreamMask & mask, Bindings & inputs);
Binding maskedOutput(const StreamMask & mask, std::string name, StreamSet * output);


class ByteClassesKernel final : public pablo::PabloKernel {
public:
    ByteClassesKernel(LLVMTypeSystemInterface & ts, std::vector<re::CC *> ccs, StreamSet * inputStream, StreamSet * CharClasses);
    bool hasSignature() const override { return true; }
    llvm::StringRef getSignature() const override;
protected:
    ByteClassesKernel(LLVMTypeSystemInterface & ts, std::string signature, std::vector<re::CC *> && ccs, StreamSet * inputStream, StreamSet * CharClasses);
    void generatePabloMethod() override;
protected:
    const std::vector<re::CC *> mCCs;
    const std::string mSignature;
};


}

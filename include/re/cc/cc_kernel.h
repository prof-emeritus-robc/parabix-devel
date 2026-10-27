/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once
#include <pablo/compiler/pablo_kernel.h>

// #include <kernel/util/callback.h>

namespace IDISA { class IDISA_Builder; }
namespace re { class CC; }

namespace kernel {

struct CharacterClassesSignature {
    CharacterClassesSignature(const std::vector<re::CC *> & ccs, StreamSet * source, Scalar * signal);
protected:
    const std::string mSignature;
};

class CharacterClassKernelBuilder final : public CharacterClassesSignature, public pablo::PabloKernel {
public:    
    CharacterClassKernelBuilder(LLVMTypeSystemInterface & ts, std::vector<re::CC *> charClasses, StreamSet * source, StreamSet * ccStream, Scalar * signalNullObject = nullptr);
protected:
    bool hasSignature() const override { return true; }
    llvm::StringRef getSignature() const override;
    void generatePabloMethod() override;
    Bindings makeInputScalarBindings(Scalar * signalNullObject);
private:
    const std::vector<re::CC *> mCharClasses;
    bool mAbortOnNull;
};

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/core/kernel.h>
#include <kernel/core/block_kernel_compiler.h>
#include <codegen/LLVMTypeSystemInterface.h>

namespace kernel {

// TODO: Break the BlockOrientedKernel into two classes, one with an explicit DoFinal block and another that
// calls the DoBlock method with optional preamble and postamble hooks. By doing so, we can remove the indirect
// branches (or function calls) from the following kernel and simplify the cognitive load for the kernel
// programmer. This is less general than the current method but no evidence that being able to reenter the
// DoBlock method multiple times from the DoFinal block would ever be useful.

#define COMPILER (static_cast<BlockKernelCompiler *>(b.getCompiler()))

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief instantiateKernelCompiler
 ** ------------------------------------------------------------------------------------------------------------- */
std::unique_ptr<KernelCompiler> BlockOrientedKernel::instantiateKernelCompiler(KernelBuilder & /* b */) {
    return std::make_unique<BlockKernelCompiler>(const_cast<BlockOrientedKernel *>(this));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateMultiBlockLogic
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockOrientedKernel::generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfBlocks) {
    COMPILER->generateMultiBlockLogic(b, numOfBlocks);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief RepeatDoBlockLogic
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockOrientedKernel::RepeatDoBlockLogic(KernelBuilder & b) {
    COMPILER->generateDefaultFinalBlockMethod(b);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateFinalBlockMethod
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockOrientedKernel::generateFinalBlockMethod(KernelBuilder & b, llvm::Value * /* remainingItems */) {
    COMPILER->generateDefaultFinalBlockMethod(b);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setProvisionalLookAheadStride
 ** ------------------------------------------------------------------------------------------------------------- */
void BlockOrientedKernel::setProvisionalLookAheadStride() {
    if (!hasAttribute(Attribute::KindId::ProvisionalLookAheadStride)) {
        addAttribute(ProvisionalLookAheadStride());
        mKernelName += "+PLS";
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief meetsProvisionalLookAheadStrideRequirements
 ** ------------------------------------------------------------------------------------------------------------- */
bool BlockOrientedKernel::meetsProvisionalLookAheadStrideRequirements(std::string * reason) const {
    auto fail = [&](const std::string & r) {
        if (reason) {
            *reason = r;
        }
        return false;
    };
    if (mInputStreamSets.empty()) {
        return fail("at least one stream input");
    }
    auto checkBinding = [&](const Binding & binding) -> bool {
        const ProcessingRate & rate = binding.getRate();
        if (!rate.isFixed() || rate.getRate() != ProcessingRate::Rational{1}) {
            return fail("every stream input and output to be FixedRate(1); " + binding.getName() + " is not");
        }
        for (const Attribute & attr : binding.getAttributes()) {
            switch (attr.getKind()) {
                case Attribute::KindId::Add:
                case Attribute::KindId::Truncate:
                case Attribute::KindId::AddCarry:
                case Attribute::KindId::Delayed:
                case Attribute::KindId::Deferred:
                case Attribute::KindId::BlockSize:
                case Attribute::KindId::ManagedBuffer:
                case Attribute::KindId::SharedManagedBuffer:
                case Attribute::KindId::ReturnedBuffer:
                    return fail("stream bindings without rate-changing or managed-buffer attributes; " + binding.getName() + " has one");
                case Attribute::KindId::InOut:
                    // the provisional stride would overwrite input data that the next call reprocesses
                    return fail("outputs that do not share a buffer with an input; " + binding.getName() + " is InOut");
                default: break;
            }
        }
        return true;
    };
    for (const Binding & input : mInputStreamSets) {
        if (!checkBinding(input)) return false;
    }
    for (const Binding & output : mOutputStreamSets) {
        if (!checkBinding(output)) return false;
    }
    for (const Attribute & attr : getAttributes()) {
        switch (attr.getKind()) {
            case Attribute::KindId::CanTerminateEarly:
            case Attribute::KindId::MustExplicitlyTerminate:
            case Attribute::KindId::MayFatallyTerminate:
            case Attribute::KindId::InternallySynchronized:
                return fail("a kernel that does not terminate early and is not internally synchronized");
            default: break;
        }
    }
    return true;
}

// CONSTRUCTOR
BlockOrientedKernel::BlockOrientedKernel(LLVMTypeSystemInterface & ts,
    std::string && kernelName,
    Bindings && stream_inputs,
    Bindings && stream_outputs,
    Bindings && scalar_parameters,
    Bindings && scalar_outputs,
    InternalScalars && internal_scalars,
    const unsigned flags)
: MultiBlockKernel(ts,
    TypeId::BlockOriented,
    std::move(kernelName),
    std::move(stream_inputs),
    std::move(stream_outputs),
    std::move(scalar_parameters),
    std::move(scalar_outputs),
    std::move(internal_scalars),
    flags) {

}


}

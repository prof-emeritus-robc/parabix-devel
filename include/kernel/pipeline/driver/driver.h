#pragma once

#include <codegen/FunctionTypeBuilder.h>
#include <codegen/LLVMTypeSystemInterface.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>
#include <llvm/ADT/StringSet.h>
#include <kernel/core/kernel.h>
#include <kernel/core/relationship.h>
#include <util/slab_allocator.h>
#include <llvm/IR/Constants.h>
#include <kernel/illustrator/illustrator.h>
#include <string>
#include <vector>
#include <memory>

namespace llvm { class Function; }
namespace kernel { class KernelBuilder; }
namespace kernel { class PipelineAnalysis; }
namespace kernel { class PipelineBuilder; }
namespace kernel { class ProgramBuilder; }
namespace llvm { class TargetMachine; }
namespace kernel {template<typename ... Args> class TypedProgramBuilder; }

class CBuilder;
class ParabixObjectCache;

class BaseDriver : public LLVMTypeSystemInterface {
    friend class CBuilder;
    friend class kernel::PipelineAnalysis;
    friend class kernel::PipelineBuilder;
    friend class kernel::ProgramBuilder;
    friend class kernel::Kernel;
    template<typename ... Args> friend class kernel::TypedProgramBuilder;

public:

    using Kernel = kernel::Kernel;
    using Relationship = kernel::Relationship;
    using Bindings = kernel::Bindings;
    using KernelSet = std::vector<std::unique_ptr<Kernel>>;

    void addKernel(not_null<Kernel *> kernel);

    virtual bool hasExternalFunction(const llvm::StringRef functionName) const = 0;

    virtual void generateUncachedKernels() = 0;

    virtual void * finalizeObject(kernel::Kernel * pipeline) = 0;

    virtual ~BaseDriver();

    const llvm::LLVMContext & getContext() const final {
        return *mContext;
    }

    llvm::LLVMContext & getContext() final {
        return *mContext;
    }

    bool getPreservesKernels() const {
        return mPreservesKernels;
    }

    void setPreserveKernels(const bool value = true) {
        mPreservesKernels = value;
    }

    unsigned getBitBlockWidth() const final;

    virtual void addCachedObjectFile(llvm::Module * module, std::unique_ptr<llvm::MemoryBuffer> && object) = 0;

//    llvm::TargetMachine * getTargetMachine() {
//        return mTarget;
//    }

    const std::unique_ptr<kernel::KernelBuilder> & getMainBuilder() const {
        return mBuilder;
    }

protected:

    kernel::StreamSet * CreateStreamSet(const unsigned NumElements = 1, const unsigned FieldWidth = 1) noexcept;

    kernel::RepeatingStreamSet * CreateRepeatingStreamSet(const unsigned FieldWidth, std::vector<std::vector<uint64_t> > &&stringSet, const bool isDynamic = true) noexcept;

    kernel::TruncatedStreamSet * CreateTruncatedStreamSet(const kernel::StreamSet * data) noexcept;

    kernel::RepeatingStreamSet * CreateUnalignedRepeatingStreamSet(const unsigned FieldWidth, std::vector<std::vector<uint64_t> > &&stringSet, const bool isDynamic = true) noexcept;

    kernel::Scalar * CreateScalar(not_null<llvm::Type *> scalarType) noexcept;

    kernel::Scalar * CreateConstant(not_null<llvm::Constant *> value) noexcept;

    kernel::Scalar * CreateCommandLineScalar(kernel::CommandLineScalarType type) noexcept;

    llvm::VectorType * getBitBlockType() const final;

    llvm::VectorType * getStreamTy(const unsigned FieldWidth = 1) final;

    llvm::ArrayType * getStreamSetTy(const unsigned NumElements = 1, const unsigned FieldWidth = 1) final;

protected:

    BaseDriver(std::string && moduleName);

    template <typename ExternalFunctionType>
    void LinkFunction(not_null<Kernel *> kernel, llvm::StringRef name, ExternalFunctionType & functionPtr);

    virtual llvm::Function * addLinkFunction(kernel::Kernel * const kernel, llvm::StringRef name, llvm::FunctionType * type, void * functionPtr) = 0;

protected:

    llvm::LLVMContext * const                               mContext;

    llvm::Module * const                                    mMainModule;

    std::unique_ptr<kernel::KernelBuilder>                  mBuilder;
    std::unique_ptr<ParabixObjectCache>                     mObjectCache;

    llvm::StringSet<>                                       mCompiledIdentifiers;

    bool                                                    mPreservesKernels = false;
    KernelSet                                               mUncachedKernel;
    KernelSet                                               mCachedKernel;
    KernelSet                                               mCompiledKernel;
    KernelSet                                               mPreservedKernel;
    SlabAllocator<>                                         mAllocator;
};

template <typename ExternalFunctionType>
void BaseDriver::LinkFunction(not_null<Kernel *> kernel, llvm::StringRef name, ExternalFunctionType & functionPtr) {
    auto * const type = FunctionTypeBuilder<ExternalFunctionType>::get(*mContext);
    assert ("FunctionTypeBuilder did not resolve a function type." && type);
    addLinkFunction(kernel.get(), name, type, reinterpret_cast<void *>(functionPtr));
}


#pragma once

#include <codegen/FunctionTypeBuilder.h>
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

class BaseDriver : public FunctionLinkCallback {
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

    virtual void generateUncachedKernels() = 0;

    virtual void * finalizeObject(kernel::Kernel * pipeline) = 0;

    virtual ~BaseDriver();

    bool getPreservesKernels() const {
        return mPreservesKernels;
    }

    void setPreserveKernels(const bool value = true) {
        mPreservesKernels = value;
    }

    virtual void addCachedObjectFile(llvm::Module * module, std::unique_ptr<llvm::MemoryBuffer> && object) = 0;

    const std::unique_ptr<kernel::KernelBuilder> & getMainBuilder() const {
        return mBuilder;
    }

    llvm::LLVMContext & getContext() {
        return *mContext;
    }

    const llvm::LLVMContext & getContext() const {
        return *mContext;
    }

protected:

    kernel::StreamSet * CreateStreamSet(const unsigned NumElements = 1, const unsigned FieldWidth = 1) noexcept;

    kernel::RepeatingStreamSet * CreateRepeatingStreamSet(const unsigned FieldWidth, std::vector<std::vector<uint64_t> > &&stringSet, const bool isDynamic = true) noexcept;

    kernel::TruncatedStreamSet * CreateTruncatedStreamSet(const kernel::StreamSet * data) noexcept;

    kernel::RepeatingStreamSet * CreateUnalignedRepeatingStreamSet(const unsigned FieldWidth, std::vector<std::vector<uint64_t> > &&stringSet, const bool isDynamic = true) noexcept;

    kernel::Scalar * CreateScalar(not_null<llvm::Type *> scalarType) noexcept;

    kernel::Scalar * CreateConstant(not_null<llvm::Constant *> value) noexcept;

    kernel::Scalar * CreateCommandLineScalar(kernel::CommandLineScalarType type) noexcept;

protected:

    BaseDriver(std::string && moduleName);

protected:

    llvm::LLVMContext * const                               mContext;

    llvm::Module * const                                    mMainModule;

    std::unique_ptr<kernel::KernelBuilder>                  mBuilder;
    std::unique_ptr<ParabixObjectCache>                     mObjectCache;

    bool                                                    mPreservesKernels = false;
    KernelSet                                               mUncachedKernel;
    KernelSet                                               mCachedKernel;
    KernelSet                                               mCompiledKernel;
    KernelSet                                               mPreservedKernel;
    SlabAllocator<>                                         mAllocator;
};


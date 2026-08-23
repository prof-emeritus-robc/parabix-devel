#pragma once
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/LegacyPassManager.h>
#include <kernel/pipeline/driver/driver.h>
#include <toolchain/toolchain.h>

namespace llvm { class raw_fd_ostream; }
namespace llvm { class ModulePass; }
namespace kernel { class KernelBuilder; }

namespace {
class CPUDriverContextPool;
class CPUDriverTaskDispatcher;
}

class CPUDriver final : public BaseDriver {

    struct LinkedFunction {
        const kernel::Kernel * const Target;
        llvm::Function * const FunctionDecl;

        LinkedFunction(const kernel::Kernel * const target, llvm::Function * decl)
        : Target(target), FunctionDecl(decl) { }
    };

    friend class CPUDriverKernelCompiler;
    friend class KernelGenerationMU;

    using LinkedFunctionVector = llvm::SmallVector<LinkedFunction, 16>;

public:

    CPUDriver(std::string && moduleName);

    ~CPUDriver();

    void generateUncachedKernels() override;

    void * finalizeObject(kernel::Kernel * const pipeline) override;

    llvm::ModulePass * createTracePass(kernel::KernelBuilder * kb, llvm::StringRef to_trace);

    void addCachedObjectFile(llvm::Module * module, std::unique_ptr<llvm::MemoryBuffer> && object) final;

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) final;

    bool HasExternalFunction(llvm::StringRef unmangledName) const final;


private:

    void preparePassManager();

    void linkAllExternalSymbols();

private:
    std::unique_ptr<llvm::raw_fd_ostream>                   mUnoptimizedIROutputStream;
    std::unique_ptr<llvm::raw_fd_ostream>                   mIROutputStream;
    std::unique_ptr<llvm::raw_fd_ostream>                   mASMOutputStream;
    std::unique_ptr<llvm::orc::LLJIT>                       mEngine;
    std::unique_ptr<CPUDriverContextPool>                   mContextPool;
    CPUDriverTaskDispatcher *                               mTaskDispatcher = nullptr;
    std::unique_ptr<llvm::orc::SymbolMap>                   mAllLinkedSymbols;
    std::unique_ptr<llvm::orc::SymbolLookupSet>             mSymbolLookupSet;
    std::unique_ptr<llvm::orc::SymbolDependenceMap>         mPriorSymbolLayer;
};


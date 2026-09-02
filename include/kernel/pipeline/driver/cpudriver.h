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
class CPUDriverCompiler;
}

class CPUDriver final : public BaseDriver {

    friend class CPUDriverKernelCompiler;

public:

    CPUDriver(std::string && moduleName);

    ~CPUDriver();

    void generateUncachedKernels() override;

    void * finalizeObject(kernel::Kernel * const pipeline) override;

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) final;

    bool HasExternalFunction(llvm::StringRef unmangledName) const final;

private:

    std::unique_ptr<llvm::orc::LLJIT>                       mEngine;
    std::unique_ptr<CPUDriverCompiler>                      mCPUDriverCompiler;
    std::unique_ptr<llvm::orc::SymbolMap>                   mAllLinkedSymbols;
};


#pragma once
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <kernel/pipeline/driver/driver.h>
#include <toolchain/toolchain.h>

namespace llvm { class raw_fd_ostream; }
namespace llvm { class ModulePass; }
namespace kernel { class KernelBuilder; }

#include <llvm/IR/LegacyPassManager.h>
class CPUDriver final : public BaseDriver {

    struct LinkedFunction {
        const kernel::Kernel * const Target;
        llvm::Function * const FunctionDecl;

        LinkedFunction(const kernel::Kernel * const target, llvm::Function * decl)
        : Target(target), FunctionDecl(decl) { }
    };

    friend class CPUDriverIRCompiler;

public:

    CPUDriver(std::string && moduleName);

    ~CPUDriver();

    void generateUncachedKernels() override;

    void * finalizeObject(kernel::Kernel * const pipeline) override;

    bool hasExternalFunction(const llvm::StringRef functionName) const override;

    llvm::ModulePass * createTracePass(kernel::KernelBuilder * kb, llvm::StringRef to_trace);

    void addCachedObjectFile(llvm::Module * module, std::unique_ptr<llvm::MemoryBuffer> && object) final;

private:

    void preparePassManager();

    void linkAllExternalSymbols();

protected:

    llvm::Function * addLinkFunction(kernel::Kernel * const kernel, llvm::StringRef name, llvm::FunctionType * type, void * functionPtr) override;

private:
    std::unique_ptr<llvm::raw_fd_ostream>                   mUnoptimizedIROutputStream;
    std::unique_ptr<llvm::raw_fd_ostream>                   mIROutputStream;
    std::unique_ptr<llvm::raw_fd_ostream>                   mASMOutputStream;

    std::vector<llvm::orc::ThreadSafeContext>               mContexts;
    std::unique_ptr<llvm::orc::SymbolLookupSet>             mSymbolLookupSet;
    std::unique_ptr<llvm::orc::SymbolMap>                   mAllLinkedSymbols;
    std::unique_ptr<llvm::orc::LLJIT>                       mEngine;
    llvm::SmallVector<LinkedFunction, 16>                   mLinkedFunctions;
};


#pragma once

#include <kernel/pipeline/driver/cpu_jit_backend.h>
#include <llvm/Support/CodeGen.h>
#include <memory>
#include <utility>
#include <vector>

namespace llvm {
class ExecutionEngine;
class TargetMachine;
class Module;
}

class CPUDriver;
class MCJITObjectCacheAdapter;

// MCJITBackend: the classic single-threaded MCJIT backend, selected via --use-mcjit.
// Modeled on the pre-ORC driver's flow (one shared LLVMContext, one driver-owned
// TargetMachine, explicit module add/finalize/remove), adapted to call the same
// (backend-agnostic) phased Kernel API -- declareStateTypes/generateKernel/
// addOptimizationPasses+BaseDriver::runAllOptimizationPasses -- that OrcJITBackend uses.
class MCJITBackend final : public CPUJITBackend {
public:

    explicit MCJITBackend(CPUDriver & driver);

    ~MCJITBackend() override;

    void generateUncachedKernels() override;

    void * finalizeObject(kernel::Kernel * pipeline) override;

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) override;

    bool HasExternalFunction(llvm::StringRef unmangledName) const override;

private:

    CPUDriver &                                              mDriver;
    llvm::TargetMachine *                                     mTarget = nullptr; // owned by mEngine
    std::unique_ptr<llvm::ExecutionEngine>                    mEngine;
    std::unique_ptr<MCJITObjectCacheAdapter>                  mObjectCacheAdapter;

    // Kernel:: no longer tracks its own Module*, so this backend must remember, between
    // generateUncachedKernels() and finalizeObject(), which Module belongs to which Kernel.
    std::vector<std::pair<kernel::Kernel *, llvm::Module *>>  mCompiledModules;
};

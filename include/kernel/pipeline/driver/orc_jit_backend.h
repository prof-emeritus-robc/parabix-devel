#pragma once

#include <kernel/pipeline/driver/cpu_jit_backend.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <memory>

namespace llvm {
namespace orc { class LLJIT; }
}

class CPUDriver;
class CPUDriverCompiler;

// OrcJITBackend: the default, multi-threaded ORC JIT backend. This is a mechanical
// extraction of what used to be CPUDriver's own members/methods directly -- no logic
// changes versus the pre-extraction implementation, just BaseDriver field accesses
// rewritten through mDriver (OrcJITBackend is a composed object, not a BaseDriver
// subclass, so it needs the friend-class access granted in driver.h).
class OrcJITBackend final : public CPUJITBackend {
public:

#if defined(__clang__) || defined(__GNUC__)
    __attribute__((no_sanitize_address))
#endif
    explicit OrcJITBackend(CPUDriver & driver);

    ~OrcJITBackend() override;

    void generateUncachedKernels() override;

    void * finalizeObject(kernel::Kernel * const pk) override;

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) override;

    bool HasExternalFunction(llvm::StringRef functionName) const override;

private:

    CPUDriver &                                             mDriver;
    std::unique_ptr<llvm::orc::LLJIT>                       mEngine;
    std::unique_ptr<CPUDriverCompiler>                      mCPUDriverCompiler;
    std::unique_ptr<llvm::orc::SymbolMap>                   mAllLinkedSymbols;
};

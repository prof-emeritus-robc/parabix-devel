#pragma once

#include <kernel/pipeline/driver/driver.h>
#include <kernel/pipeline/driver/cpu_jit_backend.h>
#include <memory>

class CPUDriver final : public BaseDriver {

public:

    CPUDriver(std::string && moduleName);

    ~CPUDriver();

    void generateUncachedKernels() override;

    void * finalizeObject(kernel::Kernel * const pipeline) override;

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) final;

    bool HasExternalFunction(llvm::StringRef unmangledName) const final;

private:

    std::unique_ptr<CPUJITBackend>                          mBackend;
};

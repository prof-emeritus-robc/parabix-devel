#include <kernel/pipeline/driver/cpudriver.h>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/mcjit_backend.h>
#include <kernel/pipeline/driver/orc_jit_backend.h>

CPUDriver::CPUDriver(std::string && moduleName)
: BaseDriver(std::move(moduleName)) {
    if (codegen::UseMCJIT) {
        mBackend = std::make_unique<MCJITBackend>(*this);
    } else {
        mBackend = std::make_unique<OrcJITBackend>(*this);
    }
}

CPUDriver::~CPUDriver() {
}

void CPUDriver::generateUncachedKernels() {
    mBackend->generateUncachedKernels();
}

void * CPUDriver::finalizeObject(kernel::Kernel * const pipeline) {
    return mBackend->finalizeObject(pipeline);
}

llvm::Function * CPUDriver::LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
    return mBackend->LinkFunction(unmangledName, functionType, functionPointer);
}

bool CPUDriver::HasExternalFunction(llvm::StringRef unmangledName) const {
    return mBackend->HasExternalFunction(unmangledName);
}

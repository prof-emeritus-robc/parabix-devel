#pragma once

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class FunctionType;
}

namespace kernel {
class Kernel;
}

// Internal strategy interface selected at runtime by CPUDriver, based on codegen::UseMCJIT,
// between the default multi-threaded ORC JIT backend (OrcJITBackend, cpudriver.cpp) and the
// classic single-threaded MCJIT backend (MCJITBackend, mcjit_backend.cpp).
class CPUJITBackend {
public:

    virtual void generateUncachedKernels() = 0;

    virtual void * finalizeObject(kernel::Kernel * pipeline) = 0;

    virtual llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) = 0;

    virtual bool HasExternalFunction(llvm::StringRef unmangledName) const = 0;

    virtual ~CPUJITBackend() = default;
};

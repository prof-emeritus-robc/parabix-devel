/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <kernel/pipeline/driver/driver.h>

class NVPTXDriver final : public BaseDriver {
    friend class CBuilder;
public:
    NVPTXDriver(std::string && moduleName);

    ~NVPTXDriver();

    void addKernel(Kernel * const kernel) override { }

    void generateUncachedKernels() { }

    void * finalizeObject(kernel::PipelineKernel * pipeline) override;

    bool hasExternalFunction(const llvm::StringRef /* functionName */) const override { return false; }

    llvm::orc::SymbolStringPtr declareFunctionSymbol(llvm::Function * function) const final { return llvm::orc::SymbolStringPtr{}; }

    void addCachedObjectFile(llvm::Module * module, std::unique_ptr<llvm::MemoryBuffer> && object) final {}

protected:

    NVPTXDriver(std::string && moduleName);

protected:

    llvm::Function * addLinkFunction(llvm::Module * mod, llvm::StringRef name, llvm::FunctionType * type, void * functionPtr) const override;

};


#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/DynamicLibrary.h>           // for LoadLibraryPermanently
#include <llvm/ExecutionEngine/ExecutionEngine.h>  // for EngineBuilder
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/InitializePasses.h>                 // for initializeCodeGencd .
#include <llvm/PassRegistry.h>                     // for PassRegistry
#include <llvm/Support/CodeGen.h>                  // for Level, Level::None
#include <llvm/Support/Compiler.h>                 // for LLVM_UNLIKELY
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Timer.h>
#include <objcache/object_cache.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <llvm/IR/Verifier.h>
#include "llvm/IR/Mangler.h"
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/ADT/StringExtras.h>

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/PassTimingInfo.h>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#ifndef NDEBUG
#define IN_DEBUG_MODE true
#else
#define IN_DEBUG_MODE false
#endif

#if defined(__clang__) || defined (__GNUC__)
    #define ATTRIBUTE_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#else
    #define ATTRIBUTE_NO_SANITIZE_ADDRESS
#endif

using namespace llvm;
using namespace llvm::orc;
using namespace kernel;

using AttrId = kernel::Attribute::KindId;

ATTRIBUTE_NO_SANITIZE_ADDRESS
CPUDriver::CPUDriver(std::string && moduleName)
: BaseDriver(std::move(moduleName))
, mUnoptimizedIROutputStream{}
, mIROutputStream{}
, mASMOutputStream{}
, mEngine(nullptr)
, mTarget(nullptr) {

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    #if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(19, 0, 0)
    StringMap<bool> features;
    sys::getHostCPUFeatures(features);
    #else
    const StringMap<bool> features = sys::getHostCPUFeatures();
    #endif

    std::vector<std::string> attrs;
    for (auto & flag : features) {
        if (flag.second) {
            attrs.push_back("+" + flag.first().str());
        }
    }

    std::string errMessage;
    auto TripleStr = sys::getDefaultTargetTriple();
    auto CPUStr = sys::getHostCPUName();
    const Target *TheTarget = TargetRegistry::lookupTarget(TripleStr, errMessage);

    if (!TheTarget) {
        throw std::runtime_error("Could not lookup target for triple " + TripleStr + ": " + errMessage);
    }

    TargetOptions Options = codegen::target_Options;

    mTarget.reset(TheTarget->createTargetMachine(
        TripleStr,               // Target Triple
        CPUStr,                  // Host CPU Name
        llvm::join(attrs, ","),  // Flattened features string list
        Options,                 // Pipeline CodeGen target configurations
        std::nullopt,            // Fixes error! Expresses 'Default' relocation model
        CodeModel::Small,        // Standard Code Model
        codegen::BackEndOptLevel // Optimization configurations
    ));

    if (mTarget == nullptr) {
        throw std::runtime_error("Could not allocate TargetMachine wrapper profile structure layout");
    }

    auto JTMB = orc::JITTargetMachineBuilder(mTarget->getTargetTriple());
    JTMB.setCPU(CPUStr.str())
        .addFeatures(attrs)
        .setOptions(Options)
        .setCodeGenOptLevel(codegen::BackEndOptLevel);

    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));

    // Safely route the compilation process through your customized Parabix caching system
    if (mObjectCache) {
        Builder.setCompileFunctionCreator([this](llvm::orc::JITTargetMachineBuilder InnerJTMB) 
            -> Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
                auto TM = InnerJTMB.createTargetMachine();
                if (!TM) return TM.takeError();

                // TMOwningSimpleCompiler intercepts compilation and automatically coordinates
                // with your mObjectCache->getObject() and notifyObjectCompiled() methods!
                return std::make_unique<llvm::orc::TMOwningSimpleCompiler>(
                    std::move(*TM),
                    mObjectCache.get()
                );
        });
    }

    auto JITExpect = Builder.create();
    if (!JITExpect) {
        std::string errMessage;
        handleAllErrors(JITExpect.takeError(), [&](const ErrorInfoBase &EIB) {
            errMessage += EIB.message();
        });
        throw std::runtime_error("Could not create LLJIT instance: " + errMessage);
    }
    mEngine = std::move(*JITExpect);

    auto &MainJD = mEngine->getMainJITDylib();
    MainJD.addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            mEngine->getDataLayout().getGlobalPrefix()
        ))
    );

    // 5. Establish Layout Attributes matching your primary Module Context references
    const DataLayout DL = mEngine->getDataLayout();
    auto triple = mTarget->getTargetTriple().getTriple();
    mMainModule->setTargetTriple(triple);
    mMainModule->setDataLayout(DL);

    mBuilder.reset(IDISA::GetIDISA_Builder(*mContext, features));
    mBuilder->setDriver(*this);
    mBuilder->setModule(mMainModule);
}

Function * CPUDriver::addLinkFunction(Module * mod, llvm::StringRef name, FunctionType * type, void * functionPtr) const {
    if (LLVM_UNLIKELY(mod == nullptr)) {
        report_fatal_error("addLinkFunction(" + name + ") cannot be called until after addKernel");
    }
    Function * f = mod->getFunction(name);
    if (LLVM_UNLIKELY(f == nullptr)) {
        f = Function::Create(type, Function::ExternalLinkage, name, mod);

        // Define symbol mapping inside the primary ORC library environment
        auto &MainJD = mEngine->getMainJITDylib();
        Mangler Mangle;
        std::string MangledName;
        raw_string_ostream MangledStream(MangledName);
        Mangle.getNameWithPrefix(MangledStream, name, mEngine->getDataLayout());
        MangledStream.flush();

        auto InternedSymbol = mEngine->getExecutionSession().intern(MangledName);

        auto SymbolAddress = orc::ExecutorAddr::fromPtr(functionPtr);

        // Attempt to define the absolute symbol directly into MainJD
        auto Err = MainJD.define(orc::absoluteSymbols({
            { InternedSymbol, { SymbolAddress, JITSymbolFlags::Exported } }
        }));

        if (Err) {
            // Check if the error is due to a duplicate definition
            bool IsDuplicate = false;
            handleAllErrors(std::move(Err), [&](const orc::DuplicateDefinition &DD) {
                // This means the symbol was already registered by a prior kernel pass.
                // We mark it as a duplicate and safely drop the error.
                IsDuplicate = true;
            }, [&](const ErrorInfoBase &EIB) {
                // If it's a completely different linkage error, throw it up to the runtime
                report_fatal_error(Twine("Failed to map external helper linkage symbol '") +
                                   name + "': " + EIB.message());
            });
        }
    } else if (LLVM_UNLIKELY(f->getType() != type->getPointerTo())) {
        report_fatal_error("Cannot link " + name + ": a function with a different signature already exists with that name in " + mod->getName());
    }
    return f;
}

void CPUDriver::generateUncachedKernels() {
    if (mUncachedKernel.empty()) return;

    // TODO: we may be able to reduce unnecessary optimization work by having kernel specific optimization passes.

    // NOTE: we currently require DCE and Mem2Reg for each kernel to eliminate any unnecessary scalar -> value
    // mappings made by the base KernelCompiler. That could be done in a more focused manner, however, as each
    // mapping is known.

    mCachedKernel.reserve(mUncachedKernel.size());
    for (unsigned i = 0; i < mUncachedKernel.size(); ++i) {
        auto & kernel = mUncachedKernel[i];
        NamedRegionTimer T(kernel->getSignature(), kernel->getName(),
                           "kernel", "Kernel Generation",
                           codegen::TimeKernelsIsEnabled);
        kernel->generateKernel(getBuilder());
        Module * const module = kernel->getModule(); assert (module);
        module->setTargetTriple(mMainModule->getTargetTriple());
        module->setDataLayout(mMainModule->getDataLayout());
        mCachedKernel.emplace_back(kernel.release());
    }

    mUncachedKernel.clear();

    llvm::reportAndResetTimings();
    llvm::PrintStatistics();
}

void * CPUDriver::finalizeObject(kernel::Kernel * const pk) {

    using ModuleSet = llvm::SmallVector<Module *, 32>;

    ModuleSet Infrequent;
    ModuleSet Normal;

    for (const auto & kernel : mCompiledKernel) {
        kernel->ensureLoaded();
    }

    for (const auto & kernel : mCachedKernel) {
        if (LLVM_UNLIKELY(kernel->getModule() == nullptr)) {
            report_fatal_error(llvm::StringRef(kernel->getName()) + " was neither loaded from cache nor generated prior to finalizeObject");
        }
        Module * const m = kernel->getModule();
        assert ("cached kernel has no module?" && m);
        if (LLVM_UNLIKELY(kernel->hasAttribute(AttrId::InfrequentlyUsed))) {
            assert ("pipeline cannot be infrequently compiled" && !isa<PipelineKernel>(kernel));
            Infrequent.emplace_back(m);
        } else {
            Normal.emplace_back(m);
        }
    }

    auto &ES = mEngine->getExecutionSession();
    auto &MainJD = mEngine->getMainJITDylib();

    auto RunJID_Expect = ES.createJITDylib("finalize_run_" + std::to_string(reinterpret_cast<uintptr_t>(pk)));
    if (!RunJID_Expect) {
        report_fatal_error(Twine("ORC failed to allocate an execution sandbox: ") + toString(RunJID_Expect.takeError()));
    }
    auto &RunJD = *RunJID_Expect;

    // Link this temporary run context to inherit process symbols and prior links from MainJD
    RunJD.addToLinkOrder(MainJD);

    // Helper lambda to safely thread-wrap and register your generated kernel structures
    auto addModulesToORC = [&](const ModuleSet & S, const CodeGenOptLevel level) {
        if (S.empty()) return;

        unsigned moduleCounter = 0;

        // NOTE: In ORC, Optimization Levels are ideally configured per-JITDylib or via custom IR transform layers.
        // For standard transitions, compiling into the Engine instance functions directly:
        for (Module * M : S) {
            std::string uniqueID = M->getName().str() + "_run_" + 
                                   std::to_string(reinterpret_cast<uintptr_t>(pk)) + "_" + 
                                   std::to_string(moduleCounter++);
            M->setModuleIdentifier(uniqueID);

            // ORC takes absolute lifetime ownership of the module via unique_ptr wrapping
            auto UniqueM = std::unique_ptr<Module>(M);
            // Share the global driver LLVM context safe reference frame mapping
            orc::ThreadSafeContext TSCtx(std::make_unique<LLVMContext>());
            orc::ThreadSafeModule TSM(std::move(UniqueM), TSCtx);

            if (auto Err = mEngine->addIRModule(RunJD, std::move(TSM))) {
                report_fatal_error(Twine("Failed adding module to ORC execution frame: ") + toString(std::move(Err)));
            }
        }
    };

    // Add uncompiled modules straight into our JIT run scope
    addModulesToORC(Infrequent, codegen::BackEndOptLevel);
    addModulesToORC(Normal, CodeGenOptLevel::Default);

    // Build the "main" module frame context execution pipeline
    auto mainModule = std::make_unique<Module>("main", *mContext);
    mainModule->setTargetTriple(mMainModule->getTargetTriple());
    mainModule->setDataLayout(mMainModule->getDataLayout());
    mBuilder->setModule(mainModule.get());



    //pk->addKernelDeclarations(getBuilder());

    // Finalize compiling and extracting the entry address pointer context out of your JIT
    // Assuming you look up your main wrapper method afterwards using mEngine->lookup("main")

    // TODO: to ensure that we can pass the correct num of threads, we cannot statically compile the
    // main method until we add the thread count as a parameter. Investigate whether we can make a
    // better "wrapper" method for that that allows easier access to the output scalars.

    const auto e = true; // pk->containsKernelFamilyCalls() || pk->generatesDynamicRepeatingStreamSets();
    const auto method = e ? Kernel::AddInternal : Kernel::DeclareExternal;
    Function * const main = pk->addOrDeclareMainFunction(getBuilder(), method);
    mBuilder->setModule(mMainModule);

    // NOTE: the pipeline kernel is destructed after calling clear unless this driver preserves kernels!
    if (getPreservesKernels()) {
        for (auto & kernel : mCachedKernel) {
            mPreservedKernel.emplace_back(kernel.release());
        }
        for (auto & kernel : mCompiledKernel) {
            mPreservedKernel.emplace_back(kernel.release());
        }
    } else {
        mPreservedKernel.clear();
    }

    std::string mainName = main->getName().str();

    // 5. Wrap and submit your main wrapper module into the active ORC engine run instance
    orc::ThreadSafeModule TSMainModule(std::move(mainModule), orc::ThreadSafeContext(std::make_unique<LLVMContext>()));
    if (auto Err = mEngine->addIRModule(RunJD, std::move(TSMainModule))) {
        report_fatal_error(Twine("Failed adding main module script to ORC JIT: ") + toString(std::move(Err)));
    }

    if (LLVM_UNLIKELY(codegen::ShowASMOption != codegen::OmittedOption)) {
        if (!codegen::ShowASMOption.empty()) {
            std::error_code error;
            mASMOutputStream = std::make_unique<raw_fd_ostream>(codegen::ShowASMOption, error, sys::fs::OpenFlags::OF_None);
        } else {
            mASMOutputStream = std::make_unique<raw_fd_ostream>(STDERR_FILENO, false, true);
        }

        // TODO: there does not seem to be an ASM printer for the new PassManager?
        auto pm = std::make_unique<legacy::PassManager>();

        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
        const auto r = mTarget->addPassesToEmitFile(*pm, *mASMOutputStream, nullptr, CodeGenFileType::AssemblyFile);
        #else
        const auto r = mTarget->addPassesToEmitFile(*pm, *mASMOutputStream, nullptr, CGFT_AssemblyFile);
        #endif
        if (r) {
            report_fatal_error("LLVM error: could not add emit assembly pass");
        }

        for (const auto & kernel : mCachedKernel) {
            pm->run(*kernel->getModule());
        }

        for (const auto & kernel : mCompiledKernel) {
            pm->run(*kernel->getModule());
        }

    }

    mCachedKernel.clear();
    mCompiledKernel.clear();

    // 7. Look up and resolve symbols using standard target data layout policies.
    // Compilation triggers on-demand here during lookup, bypassing the old explicit finalizeObject() call.
    auto SymExpect = mEngine->lookup(RunJD, mainName);
    if (!SymExpect) {
        report_fatal_error(Twine("ORC Lookup failed for entry point ") + mainName + ": " + toString(SymExpect.takeError()));
    }

    // Convert resolved executor address smoothly to a naked execution pointer address
    void* mainFnPtr = SymExpect->toPtr<void*>();

    // NOTE ON MEMORY MANAGEMENT:
    // With MCJIT, you explicitly executed manual removeModule tracking loops here to free IR structures. 
    // In ORC, because compilation happens inside our standalone 'RunJD' sandbox partition, 
    // these compiled structures will sit immutably in memory until the base mEngine layout drops, 
    // eliminating the risk of accidental premature cross-module lookups while your code executes.

    return mainFnPtr;
}

bool CPUDriver::hasExternalFunction(llvm::StringRef functionName) const {
    return RTDyldMemoryManager::getSymbolAddressInProcess(functionName.str());
}

CPUDriver::~CPUDriver() {

}


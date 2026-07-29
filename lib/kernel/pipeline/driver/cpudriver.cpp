#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/DynamicLibrary.h>           // for LoadLibraryPermanently
#include <llvm/ExecutionEngine/ExecutionEngine.h>  // for EngineBuilder
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
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

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

using namespace llvm;
using namespace llvm::orc;
using namespace kernel;

using AttrId = kernel::Attribute::KindId;


// Modified version of llvm ConcurrentIRCompiler
class InternalCompiler : public orc::IRCompileLayer::IRCompiler {

public:

    InternalCompiler(CPUDriver & driver, JITTargetMachineBuilder JTMB, const StringMap<bool> && features, ObjectCache *ObjCache)
    : orc::IRCompileLayer::IRCompiler(irManglingOptionsFromTargetOptions(JTMB.getOptions()))
    , Driver(driver), ObjCache(ObjCache), JTMB(std::move(JTMB)), CPUFeatures(std::move(features)) {

    }

    void registerKernel(Kernel * const kernel) {
        Module * const m = kernel->getModule(); assert (m);
        InternalMapping.insert(std::make_pair(m->getName(), kernel));
    };

    // override the actual orc compiler routine to
    Expected<std::unique_ptr<MemoryBuffer>> operator()(Module & M) override {
        // TODO: use a threadpool with a fixed number of expected threads to avoid reconstructing the builder and compiler objects
        const auto f = InternalMapping.find(M.getName());

        auto optLevel = CodeGenOptLevel::Default;

        errs() << "Compiling Module Code " << M.getName() << " (" << (f != InternalMapping.end()) << ")\n";

        if (LLVM_LIKELY(f != InternalMapping.end())) {

            Kernel * const K = f->getValue();

            NamedRegionTimer T(K->getSignature(), K->getName(),
                               "Kernel", "Kernel Generation",
                               codegen::TimeKernelsIsEnabled);

            std::unique_ptr<KernelBuilder> builder(IDISA::GetIDISA_Builder(M.getContext(), CPUFeatures));
            builder->setDriver(Driver);
            builder->setModule(&M);
            K->setModule(&M); // the module may have changed

            K->generateKernel(*builder);
            if (LLVM_UNLIKELY(K->hasAttribute(AttrId::InfrequentlyUsed))) {
                optLevel = codegen::BackEndOptLevel;
            }

            errs() << "Generated Kernel Code " << K->getName() << "\n";
        }



        auto TM = cantFail(JTMB.createTargetMachine());
        TM->setOptLevel(optLevel);
        SimpleCompiler C(*TM, ObjCache);
        auto r = C(M);

        errs() << "* Compiled Module Code " << M.getName() << " (" << (f != InternalMapping.end()) << ")\n";

        return r;
    }

private:


    CPUDriver & Driver;
    ObjectCache * const ObjCache;
    JITTargetMachineBuilder JTMB;
    StringMap<Kernel *> InternalMapping;
    const StringMap<bool> CPUFeatures;
};

class KernelGenerationMU : public orc::MaterializationUnit {
public:
    KernelGenerationMU(Kernel * target, MangleAndInterner & mangler, orc::SymbolLookupSet & lookupSet,
                       IRCompileLayer & targetLayer, std::vector<ThreadSafeContext> & contexts)
    : MaterializationUnit(createInterface(target, mangler, lookupSet))
    , Target(target)
    , TargetLayer(targetLayer)
    , Contexts(contexts) {

    }

    StringRef getName() const override { return "<KernelGenerationMU>"; }

    void materialize(std::unique_ptr<MaterializationResponsibility> R) override {
        Module * const M = Target->getModule(); assert (M);
        ThreadSafeContext ctx(std::unique_ptr<LLVMContext>{&M->getContext()});
        ThreadSafeModule TSM(std::unique_ptr<Module>{M}, ctx);
        Contexts.emplace_back(ctx);
        TargetLayer.emit(std::move(R), std::move(TSM));
    }

    void discard(const JITDylib &, const SymbolStringPtr &) override {
        /* this MU adds the symbols for the IR it has yet to generate. do not discard any symbols. */
    }

    static Interface createInterface(Kernel * const target, MangleAndInterner & mangler, orc::SymbolLookupSet & lookupSet)  {
        SymbolFlagsMap map;
        target->addSymbols(mangler, map, lookupSet);
        return Interface(std::move(map), nullptr);
    }

private:

    Kernel * const Target;
    IRCompileLayer & TargetLayer;
    std::vector<ThreadSafeContext> & Contexts;
};

inline void removeAll(SymbolLookupSet & S) {
    auto k = S.size();
    while (k) {
        S.remove(--k);
    }
}

ATTRIBUTE_NO_SANITIZE_ADDRESS
CPUDriver::CPUDriver(std::string && moduleName)
: BaseDriver(std::move(moduleName))
, mUnoptimizedIROutputStream{}
, mIROutputStream{}
, mASMOutputStream{}
, mTarget(nullptr)
, mEngine(nullptr) {

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

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
        .setRelocationModel(Reloc::Static)
        .setCodeGenOptLevel(codegen::BackEndOptLevel);

    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));
    Builder.setNumCompileThreads(1);

    // Safely route the compilation process through your customized Parabix caching system
    Builder.setCompileFunctionCreator([&](llvm::orc::JITTargetMachineBuilder InnerJTMB)
        -> Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
            return std::make_unique<InternalCompiler>(
                *this,
                std::move(InnerJTMB),
                std::move(features),
                mObjectCache.get()
            );
    });

//    Builder.setObjectLinkingLayerCreator([&](ExecutionSession & ES, const Triple & T)
//        -> Expected<std::unique_ptr<ObjectLinkingLayer>> {
//        auto obj = std::make_unique<ObjectLinkingLayer>(ES);
//        obj->addPlugin(std::make_unique<KernelObjRemappingPlugin>());
//        return obj;
//    });

    mEngine = cantFail(Builder.create());

    mMainModule->setTargetTriple(mEngine->getTargetTriple().getTriple());
    mMainModule->setDataLayout(mEngine->getDataLayout());

    auto & MainJD = mEngine->getMainJITDylib();

    MainJD.addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            mEngine->getDataLayout().getGlobalPrefix()
        ))
    );

    mSymbolLookupSet = std::make_unique<SymbolLookupSet>();

    mBuilder.reset(IDISA::GetIDISA_Builder(getContext(), features));
    mBuilder->setModule(mMainModule);
    mBuilder->setDriver(*this);
}

Function * CPUDriver::addLinkFunction(Module * mod, llvm::StringRef name, FunctionType * type, void * functionPtr) const {
    if (LLVM_UNLIKELY(mod == nullptr)) {
        report_fatal_error("addLinkFunction(" + name + ") cannot be called until after addKernel");
    }
    Function * f = mod->getFunction(name);
    if (LLVM_UNLIKELY(f == nullptr)) {
        f = Function::Create(type, Function::ExternalLinkage, name, mod);

        // Define symbol mapping inside the primary ORC library environment
        auto & MainJD = mEngine->getMainJITDylib();

        auto InternedSymbol = mEngine->mangleAndIntern(name);

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
            }, [&](const ErrorInfoBase & EIB) {
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

    const auto numKernels = mUncachedKernel.size();

    auto & CL = mEngine->getIRCompileLayer();
    auto & CC = reinterpret_cast<InternalCompiler &>(CL.getCompiler());

    auto & MainJD = mEngine->getMainJITDylib();

    auto & ES = mEngine->getExecutionSession();

    ES.setErrorReporter([](Error err) {
        logAllUnhandledErrors(std::move(err), errs(), "LLJIT Session Error: ");
    });

    for (unsigned i = 0; i < numKernels; ++i) {
        CC.registerKernel(mUncachedKernel[i].get());
   }

    MangleAndInterner Mangler(ES, mEngine->getDataLayout());

    mCachedKernel.reserve(numKernels);
    for (unsigned i = 0; i < numKernels; ++i) {
        auto & kernel = mUncachedKernel[i];
        cantFail(MainJD.define(std::make_unique<KernelGenerationMU>(kernel.get(), Mangler, *mSymbolLookupSet, CL, mContexts)));
        mCachedKernel.emplace_back(kernel.release());
    }

    mUncachedKernel.clear();

    assert (!mSymbolLookupSet->containsDuplicates());

    auto S = makeJITDylibSearchOrder({&MainJD}, JITDylibLookupFlags::MatchExportedSymbolsOnly);
    cantFail(ES.lookup(S, *mSymbolLookupSet, LookupKind::Static, SymbolState::Ready));
    removeAll(*mSymbolLookupSet);

}

SymbolStringPtr CPUDriver::declareFunctionSymbol(llvm::Function * function) const {
    // this may require modulename_functionname ?
    assert (function);
    MangleAndInterner MI(mEngine->getExecutionSession(), mEngine->getDataLayout());
    SymbolStringPtr symbolPtr = MI(function->getName());
    mSymbolLookupSet->add(symbolPtr, SymbolLookupFlags::RequiredSymbol);
    return symbolPtr;
}

void CPUDriver::addCachedObjectFile(llvm::Module * module, std::unique_ptr<MemoryBuffer> &&object) {
    auto & JITLib = mEngine->getMainJITDylib();
    cantFail(mEngine->addObjectFile(JITLib, std::move(object)));
}

void * CPUDriver::finalizeObject(kernel::Kernel * const pk) {

    mBuilder->setModule(mMainModule);

    // Build the "main" module frame context execution pipeline
    pk->addKernelDeclarations(*mBuilder, false);

    // Finalize compiling and extracting the entry address pointer context out of your JIT
    // Assuming you look up your main wrapper method afterwards using mEngine->lookup("main")

    // TODO: to ensure that we can pass the correct num of threads, we cannot statically compile the
    // main method until we add the thread count as a parameter. Investigate whether we can make a
    // better "wrapper" method for that that allows easier access to the output scalars.

    Function * const main = pk->addOrDeclareMainFunction(*mBuilder, Kernel::AddInternal);

    // NOTE: the pipeline kernel is destructed after calling clear unless this driver preserves kernels!


    //  Wrap and submit your main wrapper module into the active ORC engine run instance

    MangleAndInterner MI(mEngine->getExecutionSession(), mEngine->getDataLayout());
    SymbolStringPtr mainSymbol = MI(main->getName());
    mSymbolLookupSet->add(mainSymbol, SymbolLookupFlags::RequiredSymbol);

    auto & MainJD = mEngine->getMainJITDylib();
    ThreadSafeContext ctx(std::unique_ptr<LLVMContext>{&mMainModule->getContext()});
    ThreadSafeModule TSM(std::unique_ptr<Module>{mMainModule}, ctx);
    mContexts.emplace_back(ctx);
    cantFail(mEngine->addIRModule(MainJD, std::move(TSM)));


#if 0
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
#endif



    // 7. Look up and resolve symbols using standard target data layout policies.
    // Compilation triggers on-demand here during lookup, bypassing the old explicit finalizeObject() call.

    assert (!mSymbolLookupSet->containsDuplicates());

    auto & ES = mEngine->getExecutionSession();
    auto S = makeJITDylibSearchOrder({&MainJD}, JITDylibLookupFlags::MatchExportedSymbolsOnly); // mSymbolStubs,
    auto funcMap = cantFail(ES.lookup(S, *mSymbolLookupSet, LookupKind::Static, SymbolState::Ready));
    auto mainFuncPtr = funcMap.find(mainSymbol)->getSecond().getAddress().toPtr<void*>();

    assert (mainFuncPtr);

    removeAll(*mSymbolLookupSet);

    // NOTE ON MEMORY MANAGEMENT:
    // With MCJIT, you explicitly executed manual removeModule tracking loops here to free IR structures. 
    // In ORC, because compilation happens inside our standalone 'RunJD' sandbox partition, 
    // these compiled structures will sit immutably in memory until the base mEngine layout drops, 
    // eliminating the risk of accidental premature cross-module lookups while your code executes.

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

    mCachedKernel.clear();
    mCompiledKernel.clear();

    //    llvm::reportAndResetTimings();
    //    llvm::PrintStatistics();

    return mainFuncPtr;
}

bool CPUDriver::hasExternalFunction(llvm::StringRef functionName) const {
//    auto & ES = mEngine->getExecutionSession();
//    auto ss = ES.intern(functionName);
//    auto r = ES.lookup(mEngine->defaultLinkOrder(), ss, SymbolState::Ready);
//    return r.operator bool();
    return RTDyldMemoryManager::getSymbolAddressInProcess(functionName.str());
}

CPUDriver::~CPUDriver() {

}


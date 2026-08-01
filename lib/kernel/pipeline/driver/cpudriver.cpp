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

class KernelGenerationContainer {

private:
    ThreadSafeContext Context;
};

// Modified version of llvm ConcurrentIRCompiler
class CPUDriverIRCompiler : public orc::IRCompileLayer::IRCompiler {

public:

    CPUDriverIRCompiler(CPUDriver & driver, JITTargetMachineBuilder JTMB, const StringMap<bool> && features, ObjectCache *ObjCache)
    : orc::IRCompileLayer::IRCompiler(irManglingOptionsFromTargetOptions(JTMB.getOptions()))
    , Driver(driver), ObjCache(ObjCache), JTMB(std::move(JTMB)), CPUFeatures(std::move(features)) {

    }

    void registerKernel(Kernel * const kernel) {
        Module * const m = kernel->getModule(); assert (m);
        InternalMapping.insert(std::make_pair(m, kernel));
    };

    // override the actual orc compiler routine to
    Expected<std::unique_ptr<MemoryBuffer>> operator()(Module & M) override {
        // TODO: use a threadpool with a fixed number of expected threads to avoid reconstructing the builder and compiler objects
        const auto f = InternalMapping.find(&M);



        auto optLevel = CodeGenOptLevel::Default;
        if (LLVM_LIKELY(f != InternalMapping.end())) {

            Kernel * const K = f->getSecond();

            errs() << " ----- generating " << K->getName() << "\n";

            assert (M.empty());

            NamedRegionTimer T(K->getSignature(), K->getName(),
                               "Kernel", "Kernel Generation",
                               codegen::TimeKernelsIsEnabled);

            std::unique_ptr<KernelBuilder> builder(IDISA::GetIDISA_Builder(M.getContext(), CPUFeatures));
            builder->setDriver(Driver);
            builder->setModule(&M);
            for (const auto & link : Driver.mLinkedFunctions) {
                if (link.Target == K || link.Target == nullptr) {
                    Type * funcType = CBuilder::convertTypeToLLVMContext(M.getContext(), link.FunctionDecl->getFunctionType());
                    Function::Create(cast<FunctionType>(funcType), Function::ExternalLinkage, link.FunctionDecl->getName(), &M);
                }
            }
            K->setModule(&M); // the module may have changed
            K->generateKernel(*builder);
            if (LLVM_UNLIKELY(K->hasAttribute(AttrId::InfrequentlyUsed))) {
                optLevel = codegen::BackEndOptLevel;
            }
        }

        NamedRegionTimer T(M.getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        auto TM = cantFail(JTMB.createTargetMachine());
        TM->setOptLevel(optLevel);
        SimpleCompiler C(*TM, ObjCache);
        return C(M);
    }

private:


    CPUDriver & Driver;
    ObjectCache * const ObjCache;
    JITTargetMachineBuilder JTMB;
    DenseMap<const Module *, Kernel *> InternalMapping;
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
        // TODO: using a null context may allow us to select from a pool of contexts and builders
        // but I need to remove Kernel::makeModule, getModule, and setModule first.
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

    mTarget = TheTarget->createTargetMachine(
        TripleStr,               // Target Triple
        CPUStr,                  // Host CPU Name
        llvm::join(attrs, ","),  // Flattened features string list
        Options,                 // Pipeline CodeGen target configurations
        std::nullopt,            // Fixes error! Expresses 'Default' relocation model
        CodeModel::Small,        // Standard Code Model
        codegen::BackEndOptLevel // Optimization configurations
    );

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
            return std::make_unique<CPUDriverIRCompiler>(
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

    mAllLinkedSymbols = std::make_unique<SymbolMap>();

    mBuilder.reset(IDISA::GetIDISA_Builder(getContext(), features));
    mBuilder->setModule(mMainModule);
    mBuilder->setDriver(*this);

    StreamSetBuffer::linkFunctions(*mBuilder);
    mBuilder->LinkAllNecessaryExternalFunctions();
}

Function * CPUDriver::addLinkFunction(Kernel * kernel, llvm::StringRef name, FunctionType * type, void * functionPtr) {
    // TODO: this will need a main context lock
    assert (&mMainModule->getContext() == mContext);
    assert (&type->getContext() == mContext);
    Function * f = mMainModule->getFunction(name);
    if (LLVM_UNLIKELY(f == nullptr)) {
        f = Function::Create(type, Function::ExternalLinkage, name, mMainModule);
        MangleAndInterner M(mEngine->getExecutionSession(), mEngine->getDataLayout());
        auto symbol = M(name);
        auto addr = orc::ExecutorAddr::fromPtr(functionPtr);
        mAllLinkedSymbols->insert(std::make_pair(symbol, ExecutorSymbolDef{addr, JITSymbolFlags::Exported}));
    }
    mLinkedFunctions.emplace_back(kernel, f);
    return f;
}

void CPUDriver::linkAllExternalSymbols() {
    auto & MainJD = mEngine->getMainJITDylib();
    auto err = MainJD.define(orc::absoluteSymbols(*mAllLinkedSymbols));
    if (err) {
        handleAllErrors(std::move(err),
            [](const DuplicateDefinition &) {
                /* ignored */
            },
            [](const ErrorInfoBase & err) {
                SmallVector<char, 100> tmp;
                raw_svector_ostream msg(tmp);
                msg << "Cannot link symbol: " << err.message();
                report_fatal_error(msg.str());
            });
    }
    mAllLinkedSymbols->clear();
}

void CPUDriver::generateUncachedKernels() {

    if (mUncachedKernel.empty()) return;

    // TODO: we may be able to reduce unnecessary optimization work by having kernel specific optimization passes.

    // NOTE: we currently require DCE and Mem2Reg for each kernel to eliminate any unnecessary scalar -> value
    // mappings made by the base KernelCompiler. That could be done in a more focused manner, however, as each
    // mapping is known.

    const auto numKernels = mUncachedKernel.size();

    auto & CL = mEngine->getIRCompileLayer();
    auto & CC = reinterpret_cast<CPUDriverIRCompiler &>(CL.getCompiler());

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

    linkAllExternalSymbols();

    auto S = makeJITDylibSearchOrder({&MainJD}, JITDylibLookupFlags::MatchExportedSymbolsOnly);
    cantFail(ES.lookup(S, *mSymbolLookupSet, LookupKind::Static, SymbolState::Ready));
    removeAll(*mSymbolLookupSet);

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

    linkAllExternalSymbols();

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


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
#include <queue>
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

class KernelGenerationMU;

// TODO: if a task dependency system exists, we could split the task of state identification from codegen but
// could not guarantee that the same thread/context would process it. Most kernel state types are defined fully
// in their constructors. The pipeline is the only know exception. Thus very few dependencies would be needed.


namespace {

struct CPUDriverContext : public ThreadSafeContext, public FunctionLinkCallback {
    Kernel *                                TargetKernel;
    size_t                                  IsCompilingMainFunction;
    std::unique_ptr<llvm::TargetMachine>    TargetMachine;
    std::unique_ptr<KernelBuilder>          Builder;
    std::unique_ptr<SimpleCompiler>         Compiler;
    llvm::Module *                          CurrentModule;
    llvm::orc::LLJIT *                      Engine;
    llvm::orc::SymbolMap &                  SharedSymbolList;
    llvm::orc::SymbolMap                    NewSymbolList;

    CPUDriverContext(std::unique_ptr<LLVMContext> ctx, JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, ObjectCache * ObjCache, SymbolMap & symbolList)
    : ThreadSafeContext(std::move(ctx))
    , TargetKernel(nullptr)
    , IsCompilingMainFunction(0)
    , TargetMachine(cantFail(JTMB.createTargetMachine()))
    , Builder(IDISA::GetIDISA_Builder(*getContext(), features))
    , Compiler(std::make_unique<SimpleCompiler>(*TargetMachine, ObjCache))
    , CurrentModule(nullptr)
    , Engine(nullptr)
    , SharedSymbolList(symbolList) {
        Builder->setFunctionLinkCallback(this);
        // TODO: does LLVM16 still use setDiagnosticContext?
        getContext()->setDiagnosticHandlerCallBack(nullptr, static_cast<void*>(this));
    }

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
        assert (CurrentModule && (&functionType->getContext() == &CurrentModule->getContext()));
        Function * f = CurrentModule->getFunction(unmangledName);
        if (LLVM_UNLIKELY(f == nullptr)) {
            f = Function::Create(functionType, Function::ExternalLinkage, unmangledName, CurrentModule);
            assert (Engine);
            auto & ES = Engine->getExecutionSession();
            auto addr = orc::ExecutorAddr::fromPtr(functionPointer);
            SymbolStringPtr symbol = nullptr;
            bool added = false;
            const auto flags = JITSymbolFlags::Exported | JITSymbolFlags::Callable;
            ES.runSessionLocked([&]{
                MangleAndInterner M(ES, Engine->getDataLayout());
                symbol = M(unmangledName);
                added = SharedSymbolList.insert(std::make_pair(symbol, ExecutorSymbolDef{addr, flags})).second;
            });
            if (added) {
                NewSymbolList.insert(std::make_pair(symbol, ExecutorSymbolDef{addr, flags}));
            }
        }
        return f;
    }

    bool HasExternalFunction(llvm::StringRef unmangledName) const {
        return RTDyldMemoryManager::getSymbolAddressInProcess(unmangledName.str());
    }

    virtual ~CPUDriverContext() {}
};

class CPUDriverContextPool {
public:

    CPUDriverContextPool(const size_t count, JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, ObjectCache * ObjCache, SymbolMap & symbolList) {
        for (size_t i = 0; i < count; ++i) {
            Contexts.push(std::make_unique<CPUDriverContext>(std::make_unique<LLVMContext>(), JTMB, features, ObjCache, symbolList));
        }
    }

    CPUDriverContext * acquire() {
        // TODO: use session lock?
        std::unique_lock<std::mutex> lock(Mutex);
        Cond.wait(lock, [this](){ return !Contexts.empty(); });
        auto next = Contexts.front().release();
        Contexts.pop();
        return next;
    }

    void release(CPUDriverContext * const ctx) {
        std::unique_lock<std::mutex> lock(Mutex);
        Contexts.push(std::unique_ptr<CPUDriverContext>(ctx));
        Cond.notify_one();
    }

private:
    std::mutex Mutex;
    std::condition_variable Cond;
    std::queue<std::unique_ptr<CPUDriverContext>> Contexts;
};


} // end of anon namespace



// Modified version of llvm ConcurrentIRCompiler
class CPUDriverKernelCompiler : public orc::IRCompileLayer::IRCompiler {

public:

    CPUDriverKernelCompiler(JITTargetMachineBuilder & JTMB, ParabixObjectCache * const objCache)
    : orc::IRCompileLayer::IRCompiler(irManglingOptionsFromTargetOptions(JTMB.getOptions()))
    , Pool(nullptr)
    , ObjectCache(objCache) {

    }

    void setPool(CPUDriverContextPool * pool) {
        Pool = pool;
    }

    void setEngine(LLJIT * engine) {
        Engine = engine;
    }

    // override the actual orc compiler routine to
    Expected<std::unique_ptr<MemoryBuffer>> operator()(Module & M) override {
        // TODO: use a threadpool with a fixed number of expected threads to avoid reconstructing the builder and compiler objects
        auto ctx = static_cast<CPUDriverContext *>(M.getContext().getDiagnosticContext()); assert (ctx);

        // TODO: need to do the IR lookup from the object cache here

        Kernel * const K = ctx->TargetKernel;
        auto & C = M.getContext();
        assert (ctx->getContext() == &C);

        auto & builder = *ctx->Builder;

        ctx->CurrentModule = &M;
        ctx->Engine = Engine;
        builder.setModule(&M);

        M.setTargetTriple(Engine->getTargetTriple().getTriple());
        M.setDataLayout(Engine->getDataLayout());

        K->linkExternalMethods(builder);

        auto & SL = ctx->NewSymbolList;

        if (!SL.empty()) {
            auto & MainJD = Engine->getMainJITDylib();
            auto err = MainJD.define(orc::absoluteSymbols(SL));
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
            SL.clear();
        }

        auto optLevel = CodeGenOptLevel::Default;
        if (LLVM_LIKELY(ctx->IsCompilingMainFunction == 0)) {

            NamedRegionTimer T(K->getSignature(), K->getName(),
                               "Kernel", "Kernel Generation",
                               codegen::TimeKernelsIsEnabled);

            if (LLVM_LIKELY(ObjectCache && K->isCachable())) {
                auto cached = ObjectCache->loadCachedObjectFile(builder, K, M);
                if (cached) {
                    Pool->release(ctx);
                    return cached;
                }
            }

            K->generateKernel(builder, ctx->TargetMachine.get());
            if (LLVM_UNLIKELY(K->hasAttribute(AttrId::InfrequentlyUsed))) {
                optLevel = codegen::BackEndOptLevel;
            }

        } else {

            // Build the "main" module frame context execution pipeline
            K->addKernelDeclarations(builder);
            K->addOrDeclareMainFunction(builder, Kernel::AddInternal);

        }

        NamedRegionTimer T(M.getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        ctx->TargetMachine->setOptLevel(optLevel);
        auto result = ctx->Compiler->operator()(M);
        // M.dropAllReferences();
        Pool->release(ctx);
        return result;
    }

private:

    CPUDriverContextPool * Pool = nullptr;
    ParabixObjectCache * const ObjectCache;
    LLJIT * Engine = nullptr;

};

class KernelGenerationMU : public orc::MaterializationUnit {
public:
    KernelGenerationMU(Kernel * target, MangleAndInterner & mangler, orc::SymbolLookupSet & lookupSet,
                       IRCompileLayer & targetLayer, CPUDriverContextPool & pool)
    : MaterializationUnit(createInterface(target, mangler, lookupSet))
    , Target(target)
    , TargetLayer(targetLayer)
    , Pool(pool) {

    }

    StringRef getName() const override { return "<KernelGenerationMU>"; }

    void materialize(std::unique_ptr<MaterializationResponsibility> R) override {
        auto ctx = Pool.acquire();
        assert (&ctx->Builder->getContext() == ctx->getContext());
        ctx->TargetKernel = Target; assert (Target);
        ctx->IsCompilingMainFunction = 0;
        Module * const M = Target->makeEmptyModule(*ctx->Builder);
        ThreadSafeModule TSM(std::unique_ptr<Module>(M), *ctx);
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
    CPUDriverContextPool & Pool;
};

class MainGenerationMU : public orc::MaterializationUnit {
public:
    MainGenerationMU(Kernel * target, SymbolStringPtr mainSymbol, orc::SymbolLookupSet & lookupSet,
                       IRCompileLayer & targetLayer, CPUDriverContextPool & pool)
    : MaterializationUnit(createInterface(mainSymbol, lookupSet))
    , Target(target)
    , TargetLayer(targetLayer)
    , Pool(pool) {

    }

    StringRef getName() const override { return "<MainGenerationMU>"; }

    void materialize(std::unique_ptr<MaterializationResponsibility> R) override {
        auto ctx = Pool.acquire();
        assert (&ctx->Builder->getContext() == ctx->getContext());
        ctx->TargetKernel = Target; assert (Target);
        ctx->IsCompilingMainFunction = 1;
       // R->addDependencies()
        ThreadSafeModule TSM(std::make_unique<Module>("main", *ctx->getContext()), *ctx);
        TargetLayer.emit(std::move(R), std::move(TSM));
    }

    void discard(const JITDylib &, const SymbolStringPtr &) override {
        /* this MU adds the symbols for the IR it has yet to generate. do not discard any symbols. */
    }

    static Interface createInterface(SymbolStringPtr mainSymbol, orc::SymbolLookupSet & lookupSet)  {
        SymbolFlagsMap symbols;
        symbols.insert(std::make_pair(mainSymbol, JITSymbolFlags::Exported | JITSymbolFlags::Callable));
        lookupSet.add(mainSymbol, orc::SymbolLookupFlags::RequiredSymbol);
        return Interface(std::move(symbols), nullptr);
    }

private:

    Kernel * const Target;
    IRCompileLayer & TargetLayer;
    CPUDriverContextPool & Pool;
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

    auto TripleStr = Triple{sys::getProcessTriple()}.normalize();

    auto JTMB = orc::JITTargetMachineBuilder(Triple{TripleStr});

    JTMB.setCPU(sys::getHostCPUName().str())
        .addFeatures(attrs)
        .setOptions(codegen::target_Options)
        .setRelocationModel(Reloc::Static)
        .setCodeModel(CodeModel::Small)
        .setCodeGenOptLevel(codegen::BackEndOptLevel);


    const size_t numOfThreads = 4;

    mAllLinkedSymbols = std::make_unique<SymbolMap>();

    mContextPool = std::make_unique<CPUDriverContextPool>(numOfThreads, JTMB, features, mObjectCache.get(), *mAllLinkedSymbols);

    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));
    Builder.setNumCompileThreads(numOfThreads);

    // Safely route the compilation process through your customized Parabix caching system
    Builder.setCompileFunctionCreator([&](llvm::orc::JITTargetMachineBuilder InnerJTMB)
        -> Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
            return std::make_unique<CPUDriverKernelCompiler>(InnerJTMB, mObjectCache.get());
    });

    mEngine = cantFail(Builder.create());



    auto & CL = mEngine->getIRCompileLayer();
    auto & CC = reinterpret_cast<CPUDriverKernelCompiler &>(CL.getCompiler());

    CC.setPool(mContextPool.get());
    CC.setEngine(mEngine.get());

    auto & MainJD = mEngine->getMainJITDylib();

    MainJD.addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            mEngine->getDataLayout().getGlobalPrefix()
        ))
    );

    mSymbolLookupSet = std::make_unique<SymbolLookupSet>();



    mBuilder.reset(IDISA::GetIDISA_Builder(mMainModule->getContext(), features));
    mBuilder->setModule(mMainModule);
    mBuilder->setFunctionLinkCallback(this);
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

    // TODO: we don't want to have more contexts than our thread count will allow

    const auto numKernels = mUncachedKernel.size();

    auto & CL = mEngine->getIRCompileLayer();

    auto & MainJD = mEngine->getMainJITDylib();

    auto & ES = mEngine->getExecutionSession();

    ES.setErrorReporter([](Error err) {
        logAllUnhandledErrors(std::move(err), errs(), "LLJIT Session Error: ");
    });

    MangleAndInterner Mangler(ES, mEngine->getDataLayout());

    mCachedKernel.reserve(numKernels);
    for (unsigned i = 0; i < numKernels; ++i) {
        auto & kernel = mUncachedKernel[i];
        cantFail(MainJD.define(std::make_unique<KernelGenerationMU>(kernel.get(), Mangler, *mSymbolLookupSet, CL, *mContextPool)));
        assert (!mSymbolLookupSet->containsDuplicates());
        mCachedKernel.emplace_back(kernel.release());
    }

    mUncachedKernel.clear();

    auto S = makeJITDylibSearchOrder({&MainJD}, JITDylibLookupFlags::MatchExportedSymbolsOnly);
    cantFail(ES.lookup(S, *mSymbolLookupSet, LookupKind::Static, SymbolState::Ready));
    removeAll(*mSymbolLookupSet);

}

void CPUDriver::addCachedObjectFile(llvm::Module * module, std::unique_ptr<MemoryBuffer> &&object) {
    auto & JITLib = mEngine->getMainJITDylib();
    cantFail(mEngine->addObjectFile(JITLib, std::move(object)));
}

void * CPUDriver::finalizeObject(kernel::Kernel * const pk) {

    auto & MainJD = mEngine->getMainJITDylib();

    MangleAndInterner mangler(mEngine->getExecutionSession(), mEngine->getDataLayout());

    SmallVector<char, 256> tmp;
    raw_svector_ostream mainName(tmp);
    mainName << pk->getName() << "_main";
    auto mainSymbol = mangler(mainName.str());

    auto & CL = mEngine->getIRCompileLayer();

    cantFail(MainJD.define(std::make_unique<MainGenerationMU>(pk, mainSymbol, *mSymbolLookupSet, CL, *mContextPool)));
    assert (!mSymbolLookupSet->containsDuplicates());

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

llvm::Function * CPUDriver::LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
    assert (&functionType->getContext() == &mMainModule->getContext());
    Function * f = mMainModule->getFunction(unmangledName);
    if (LLVM_UNLIKELY(f == nullptr)) {
        f = Function::Create(functionType, Function::ExternalLinkage, unmangledName, mMainModule);
        auto & ES = mEngine->getExecutionSession();
        ES.runSessionLocked([&]{
            MangleAndInterner M(mEngine->getExecutionSession(), mEngine->getDataLayout());
            auto symbol = M(unmangledName);
            auto addr = orc::ExecutorAddr::fromPtr(functionPointer);
            mAllLinkedSymbols->insert(std::make_pair(symbol, ExecutorSymbolDef{addr, JITSymbolFlags::Exported}));
        });
    }
    return f;
}

bool CPUDriver::HasExternalFunction(llvm::StringRef functionName) const {
    return RTDyldMemoryManager::getSymbolAddressInProcess(functionName.str());
}

CPUDriver::~CPUDriver() {
    cantFail(mEngine->getExecutionSession().endSession());
}


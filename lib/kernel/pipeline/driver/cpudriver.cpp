#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/DynamicLibrary.h>           // for LoadLibraryPermanently
#include <llvm/ExecutionEngine/ExecutionEngine.h>  // for EngineBuilder
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/ObjectFileInterface.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/InitializePasses.h>                 // for initializeCodeGencd .
#include <llvm/PassRegistry.h>                     // for PassRegistry
#include <llvm/Support/CodeGen.h>                  // for Level, Level::None
#include <llvm/Support/Compiler.h>                 // for LLVM_UNLIKELY
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Timer.h>
#include <llvm/Transforms/Utils/Cloning.h>
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
//    Kernel *                                TargetKernel;
//    size_t                                  IsCompilingMainFunction;
    std::unique_ptr<llvm::TargetMachine>    TargetMachine;
    std::unique_ptr<KernelBuilder>          Builder;
    std::unique_ptr<SimpleCompiler>         Compiler;
    llvm::Module *                          CurrentModule;
    llvm::orc::LLJIT *                      Engine;
    llvm::orc::SymbolMap &                  SharedSymbolList;
    llvm::orc::SymbolMap                    NewSymbolList;

    CPUDriverContext(JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, SymbolMap & symbolList)
    : ThreadSafeContext(std::make_unique<LLVMContext>())
    , TargetMachine(cantFail(JTMB.createTargetMachine()))
    , Builder(IDISA::GetIDISA_Builder(*getContext(), features))
    , Compiler(std::make_unique<SimpleCompiler>(*TargetMachine, nullptr))
    , CurrentModule(nullptr)
    , Engine(nullptr)
    , SharedSymbolList(symbolList) {
        Builder->setFunctionLinkCallback(this);
    }

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
        Function * f = CurrentModule->getFunction(unmangledName);
        if (LLVM_UNLIKELY(f == nullptr)) {
            auto & C = CurrentModule->getContext();
            FunctionType * funcTy = cast<FunctionType>(CBuilder::convertTypeToLLVMContext(C, functionType));
            f = Function::Create(funcTy, Function::ExternalLinkage, unmangledName, CurrentModule);
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

struct CPUDriverTask {
    Kernel * Target;
    Module * TargetModule;
    size_t   TypeId;

    CPUDriverTask() = default;

    CPUDriverTask(size_t typeId, Kernel * target, Module * module) : TypeId(typeId), Target(target), TargetModule(module) { }

    CPUDriverTask & operator=(const CPUDriverTask&) = default;


};

struct CPUDriverTaskQueue {

    CPUDriverTaskQueue(size_t initialCapacity = 64)
    : Head(0), Tail(0), Buffer(initialCapacity), InFlight(0) {

    }

    void push(CPUDriverTask & task) {
        //std::lock_guard<std::mutex> L(Mutex);
        if (LLVM_UNLIKELY(((Tail + 1U) % Buffer.size()) == Head)) {
            Buffer.resize(Buffer.size() * 2);
        }
        Buffer[Tail] = task; assert (task.Target);
        Tail = (Tail + 1U) % Buffer.size();
        InFlight++;
    }

    bool pop(CPUDriverTask & out) {
        //std::lock_guard<std::mutex> L(Mutex);
        if (Head == Tail) {
            return false;
        }
        out = Buffer[Head]; assert (out.Target);
        Head = (Head + 1U) % Buffer.size();
        return true;
    }

    bool any() const {
        return InFlight != 0;
    }

    void markCompleted() {
        InFlight--;
    }

private:
    size_t Head;
    size_t Tail;
    std::vector<CPUDriverTask> Buffer;
    size_t InFlight;
};

class CPUDriverCompiler {
public:

    CPUDriverCompiler(ThreadPoolStrategy strategy,
                      JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, SymbolMap & symbolList,
                      ParabixObjectCache * objCache)
    : Engine(nullptr)
    , ObjectCache(objCache)
    , LaneCount(1)
    , ActiveThreads(strategy.ThreadsRequested)
    , Tasks(ActiveThreads)
    , Contexts(ActiveThreads, nullptr) {

        // NOTE: THe pipeline may read information from other kernels to determine their state types and convert them
        // to its own LLVMContext as needed. We preserve the contexts as long as this compiler exists.
        for (size_t i = 0; i < ActiveThreads; ++i) {
            Contexts[i] = new CPUDriverContext(JTMB, features, symbolList);
        }

        for (size_t i = 0; i < ActiveThreads; ++i) {
            CPUDriverContext & ctx = *(Contexts[i]);
            Threads.emplace_back([this, &ctx]() {
                for (;;) {

                    CPUDriverTask toExecute;
                    size_t taskIndex = 0;
                    bool hasTask = false;

                    auto checkQueue = [&]() -> bool {

                        std::lock_guard<std::mutex> L(LaneMutex);
                        assert (LaneCount > 0);

                        for (taskIndex = 1; taskIndex < LaneCount; ++taskIndex) {
                            auto & cur = Tasks[taskIndex];
                            if (cur.any()) {
                                hasTask = cur.pop(toExecute);
                                if (hasTask) {
                                    return true;
                                }
                                break;
                            }
                        }

                        taskIndex = 0;
                        auto & cur = Tasks[0];
                        hasTask = cur.pop(toExecute);
                        return hasTask;
                    };

                    BEGIN_SCOPED_REGION
                    std::unique_lock<std::mutex> T(TaskMutex);
                    while (!checkQueue() && NoMoreNewTasks.load(std::memory_order_acquire) == 0) {
                        TaskCV.wait(T);
                    }
                    END_SCOPED_REGION

                    if (hasTask) {

                        switch (toExecute.TypeId) {
                            case 0:
                                materializeObject(ctx, toExecute.Target, toExecute.TargetModule);
                                break;
                            case 1:
                                materializeDecl(ctx, taskIndex, toExecute.Target);
                                break;
                            case 2:
                                materializeMain(ctx, toExecute.Target);
                                break;
                        }

                        BEGIN_SCOPED_REGION
                        std::lock_guard<std::mutex> T(TaskMutex);
                        // we need an immutable DS for the tasks array. use linked list?
                        std::lock_guard<std::mutex> L(LaneMutex);
                        Tasks[taskIndex].markCompleted();
                        END_SCOPED_REGION

                    } else if (NoMoreNewTasks.load(std::memory_order_acquire)) {
                        std::unique_lock<std::mutex> S(ShutdownMutex);
                        assert (ActiveThreads > 0);
                        --ActiveThreads;
                        if (ActiveThreads == 0) {
                            ShutdownCV.notify_all();
                        }
                        break;
                    }
                }
            });
        }
    }

    size_t addNewTaskGroup() {
        const auto m = Tasks.size();
        if (LaneCount < m) {
            return LaneCount++;
        }
        std::lock_guard<std::mutex> L(LaneMutex);
        Tasks.resize(m * 2);
        return LaneCount++;
    }

    void addCompilationTask(const size_t typeId, const size_t taskLayer, Kernel * const kernel, Module * const module = nullptr) {
        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> R(TaskMutex);
        assert (taskLayer < Tasks.size());
        auto & T = Tasks[taskLayer];
        CPUDriverTask S{typeId, kernel, module};
        T.push(S);
        END_SCOPED_REGION
        TaskCV.notify_one();
    }

    void * waitUntilCompleted(Kernel * const Target) {

        NoMoreNewTasks.store(1, std::memory_order_release);
        std::unique_lock<std::mutex> S(ShutdownMutex);
        ShutdownCV.wait(S, [this]{
            return (ActiveThreads == 0);
        });
        for (auto & t : Threads) {
            if (t.joinable()) t.join();
        }

        auto & JITLib = Engine->getMainJITDylib();

        SmallVector<char, 256> tmp;
        raw_svector_ostream mainName(tmp);
        mainName << Target->getName() << "_main";
        auto sym = Engine->lookup(JITLib, mainName.str());
        if (!sym) {
            report_fatal_error(sym.takeError());
        }
        return sym->toPtr<void*>();

    }

    void setEngine(orc::LLJIT * engine) {
        Engine = engine;
    }

    ~CPUDriverCompiler() {
        for (auto & C : Contexts) {
            delete C;
        }
    }

private:

    void materializeDecl(CPUDriverContext & ctx, const size_t declLayer, Kernel * Target) {

        const auto sig = Target->hasSignature() ? Target->getSignature() : StringRef{Target->getName()};

        BEGIN_SCOPED_REGION
        bool added;
        StringMap<Kernel *>::iterator itr;
        std::unique_lock<std::mutex> L(PrecompiledStateObjectMutex);
        std::tie(itr, added) = AlreadyCompiled.insert(std::make_pair(sig, nullptr));
        if (LLVM_UNLIKELY(!added)) {
            const auto other = itr->getValue();
            L.unlock();
            // TODO: this isn't safe if we want to discard kernels on restart
            if (other) {
                Target->setSharedStateType(other->getSharedStateType());
                Target->setThreadLocalStateType(other->getThreadLocalStateType());
            } else {
                // We haven't yet finished declaring the other instance of this one. Re-add this
                // to the queue and hope we can continue processing after.
                addCompilationTask(1, declLayer, Target);
            }
            return;
        }
        L.unlock();
        END_SCOPED_REGION

        assert (&ctx.Builder->getContext() == ctx.getContext());

        auto & builder = *ctx.Builder;

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            std::unique_ptr<MemoryBuffer> cached;
            std::unique_ptr<Module> M;
            std::tie(cached, M) = ObjectCache->loadCachedObjectFile(builder, Target);
            if (M) {

                auto & JITLib = Engine->getMainJITDylib();

                M->setTargetTriple(Engine->getTargetTriple().getTriple());
                M->setDataLayout(Engine->getDataLayout());

                cantFail(M->materializeAll());

                Target->loadCachedKernel(M.get());
                builder.setModule(M.get());
                ctx.CurrentModule = M.get();
                ctx.Engine = Engine;

                linkExternalFunctions(ctx, Target);

                cantFail(Engine->addObjectFile(JITLib, std::move(cached)));
                goto record_decl;
            }
        }

        BEGIN_SCOPED_REGION
        Module * const M = Target->makeEmptyModule(builder); assert (M);
        builder.setModule(M);
        ctx.CurrentModule = M;
        ctx.Engine = Engine;
        Target->declareStateTypes(builder);
        addCompilationTask(0, 0, Target, M);
        END_SCOPED_REGION
record_decl:
        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(PrecompiledStateObjectMutex);
        auto f = AlreadyCompiled.find(sig);
        assert (f != AlreadyCompiled.end());
        assert (f->second == nullptr);
        f->second = Target;
        END_SCOPED_REGION

    }



    void materializeObject(CPUDriverContext & ctx, Kernel * Target, Module * TargetModule) {

        auto & C = *ctx.getContext();

        // We can't be sure that the context associated with the decl is the same
        // one that we acquired here. However since we cannot control the order of
        // which materialization tasks will fire, it's better to just regenerate the
        // state type metadata as needed or we risk deadlocking the system.
        Module * M = TargetModule; assert (M);
        if (&M->getContext() != &C) {
            M = new Module(TargetModule->getModuleIdentifier(), C);
            for (const auto & og : TargetModule->named_metadata()) {
                NamedMDNode * const md = M->getOrInsertNamedMetadata(og.getName());
                const auto n = og.getNumOperands();
                for (unsigned i = 0; i < n; ++i) {
                    auto val = CBuilder::convertMetadataToLLVMContext(M, og.getOperand(i));
                    md->addOperand(cast<MDNode>(val));
                }
            }
            #ifndef NDEBUG
            StructType * const sharedTy = Target->getSharedStateType();
            StructType * const threadLocalTy = Target->getThreadLocalStateType();
            #endif
            Target->loadCachedKernel(M);
            assert ((sharedTy == nullptr) == (Target->getSharedStateType() == nullptr));
            assert ((sharedTy == nullptr) || (Target->getSharedStateType() != sharedTy));
            assert ((threadLocalTy == nullptr) == (Target->getThreadLocalStateType() == nullptr));
            assert ((threadLocalTy == nullptr) || (Target->getThreadLocalStateType() != threadLocalTy));
            delete TargetModule;
        }

        auto & builder = *ctx.Builder;
        assert (&builder.getContext() == &C);
        builder.setModule(M);
        ctx.CurrentModule = M;
        ctx.Engine = Engine; assert (Engine);

        M->setTargetTriple(Engine->getTargetTriple().getTriple());
        M->setDataLayout(Engine->getDataLayout());

        linkExternalFunctions(ctx, Target);

        BEGIN_SCOPED_REGION
        NamedRegionTimer T(Target->getSignature(), Target->getName(),
                           "Kernel", "Kernel Generation",
                           codegen::TimeKernelsIsEnabled);

        Target->generateKernel(builder, ctx.TargetMachine.get(), GlobalValue::ExternalLinkage);

        Kernel::SelectedOptimizationPasses passes;
        Target->addOptimizationPasses(builder, passes);
        BaseDriver::runAllOptimizationPasses(builder, passes, ctx.TargetMachine.get());

        END_SCOPED_REGION

        BEGIN_SCOPED_REGION

        NamedRegionTimer T(M->getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        auto optLevel = CodeGenOptLevel::Default;
        if (LLVM_UNLIKELY(Target->hasAttribute(AttrId::InfrequentlyUsed))) {
            optLevel = codegen::BackEndOptLevel;
        }
        ctx.TargetMachine->setOptLevel(optLevel);
        auto result = cantFail(ctx.Compiler->operator()(*M));

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            ObjectCache->saveCachedObjectFile(*M, result->getMemBufferRef());
        }

        auto & JITLib = Engine->getMainJITDylib();
        cantFail(Engine->addObjectFile(JITLib, std::move(result)));

        delete M;

        END_SCOPED_REGION

    }

    void materializeMain(CPUDriverContext & ctx, Kernel * Target) {

        assert (&ctx.Builder->getContext() == ctx.getContext());



        auto M = std::make_unique<Module>("main", *ctx.getContext());
        M->setTargetTriple(Engine->getTargetTriple().getTriple());
        M->setDataLayout(Engine->getDataLayout());

        KernelBuilder & builder = *ctx.Builder;
        builder.setModule(M.get());
        ctx.CurrentModule = M.get();
        ctx.Engine = Engine;

        linkExternalFunctions(ctx, Target);

        Target->addOrDeclareMainFunction(builder, Kernel::AddInternal);

        BEGIN_SCOPED_REGION

        NamedRegionTimer T(M->getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        ctx.TargetMachine->setOptLevel(CodeGenOptLevel::Default);
        auto result = cantFail(ctx.Compiler->operator()(*M));


        auto & JITLib = Engine->getMainJITDylib();
        auto err = Engine->addObjectFile(JITLib, std::move(result));
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

        END_SCOPED_REGION

    }

    inline void linkExternalFunctions(CPUDriverContext & ctx, Kernel * Target) {

        Target->linkExternalMethods(*ctx.Builder);

        auto & SL = ctx.NewSymbolList;

        if (!SL.empty()) {
            auto & MainJD = Engine->getMainJITDylib();
            auto err = MainJD.define(orc::absoluteSymbols(SL));
            if (err) {
                handleAllErrors(std::move(err),
                    [](const DuplicateDefinition &) {
                        /* ignored */
                    },
                    [Target](const ErrorInfoBase & err) {
                        SmallVector<char, 100> tmp;
                        raw_svector_ostream msg(tmp);
                        msg << Target->getName() << ": cannot link symbol: " << err.message();
                        report_fatal_error(msg.str());
                    });
            }
            SL.clear();
        }

    }

private:

    llvm::orc::LLJIT *                              Engine;
    ParabixObjectCache * const                      ObjectCache;

    size_t                                          LaneCount;

    std::atomic<size_t>                             NoMoreNewTasks{0};

    size_t                                          ActiveThreads;

    StringMap<Kernel *>                             AlreadyCompiled;

    std::vector<CPUDriverContext *>                 Contexts;

    std::mutex PrecompiledStateObjectMutex;

    std::mutex TaskMutex;
    std::condition_variable TaskCV;

    std::mutex ShutdownMutex;
    std::condition_variable ShutdownCV;

    std::mutex LaneMutex;

    // TODO: decl queue needs to be a priority queue so that we can force more complex
    // kernels (such as the pipeline) to be started sooner.

    std::vector<CPUDriverTaskQueue> Tasks;

    std::vector<std::thread> Threads;

};

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


    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));
    Builder.setNumCompileThreads(0);
    Builder.setCompileFunctionCreator(nullptr);

    mCPUDriverCompiler = std::make_unique<CPUDriverCompiler>(
                                  llvm::hardware_concurrency(numOfThreads),
                                  *Builder.getJITTargetMachineBuilder(), features, *mAllLinkedSymbols,
                                  mObjectCache.get());

    mEngine = cantFail(Builder.create());


    mCPUDriverCompiler->setEngine(mEngine.get());

    auto & ES = mEngine->getExecutionSession();

    ES.setDispatchTask([](std::unique_ptr<Task> T){
        T->run();
    });

    auto & MainJD = mEngine->getMainJITDylib();

    MainJD.addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            mEngine->getDataLayout().getGlobalPrefix()
        ))
    );

    mBuilder.reset(IDISA::GetIDISA_Builder(mMainModule->getContext(), features));
    mBuilder->setModule(mMainModule);
    mBuilder->setFunctionLinkCallback(this);
}

void CPUDriver::generateUncachedKernels() {

    if (mUncachedKernel.empty()) return;

    // TODO: we may be able to reduce unnecessary optimization work by having kernel specific optimization passes.

    // NOTE: we currently require DCE and Mem2Reg for each kernel to eliminate any unnecessary scalar -> value
    // mappings made by the base KernelCompiler. That could be done in a more focused manner, however, as each
    // mapping is known.

    // TODO: we don't want to have more contexts than our thread count will allow

    const auto numKernels = mUncachedKernel.size();

    mCachedKernel.reserve(numKernels);

    const auto layerId = mCPUDriverCompiler->addNewTaskGroup();

    for (unsigned i = 0; i < numKernels; ++i) {
        auto & kernel = mUncachedKernel[i];
        mCPUDriverCompiler->addCompilationTask(1, layerId, kernel.get());
        mCachedKernel.emplace_back(kernel.release());
    }

    mUncachedKernel.clear();

}

void * CPUDriver::finalizeObject(kernel::Kernel * const pk) {

    auto & MainJD = mEngine->getMainJITDylib();

    const auto layerId = mCPUDriverCompiler->addNewTaskGroup();

    mCPUDriverCompiler->addCompilationTask(2, layerId, pk);

    auto mainFuncPtr = mCPUDriverCompiler->waitUntilCompleted(pk);

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

    assert (mainFuncPtr);

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


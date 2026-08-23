#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/DynamicLibrary.h>           // for LoadLibraryPermanently
#include <llvm/ExecutionEngine/ExecutionEngine.h>  // for EngineBuilder
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
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
    Kernel *                                TargetKernel;
    size_t                                  IsCompilingMainFunction;
    std::unique_ptr<llvm::TargetMachine>    TargetMachine;
    std::unique_ptr<KernelBuilder>          Builder;
    std::unique_ptr<SimpleCompiler>         Compiler;
    llvm::Module *                          CurrentModule;
    llvm::orc::LLJIT *                      Engine;
    llvm::orc::SymbolMap &                  SharedSymbolList;
    llvm::orc::SymbolMap                    NewSymbolList;

    CPUDriverContext(std::unique_ptr<LLVMContext> ctx, JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, SymbolMap & symbolList)
    : ThreadSafeContext(std::move(ctx))
    , TargetKernel(nullptr)
    , IsCompilingMainFunction(0)
    , TargetMachine(cantFail(JTMB.createTargetMachine()))
    , Builder(IDISA::GetIDISA_Builder(*getContext(), features))
    , Compiler(std::make_unique<SimpleCompiler>(*TargetMachine, nullptr))
    , CurrentModule(nullptr)
    , Engine(nullptr)
    , SharedSymbolList(symbolList) {
        Builder->setFunctionLinkCallback(this);
        // TODO: does LLVM16 still use setDiagnosticContext?
        getContext()->setDiagnosticHandlerCallBack(nullptr, static_cast<void*>(this));
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

class CPUDriverContextPool {
public:

    CPUDriverContextPool(const size_t count, JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, SymbolMap & symbolList) {
        for (size_t i = 0; i < count; ++i) {
            Contexts.push(std::make_unique<CPUDriverContext>(std::make_unique<LLVMContext>(), JTMB, features, symbolList));
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

struct CPUDriverMaterializationData {
    IRCompileLayer & TargetLayer;
    SymbolDependenceMap & PriorSymbolLayer;
    CPUDriverContextPool & Pool;
    ParabixObjectCache * const ObjectCache;
    LLJIT * const Engine;
};

using PriorityType = uint32_t;

// TODO using InitSymbol for the interface, we can return a result using getInitializerSymbol for the MU.
// Split the kernel gen to have a decl phase and add dependencies between the gen and decl plus the decl and
// all pipeline decl phases. Need to carry kernel compiler with context. This means two or more materialiationunits
// would have to move the same thread safe module. However since we cannot construct the TSM until after the decl unit

class KernelGenerationMU : public orc::MaterializationUnit {
public:
    KernelGenerationMU(Kernel * target, Module * module,
                       ParabixObjectCache * objCache,
                       LLJIT * engine,
                       SymbolFlagsMap && symbols,
                       CPUDriverContextPool & pool)
    : MaterializationUnit(createInterface(target, std::move(symbols)))
    , Target(target)
    , TargetModule(module)
    , ObjectCache(objCache)
    , Engine(engine)
    , Pool(pool) {
        assert (TargetModule);
    }

    StringRef getName() const override {
        return StringRef{"0_", 1};
    }

    void materialize(std::unique_ptr<MaterializationResponsibility> R) override {

        auto ctx = Pool.acquire();

        auto & C = *ctx->getContext();

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
            delete TargetModule;
        }

        auto & builder = *ctx->Builder;
        assert (&builder.getContext() == &C);
        builder.setModule(M);
        ctx->CurrentModule = M;
        ctx->Engine = Engine;

        errs() << "0: K_" << M->getModuleIdentifier() << "\n";

        M->setTargetTriple(Engine->getTargetTriple().getTriple());
        M->setDataLayout(Engine->getDataLayout());

        Target->linkExternalMethods(builder);

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

        BEGIN_SCOPED_REGION
        NamedRegionTimer T(Target->getSignature(), Target->getName(),
                           "Kernel", "Kernel Generation",
                           codegen::TimeKernelsIsEnabled);

        Target->generateKernel(builder, ctx->TargetMachine.get());
        END_SCOPED_REGION

        BEGIN_SCOPED_REGION

        NamedRegionTimer T(M->getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        auto optLevel = CodeGenOptLevel::Default;
        if (LLVM_UNLIKELY(Target->hasAttribute(AttrId::InfrequentlyUsed))) {
            optLevel = codegen::BackEndOptLevel;
        }
        ctx->TargetMachine->setOptLevel(optLevel);
        auto result = ctx->Compiler->operator()(*M);
        Pool.release(ctx);

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            ObjectCache->saveCachedObjectFile(*M, MemoryBufferRef{*result.get()});
        }

        delete M;

        auto & JITLib = Engine->getMainJITDylib();
        cantFail(Engine->addObjectFile(JITLib, std::move(*result)));

        cantFail(R->notifyEmitted());

        END_SCOPED_REGION

    }

    void discard(const JITDylib &, const SymbolStringPtr &) override {
        /* this MU adds the symbols for the IR it has yet to generate. do not discard any symbols. */
    }

    static Interface createInterface(Kernel * const target, SymbolFlagsMap && symbols)  {
        return Interface(std::move(symbols), nullptr);
    }

private:

    Kernel * const Target;
    Module * const TargetModule;
    ParabixObjectCache * const ObjectCache;
    LLJIT * const Engine;
    CPUDriverContextPool & Pool;
};

class KernelDeclarationMU : public orc::MaterializationUnit {
public:
    KernelDeclarationMU(const PriorityType declLayer,
                        Kernel * target, MangleAndInterner & mangler,
                        orc::SymbolLookupSet & lookupSet,
                        ParabixObjectCache * objCache,
                        LLJIT * engine,
                        CPUDriverContextPool & pool)
    : MaterializationUnit(createInterface(target, mangler, lookupSet))
    , Target(target)
    , ObjectCache(objCache)
    , Engine(engine)
    , Pool(pool)
    , DeclLayer(std::to_string(declLayer) + "_") {

    }

    StringRef getName() const override {
        return StringRef{DeclLayer};
    }

    void materialize(std::unique_ptr<MaterializationResponsibility> R) override {

        // TODO: is this safe? we may end up executing this MU many times before the emit'ed layer?

        auto ctx = Pool.acquire();
        assert (&ctx->Builder->getContext() == ctx->getContext());

        auto & builder = *ctx->Builder;

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            auto cached = ObjectCache->loadCachedObjectFile(builder, Target);
            if (cached) {
                Pool.release(ctx);
                auto & JITLib = Engine->getMainJITDylib();
                cantFail(Engine->addObjectFile(JITLib, std::move(cached)));
//                SymbolFlagsMap map;
//                Target->addSymbols(mangler, map, lookupSet);
//                R->notifyResolved(map);
                return;
            }
        }

        Module * const M = Target->makeEmptyModule(builder); assert (M);

        errs() << DeclLayer << ": D_" << M->getModuleIdentifier() << "\n";

        builder.setModule(M);
        ctx->CurrentModule = M;
        ctx->Engine = Engine;

        Target->declareKernel(builder, ctx->TargetMachine.get());

        cantFail(R->notifyEmitted());

        Pool.release(ctx);

        auto & ES = R->getExecutionSession();

        MangleAndInterner mangler(ES, Engine->getDataLayout());
        SymbolFlagsMap symbols;
        Target->addSymbols(mangler, symbols);

       // cantFail(R->defineMaterializing(symbols));

        auto genTask = std::make_unique<KernelGenerationMU>(Target, M, ObjectCache, Engine, std::move(symbols), Pool);

//        auto & MainJD = Engine->getMainJITDylib();
//        cantFail(MainJD.define(std::move(genTask)));

        ES.dispatchTask(std::make_unique<MaterializationTask>(std::move(genTask), std::move(R)));

    }

    void discard(const JITDylib &, const SymbolStringPtr &) override {
        /* this MU adds the symbols for the IR it has yet to generate. do not discard any symbols. */
    }

    static Interface createInterface(Kernel * const target, MangleAndInterner & mangler, orc::SymbolLookupSet & lookupSet)  {
        auto sym = mangler(target->getName());
        lookupSet.add(sym, orc::SymbolLookupFlags::WeaklyReferencedSymbol);
        SymbolFlagsMap symbols{};
        symbols.insert(std::make_pair(sym, JITSymbolFlags::Exported | JITSymbolFlags::MaterializationSideEffectsOnly));
        return Interface(std::move(symbols), sym);
    }

private:

    Kernel * const Target;
    ParabixObjectCache * const ObjectCache;
    LLJIT * const Engine;
    CPUDriverContextPool & Pool;
    const std::string DeclLayer;
};


class MainGenerationMU : public orc::MaterializationUnit {
public:
    MainGenerationMU(const PriorityType declLayer,
                     Kernel * target, SymbolStringPtr mainSymbol, orc::SymbolLookupSet & lookupSet,
                     LLJIT * engine,
                     CPUDriverContextPool & pool)
    : MaterializationUnit(createInterface(mainSymbol, lookupSet))
    , Target(target)
    , Engine(engine)
    , Pool(pool)
    , DeclLayer(std::to_string(declLayer) + "_") {

    }

    StringRef getName() const override {
        return StringRef{DeclLayer};
    }

    void materialize(std::unique_ptr<MaterializationResponsibility> R) override {
        auto ctx = Pool.acquire();
        assert (&ctx->Builder->getContext() == ctx->getContext());

        auto M = std::make_unique<Module>("main", *ctx->getContext());
        M->setTargetTriple(Engine->getTargetTriple().getTriple());
        M->setDataLayout(Engine->getDataLayout());

        errs() << DeclLayer << ": M_" << M->getModuleIdentifier() << "\n";

        auto & builder = *ctx->Builder;
        builder.setModule(M.get());
        ctx->CurrentModule = M.get();
        ctx->Engine = Engine;

        Target->linkExternalMethods(builder);

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

        // Build the "main" module frame context execution pipeline
        Target->addKernelDeclarations(builder, ctx->TargetMachine.get());
        Target->addOrDeclareMainFunction(builder, Kernel::AddInternal);

        BEGIN_SCOPED_REGION

        NamedRegionTimer T(M->getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        ctx->TargetMachine->setOptLevel(CodeGenOptLevel::Default);
        auto result = ctx->Compiler->operator()(*M);
        Pool.release(ctx);

        auto & JITLib = Engine->getMainJITDylib();
        auto err = Engine->addObjectFile(JITLib, std::move(*result));
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

        cantFail(R->notifyEmitted());
    }

    void discard(const JITDylib &, const SymbolStringPtr &) override {
        /* this MU adds the symbols for the IR it has yet to generate. do not discard any symbols. */
    }

    static Interface createInterface(SymbolStringPtr mainSymbol, orc::SymbolLookupSet & lookupSet)  {
        lookupSet.add(mainSymbol, orc::SymbolLookupFlags::RequiredSymbol);
        SymbolFlagsMap symbols{};
        symbols.insert(std::make_pair(mainSymbol, JITSymbolFlags::Exported));
        return Interface(std::move(symbols), mainSymbol);
    }

private:

    Kernel * const Target;
    LLJIT * const Engine;
    CPUDriverContextPool & Pool;
    const std::string DeclLayer;
};

inline void removeAll(SymbolLookupSet & S) {
    auto k = S.size();
    while (k) {
        S.remove(--k);
    }
}

namespace {

class CPUDriverTaskDispatcher : public TaskDispatcher {
    struct TaskQueue {

        TaskQueue(size_t initialCapacity = 64)
        : Head(0), Tail(0), Buffer(initialCapacity, nullptr) {

        }

        void push(Task * task) {
            //std::lock_guard<std::mutex> L(Mutex);
            if (LLVM_UNLIKELY(((Tail + 1U) % Buffer.size()) == Head)) {
                Buffer.resize(Buffer.size() * 2, nullptr);
            }
            Buffer[Tail] = task; assert (task);
            Tail = (Tail + 1U) % Buffer.size();
        }

        bool pop(Task *& out) {
            //std::lock_guard<std::mutex> L(Mutex);
            if (Head == Tail) {
                return false;
            }
            out = Buffer[Head]; assert (out);
            Head = (Head + 1U) % Buffer.size();
            return true;
        }


    private:
        size_t Head;
        size_t Tail;
        std::vector<Task *> Buffer;
        //std::mutex Mutex;
    };

    struct TaskLane {
        TaskQueue Tasks;
        size_t InFlight = 0;
    };

    class ParsePriority : public raw_ostream {
    public:
        ParsePriority() {
            SetUnbuffered();
        }

        void write_impl(const char *Ptr, size_t Size) final {
            assert (Size >= sizeof(PriorityType));
            assert (Pos == 0 && Size >= sizeof(PriorityType) || Pos > 0);
            if (Pos == 0) {
                Value = *reinterpret_cast<const PriorityType*>(Ptr);
            }
            Pos += Size;
        }

        virtual uint64_t current_pos() const final {
            return Pos;
        }

        PriorityType getValue() const {
            return Value;
        }
    private:
        PriorityType Value = 0;
        uint64_t Pos = 0;
    };

public:

    CPUDriverTaskDispatcher(ThreadPoolStrategy strategy)
    : LaneCount(1)
    , Tasks(strategy.ThreadsRequested) {

        // TODO: is a linked list better than the mutex here? we could use an immutable list for the task lane array

        for (size_t i = 0; i < strategy.ThreadsRequested; ++i) {
            Threads.emplace_back([this]() {
                while (Shutdown.load(std::memory_order_acquire) == 0) {

                    Task * toExecute = nullptr;

                    std::unique_lock<std::mutex> L(Mutex);

                    size_t taskIndex = 0;

                    TaskCV.wait(L, [&]{

                        errs() << "TaskCV ...\n";

                        std::lock_guard<std::mutex> R(LaneMutex);

                        errs() << "TaskCV: got read lock\n";

                        assert (LaneCount > 0);

                        for (size_t j = 1; j < LaneCount; ++j) {
                            auto & cur = Tasks[j];
                            if (cur.InFlight) {
                                const auto any = cur.Tasks.pop(toExecute);
                                assert (any == (toExecute != nullptr));
                                errs() << "TaskCV: found " << j << "\n";
                                taskIndex = j;
                                return true;
                            }
                        }

                        auto & cur = Tasks[0];
                        if (cur.InFlight == 0) {
                            return false;
                        }
                        taskIndex = 0;
                        const auto any = cur.Tasks.pop(toExecute);
                        assert (any == (toExecute != nullptr));
                        if (any) {
                            errs() << "TaskCV: found " << 0 << "\n";
                        }
                        return true;
                    });


                    if (toExecute) {

                        errs() << "toExec p_" << taskIndex << "\n";

                        toExecute->run();
                        delete toExecute;
                        std::lock_guard<std::mutex> R(LaneMutex);
                        auto & cur = Tasks[taskIndex];
                        cur.InFlight--;
                    }

                }
            });
        }
    }

    void dispatch(std::unique_ptr<Task> T) override {

        constexpr auto taskPrefix = std::string_view("Materialization task: ");

        SmallVector<char, 256> tmp;
        raw_svector_ostream m(tmp);

        T->printDescription(m);

        const auto str = m.str();

        errs() << "task " << str << "\n";

        if (str.compare(taskPrefix) == 0) {
            const auto s = taskPrefix.length() + 1;
            const auto f = str.find(s, '_');
            const auto num = str.substr(s, f - s);

            const auto priority = std::stoi(num.data());

            // const auto priority = prior.getValue();
            errs() << "dispatch " << priority << "\n";
            BEGIN_SCOPED_REGION
            std::lock_guard<std::mutex> R(LaneMutex);

            errs() << "TaskCV: got dispatch lock\n";

            assert (priority < Tasks.size());
            auto & D = Tasks[priority];
            D.Tasks.push(T.release());
            D.InFlight++;
            END_SCOPED_REGION
            TaskCV.notify_one();
            return;

        }

        T->run();
    }

    size_t addNewTaskGroup() {
        const auto m = Tasks.size();
        if (LaneCount < m) {
            return LaneCount++;
        }
        std::lock_guard<std::mutex> R(LaneMutex);
        Tasks.resize(m * 2);
        return LaneCount++;
    }

    void enqueue(const size_t taskGroupId, std::unique_ptr<MaterializationTask> T) {


    }

    void start() {

    }

    void shutdown() override {
        Shutdown.store(1, std::memory_order_release);
        TaskCV.notify_all();
        for (auto & w : Threads) {
            if (w.joinable()) w.join();
        }
    }

    ~CPUDriverTaskDispatcher() {
        shutdown();
    }

private:




private:

    std::atomic<size_t>  Shutdown{0};

    std::mutex Mutex;
    std::condition_variable TaskCV;

    std::mutex LaneMutex;
    size_t LaneCount;
    std::vector<TaskLane> Tasks;

    size_t TotalPending = 0;

    std::vector<std::thread> Threads;
};

class TaskPlatform : public Platform {

/// MaterializationUnit is added to a JITDylib.
Error notifyAdding(ResourceTracker &RT, const MaterializationUnit &MU) override {


}

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


    const size_t numOfThreads = 1;

    mAllLinkedSymbols = std::make_unique<SymbolMap>();

    mContextPool = std::make_unique<CPUDriverContextPool>(numOfThreads, JTMB, features, *mAllLinkedSymbols);

    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));
    Builder.setNumCompileThreads(0);
    Builder.setCompileFunctionCreator(nullptr);
    auto dispatcher = std::make_unique<CPUDriverTaskDispatcher>(llvm::hardware_concurrency(numOfThreads));
    mTaskDispatcher = dispatcher.get();
    auto epc = SelfExecutorProcessControl::Create(nullptr, std::move(dispatcher));
    Builder.setExecutorProcessControl(std::move(*epc));



    // Safely route the compilation process through your customized Parabix caching system
//    Builder.setCompileFunctionCreator([&](llvm::orc::JITTargetMachineBuilder InnerJTMB)
//        -> Expected<std::unique_ptr<llvm::orc::IRCompileLayer::IRCompiler>> {
//            return std::make_unique<CPUDriverKernelCompiler>(InnerJTMB, mObjectCache.get());
//    });

    mEngine = cantFail(Builder.create());


    auto & ES = mEngine->getExecutionSession();

    ES.setDispatchTask([this](std::unique_ptr<Task> T){
        mTaskDispatcher->dispatch(std::move(T));
    });



//    auto & CL = mEngine->getIRCompileLayer();
//    auto & CC = reinterpret_cast<CPUDriverKernelCompiler &>(CL.getCompiler());

//    CC.setPool(mContextPool.get());
//    CC.setEngine(mEngine.get());

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

    auto & MainJD = mEngine->getMainJITDylib();

    auto & ES = mEngine->getExecutionSession();

    ES.setErrorReporter([](Error err) {
        logAllUnhandledErrors(std::move(err), errs(), "LLJIT Session Error: ");
    });

    MangleAndInterner Mangler(ES, mEngine->getDataLayout());

    mCachedKernel.reserve(numKernels);

    const auto layerId = mTaskDispatcher->addNewTaskGroup();

    errs() << "Generating " << numKernels << " Layer " << layerId << " Kernels\n";

    for (unsigned i = 0; i < numKernels; ++i) {
        auto & kernel = mUncachedKernel[i];

        errs() << "ADDING " << layerId << "  " << kernel->getName() << "\n";



        auto declTask = std::make_unique<KernelDeclarationMU>(layerId, kernel.get(), Mangler, *mSymbolLookupSet, mObjectCache.get(), mEngine.get(), *mContextPool);
        cantFail(MainJD.define(declTask));
        assert (!mSymbolLookupSet->containsDuplicates());
        mCachedKernel.emplace_back(kernel.release());
    }



    mUncachedKernel.clear();

//    auto S = makeJITDylibSearchOrder({&MainJD}, JITDylibLookupFlags::MatchExportedSymbolsOnly);
//    cantFail(ES.lookup(S, *mSymbolLookupSet, LookupKind::Static, SymbolState::Ready));
//    removeAll(*mSymbolLookupSet);

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

    const auto layerId = mTaskDispatcher->addNewTaskGroup();

    errs() << "Generating main at Layer " << layerId << "\n";

    auto & ES = mEngine->getExecutionSession();

    auto mainDecl = std::make_unique<MainGenerationMU>(layerId, pk, mainSymbol, *mSymbolLookupSet, mEngine.get(), *mContextPool);
  //  ES.dispatchTask(std::make_unique<MaterializationTask>(std::move(mainDecl), std::make_unique<MaterializationResponsibility>()));

    cantFail(MainJD.define(mainDecl));
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

    auto S = makeJITDylibSearchOrder({&MainJD}, JITDylibLookupFlags::MatchExportedSymbolsOnly);
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


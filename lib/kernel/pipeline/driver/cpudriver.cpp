#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/DynamicLibrary.h>           // for LoadLibraryPermanently
#include <llvm/ExecutionEngine/ExecutionEngine.h>  // for EngineBuilder
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/ObjectFileInterface.h>
#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
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
#include <llvm/IR/Mangler.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/PassTimingInfo.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <queue>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#include <numeric>

#include <boost/interprocess/mapped_region.hpp>
#include <allocator/threadsafe_slaballocator.h>

inline unsigned getPageSize() {
    return boost::interprocess::mapped_region::get_page_size();
}

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

// TODO: if a task dependency system exists, we could split the task of state identification from codegen but
// could not guarantee that the same thread/context would process it. Most kernel state types are defined fully
// in their constructors. The pipeline is the only know exception. Thus very few dependencies would be needed.


namespace {

struct CPUDriverContext : public LLVMContext, public FunctionLinkCallback {
    std::unique_ptr<llvm::TargetMachine>    TargetMachine;
    std::unique_ptr<KernelBuilder>          Builder;
    llvm::Module *                          CurrentModule;
    llvm::orc::LLJIT *                      Engine;
    llvm::DataLayout                        DataLayout;
    llvm::orc::SymbolMap &                  SharedSymbolList;
    llvm::orc::SymbolMap                    NewSymbolList;

    CPUDriverContext(JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, SymbolMap & symbolList)
    : TargetMachine(cantFail(JTMB.createTargetMachine()))
    , Builder(IDISA::GetIDISA_Builder(*this, features))
    , CurrentModule(nullptr)
    , Engine(nullptr)
    , DataLayout(TargetMachine->createDataLayout())
    , SharedSymbolList(symbolList) {
        Builder->setFunctionLinkCallback(this);
    }

    llvm::Function * LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
        Function * f = CurrentModule->getFunction(unmangledName);
        if (LLVM_UNLIKELY(f == nullptr)) {
            FunctionType * funcTy = cast<FunctionType>(CBuilder::convertTypeToLLVMContext(*this, functionType));
            f = Function::Create(funcTy, Function::ExternalLinkage, unmangledName, CurrentModule);
            assert (Engine);
            auto & ES = Engine->getExecutionSession();
            auto addr = orc::ExecutorAddr::fromPtr(functionPointer);
            MangleAndInterner M(ES, Engine->getDataLayout());
            const auto flags = JITSymbolFlags::Exported | JITSymbolFlags::Callable;
            NewSymbolList.insert(std::make_pair(M(unmangledName), ExecutorSymbolDef{addr, flags}));
        }
        return f;
    }

    bool HasExternalFunction(llvm::StringRef unmangledName) const {
        auto & ES = Engine->getExecutionSession();
        MangleAndInterner mangler(ES, Engine->getDataLayout());
        SymbolLookupSet syms;
        syms.add(mangler(unmangledName));
        auto result = ES.lookup(makeJITDylibSearchOrder({&Engine->getMainJITDylib()}), syms,
                                LookupKind::Static, SymbolState::Ready, NoDependenciesToRegister);
        if (!result) {
            consumeError(result.takeError());
            return false;
        }
        return true;
    }

    virtual ~CPUDriverContext() {}
};

enum class CPUDriverTaskType : size_t {
    ObjectCode = 0,
    Declaration = 1,
    MainFunction = 2
};

struct CPUDriverTask {
    Kernel * Target;
    Module * TargetModule;
    CPUDriverTaskType   TypeId;

    CPUDriverTask() = default;

    CPUDriverTask(CPUDriverTaskType typeId, Kernel * target, Module * module) : Target(target), TargetModule(module), TypeId(typeId) { }

    CPUDriverTask & operator=(const CPUDriverTask&) = default;

};



struct CircularBuffer {

    CircularBuffer(const size_t capacity)
    : Head(0), Tail(0), Count(0), Buffer(capacity) {

    }

    void push(CPUDriverTask & task) {
        assert (task.Target);
        assert (Count <= Buffer.size());
        const auto m = Buffer.size();
        if (LLVM_UNLIKELY(Count == m)) {
            std::vector<CPUDriverTask> buffer2(m * 2);
            assert (Head < m);
            for (size_t i = 0; i < m; ++i) {
                buffer2[Head + i] = Buffer[(Head + i) % m];
            }
            Buffer.swap(buffer2);
            Tail = Head + m;

//            Buffer.resize(m * 2);
//            for (size_t i = m; i-- > Head; ) {
//                Buffer[m + i] = Buffer[i];
//            }
//            Head += m;
//            Tail = Count;
        }
        Buffer[Tail] = task;
        Tail = (Tail + 1) % Buffer.size();
        ++Count;
    }

    bool pop(CPUDriverTask & out) {
        if (Count == 0) {
            return false;
        }
        --Count;
        out = Buffer[Head];
        assert (out.Target);
        Head = (Head + 1) % Buffer.size();
        return true;
    }

private:
    size_t Head;
    size_t Tail;
    size_t Count;
    std::vector<CPUDriverTask> Buffer;
};

struct CircularTaskBuffer : public CircularBuffer {

    CircularTaskBuffer(const size_t capacity)
    : CircularBuffer(capacity), InFlight(0) {

    }

    size_t InFlight;
};

class CPUDriverWorkQueue {
    constexpr static size_t OBJECT_QUEUE_PRIORITY_LEVELS = 3;
public:
    CPUDriverWorkQueue(size_t initialCapacity)
    : ObjectQueues({{CircularBuffer(32), CircularBuffer(8), CircularBuffer(2)}}) {
        TaskQueues.reserve(initialCapacity);
    }

    size_t addNewLevel(const size_t capacity) {
        std::lock_guard<std::mutex> L(mutex);
        const auto s = TaskQueues.size();
        TaskQueues.emplace_back(capacity);
        return s;
    }

    inline void requeueTask(const size_t level, CPUDriverTask task) {
        std::lock_guard<std::mutex> L(mutex);
        assert (level < TaskQueues.size());
        auto & Q = TaskQueues[level];
        Q.push(task);
        if (Sleeping) cv.notify_one();
    }

    inline void pushTask(const size_t level, CPUDriverTask task) {
        std::lock_guard<std::mutex> L(mutex);
        assert (level < TaskQueues.size());
        auto & Q = TaskQueues[level];
        Q.push(task);
        Q.InFlight++;
        if (Sleeping) cv.notify_one();
    }

    inline void pushObject(CPUDriverTask task) {
        std::lock_guard<std::mutex> L(mutex);
        auto & Q = ObjectQueues[task.Target->getCompilationPriority()];
        Q.push(task);
        if (Sleeping) cv.notify_one();
    }

    bool pop(CPUDriverTask & task, size_t & level) {
        std::unique_lock<std::mutex> L(mutex);
        for (;;) {
            auto done = StopRequested;
            const auto n = TaskQueues.size();
            for (size_t i = 0; i < n; ++i) {
                auto & Q = TaskQueues[i];
                if (Q.InFlight) {
                    if (Q.pop(task)) {
                        level = i;
                        return true;
                    }
                    done = false;
                    break;
                }
            }

            for (size_t i = OBJECT_QUEUE_PRIORITY_LEVELS; i--; ) {
                auto & Q = ObjectQueues[i];
                if (Q.pop(task)) {
                    assert (task.TypeId == CPUDriverTaskType::ObjectCode);
                    #ifndef NDEBUG
                    level = std::numeric_limits<size_t>::max();
                    #endif
                    return true;
                }
            }

            if (LLVM_UNLIKELY(done)) {
                return false;
            }

            Sleeping++;
            cv.wait(L);
            assert (Sleeping > 0);
            --Sleeping;
        }
    }

    void decrementTaskFlightCount(const size_t level) {
        std::lock_guard<std::mutex> L(mutex);
        assert (level < TaskQueues.size());
        auto & Q = TaskQueues[level];
        assert (Q.InFlight > 0);
        Q.InFlight--;
        if (Sleeping) cv.notify_all();
    }

    void noMoreTasks() {
        std::lock_guard<std::mutex> L(mutex);
        StopRequested = 1;
        if (Sleeping) cv.notify_all();
    }


private:
    std::array<CircularBuffer, OBJECT_QUEUE_PRIORITY_LEVELS> ObjectQueues;
    std::vector<CircularTaskBuffer> TaskQueues;
    std::mutex mutex;
    std::condition_variable cv;
    size_t Sleeping = 0;
    size_t StopRequested = 0;
};


struct DebugPrintingResult {
    const Kernel * Target;
    SmallVector<char, 0> UnoptimizedIR;
    SmallVector<char, 0> OptimizedIR;
    SmallVector<char, 0> ASM;

    DebugPrintingResult(const Kernel * target, SmallVector<char, 0> && unoptIR, SmallVector<char, 0> && optIR, SmallVector<char, 0> && asmOutput)
    : Target(target)
    , UnoptimizedIR(std::move(unoptIR))
    , OptimizedIR(std::move(optIR))
    , ASM(std::move(asmOutput)) {

    }
};


class CPUDriverCompiler {

    using MemoryBufferVector = std::vector<std::unique_ptr<MemoryBuffer>>;

public:

    CPUDriverCompiler(ThreadPoolStrategy strategy,
                      JITTargetMachineBuilder & JTMB, const StringMap<bool> & features, SymbolMap & symbolList,
                      ParabixObjectCache * objCache)
    : Engine(nullptr)
    , ObjectCache(objCache)
    , WorkQueue(8)
    , Contexts(strategy.ThreadsRequested, nullptr) {

        // NOTE: THe pipeline may read information from other kernels to determine their state types and convert them
        // to its own LLVMContext as needed. We preserve the contexts as long as this compiler exists.
        for (size_t i = 0; i < strategy.ThreadsRequested; ++i) {
            Contexts[i] = new CPUDriverContext(JTMB, features, symbolList);
        }

        for (size_t i = 0; i < strategy.ThreadsRequested; ++i) {
            CPUDriverContext & ctx = *(Contexts[i]);
            Threads.emplace_back([this, &ctx]() {
                // NOTE: the ThreadSafeSlabAllocator itself is not passed into any class but is used invisibly
                // behind the scenes. Do not remove this!

                ThreadSafeSlabAllocator _allocator;
                for (;;) {

                    CPUDriverTask toExecute;
                    size_t taskIndex = 0;

                    if (LLVM_LIKELY(WorkQueue.pop(toExecute, taskIndex))) {

                        switch (toExecute.TypeId) {
                            case CPUDriverTaskType::ObjectCode:
                                assert (taskIndex == std::numeric_limits<size_t>::max());
                                materializeObject(ctx, toExecute.Target, toExecute.TargetModule);
                                addFinalObjectCodeToLLJIT();
                                break;
                            case CPUDriverTaskType::Declaration:
                                assert (taskIndex != std::numeric_limits<size_t>::max());
                                if (LLVM_LIKELY(materializeDecl(ctx, taskIndex, toExecute.Target))) {
                                    WorkQueue.decrementTaskFlightCount(taskIndex);
                                }
                                break;
                            case CPUDriverTaskType::MainFunction:
                                assert (taskIndex != std::numeric_limits<size_t>::max());
                                materializeMain(ctx, toExecute.Target);
                                WorkQueue.decrementTaskFlightCount(taskIndex);
                                break;
                        }

                    } else {
                        return;
                    }

                }
            });
        }
    }

    size_t addNewTaskGroup(const size_t capacity) {
        return WorkQueue.addNewLevel(capacity);
    }

    void addCompilationTask(const size_t level, Kernel * const kernel) {
        CPUDriverTask S{CPUDriverTaskType::Declaration, kernel, nullptr};
        WorkQueue.pushTask(level, S);
    }

    void addMainFunctionTask(const size_t level, Kernel * const kernel) {
        CPUDriverTask S{CPUDriverTaskType::MainFunction, kernel, nullptr};
        WorkQueue.pushTask(level, S);
    }

    void * waitUntilCompleted(Kernel * const Target) {

        WorkQueue.noMoreTasks();
        for (auto & t : Threads) {
            if (t.joinable()) t.join();
        }
        addFinalObjectCodeToLLJIT();
        for (auto & C : Contexts) {
            auto & S = C->NewSymbolList;
            DriverLinkedSymbols->insert(S.begin(), S.end());
        }

        if (!DriverLinkedSymbols->empty()) {
            auto & MainJD = Engine->getMainJITDylib();
            auto err = MainJD.define(orc::absoluteSymbols(*DriverLinkedSymbols));
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
        }

        auto & JITLib = Engine->getMainJITDylib();
        SmallVector<char, 256> tmp;
        raw_svector_ostream mainName(tmp);
        mainName << Target->getName() << "_main";
        auto sym = Engine->lookup(JITLib, mainName.str());
        if (!sym) {
            report_fatal_error(sym.takeError());
        }
        printDebugOutput();
        return sym->toPtr<void*>();

    }

    void setEngine(orc::LLJIT * engine) {
        Engine = engine;
    }

    void setDriverLinkedSymbolMap(llvm::orc::SymbolMap * symMap) {
        DriverLinkedSymbols = symMap;
    }

    ~CPUDriverCompiler() {
        for (auto & C : Contexts) {
            delete C;
        }
    }

private:

    bool materializeDecl(CPUDriverContext & ctx, const size_t declLayer, Kernel * Target) {

        std::string tmp;
        raw_string_ostream nm(tmp);
        nm << Target->getName();
        if (Target->hasSignature()) {
            nm << '\0' << Target->getSignature();
        }
        const auto sig = nm.str();

        BEGIN_SCOPED_REGION
        bool added;
        StringMap<Kernel *>::iterator itr;

        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(PrecompiledStateObjectMutex);
        std::tie(itr, added) = AlreadyCompiled.insert(std::make_pair(sig, nullptr));
        END_SCOPED_REGION

        if (LLVM_UNLIKELY(!added)) {
            const auto other = itr->getValue();
            // TODO: this isn't safe if we want to discard kernels on restart
            if (other) {
                Target->setSharedStateType(other->getSharedStateType());
                Target->setThreadLocalStateType(other->getThreadLocalStateType());
                return true;
            } else {
                // We haven't yet finished declaring the other instance of this one. Re-add this
                // to the queue and hope we can continue processing after.
                CPUDriverTask task(CPUDriverTaskType::Declaration, Target, nullptr);
                WorkQueue.requeueTask(declLayer, task);
                return false;
            }
        }
        END_SCOPED_REGION

        assert (&ctx.Builder->getContext() == &ctx);

        auto & builder = *ctx.Builder;

        TargetMachine * const TM = ctx.TargetMachine.get();

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            std::unique_ptr<MemoryBuffer> cached;
            std::unique_ptr<Module> M;
            std::tie(cached, M) = ObjectCache->loadCachedObjectFile(builder, Target);
            if (M) {

                M->setTargetTriple(TM->getTargetTriple().getTriple());
                M->setDataLayout(ctx.DataLayout);

                Target->loadCachedKernel(M.get());
                builder.setModule(M.get());
                ctx.CurrentModule = M.get();
                ctx.Engine = Engine;

                Target->linkExternalMethods(builder);

                BEGIN_SCOPED_REGION
                std::lock_guard<std::mutex> L(AddObjectCodeMutex);
                AddObjectCodeList.emplace_back(std::move(cached));
                END_SCOPED_REGION
                goto record_decl;
            }
        }

        BEGIN_SCOPED_REGION
        Module * const M = Target->makeEmptyModule(builder); assert (M);
        M->setTargetTriple(TM->getTargetTriple().getTriple());
        M->setDataLayout(ctx.DataLayout);
        builder.setModule(M);
        ctx.CurrentModule = M;
        ctx.Engine = Engine;
        Target->declareStateTypes(builder);
        CPUDriverTask task(CPUDriverTaskType::ObjectCode, Target, M);
        WorkQueue.pushObject(task);
        END_SCOPED_REGION

record_decl:
        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(PrecompiledStateObjectMutex);
        auto f = AlreadyCompiled.find(sig);
        assert (f != AlreadyCompiled.end());
        assert (f->second == nullptr);
        f->second = Target;
        END_SCOPED_REGION
        return true;
    }

    void materializeObject(CPUDriverContext & C, Kernel * Target, Module * TargetModule) {

        // We can't be sure that the context associated with the decl is the same
        // one that we acquired here. However since we cannot control the order of
        // which materialization tasks will fire, it's better to just regenerate the
        // state type metadata as needed or we risk deadlocking the system.
        Module * M = TargetModule; assert (M);
        TargetMachine * const TM = C.TargetMachine.get();
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

        auto & builder = *C.Builder;
        assert (&builder.getContext() == &C);
        builder.setModule(M);
        C.CurrentModule = M;
        C.Engine = Engine; assert (Engine);
        M->setTargetTriple(TM->getTargetTriple().getTriple());
        M->setDataLayout(C.DataLayout);

//        M->setTargetTriple(Engine->getTargetTriple().getTriple());
//        M->setDataLayout(Engine->getDataLayout());

        Target->linkExternalMethods(builder);
        //linkExternalFunctions(C, Target);

        SmallVector<char, 0> IROutput;
        SmallVector<char, 0> OptIROutput;

        BEGIN_SCOPED_REGION
        NamedRegionTimer T(Target->getSignature(), Target->getName(),
                           "Kernel", "Kernel Generation",
                           codegen::TimeKernelsIsEnabled);


        Target->generateKernel(builder, TM, GlobalValue::ExternalLinkage);

        Kernel::SelectedOptimizationPasses passes;
        Target->addOptimizationPasses(builder, passes);
        BaseDriver::runAllOptimizationPasses(builder, passes, TM, IROutput, OptIROutput);

        END_SCOPED_REGION

        auto optLevel = CodeGenOptLevel::Default;
        if (LLVM_UNLIKELY(Target->hasAttribute(AttrId::InfrequentlyUsed))) {
            optLevel = codegen::BackEndOptLevel;
        }
        TM->setOptLevel(optLevel);

        legacy::PassManager PM;

        SmallVector<char, 0> ASMOutput;
        SmallVector<char, 0> objBuffer;


        std::unique_ptr<SmallVectorMemoryBuffer> objCode;

        BEGIN_SCOPED_REGION

        NamedRegionTimer T(M->getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        const auto ic = M->getInstructionCount();

        if (LLVM_UNLIKELY(codegen::ShowASMOption != codegen::OmittedOption)) {

            ASMOutput.reserve(ic * 64);

            raw_svector_ostream out(ASMOutput);

            #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
            constexpr auto ASMFile = CodeGenFileType::AssemblyFile;
            #else
            constexpr auto ASMFile = CGFT_AssemblyFile;
            #endif
            if (LLVM_UNLIKELY(TM->addPassesToEmitFile(PM, out, nullptr, ASMFile))) {
                report_fatal_error(Twine{"Failed to generate ASM for ", M->getModuleIdentifier()});
            }

        }

        objBuffer.reserve(4096 + ic * 16);
        raw_svector_ostream out(objBuffer);


        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
        constexpr auto ObjFile = CodeGenFileType::ObjectFile;
        #else
        constexpr auto ObjFile = CGFT_ObjectFile;
        #endif

        if (LLVM_UNLIKELY(TM->addPassesToEmitFile(PM, out, nullptr, ObjFile))) {
            report_fatal_error(Twine{"Failed to generate object file for ", M->getModuleIdentifier()});
        }

        PM.run(*M);

        objCode = std::make_unique<SmallVectorMemoryBuffer>(std::move(objBuffer), false);

        END_SCOPED_REGION

        assert (objCode.get());

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            ObjectCache->saveCachedObjectFile(*M, objCode->getMemBufferRef());
        }

        delete M;

        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(AddObjectCodeMutex);
        AddObjectCodeList.emplace_back(std::move(objCode));
        END_SCOPED_REGION

        if (LLVM_UNLIKELY(IROutput.size() || OptIROutput.size() || ASMOutput.size())) {
            recordDebugPrintResult(Target, std::move(IROutput), std::move(OptIROutput), std::move(ASMOutput));
        }
    }

    void materializeMain(CPUDriverContext & C, Kernel * Target) {

        TargetMachine * const TM = C.TargetMachine.get();
        auto M = std::make_unique<Module>("main", C);
//        M->setTargetTriple(Engine->getTargetTriple().getTriple());
//        M->setDataLayout(Engine->getDataLayout());
        M->setTargetTriple(TM->getTargetTriple().getTriple());
        M->setDataLayout(C.DataLayout);

        KernelBuilder & builder = *C.Builder;
        builder.setModule(M.get());
        C.CurrentModule = M.get();
        C.Engine = Engine;

        Target->linkExternalMethods(builder);

        Target->addOrDeclareMainFunction(builder, Kernel::AddInternal);

        BEGIN_SCOPED_REGION

        NamedRegionTimer T(M->getModuleIdentifier(), "",
                           "Module", "Object Generation",
                           codegen::TimeKernelsIsEnabled);

        C.TargetMachine->setOptLevel(CodeGenOptLevel::Default);

        SmallVector<char, 0> objBuffer;
        objBuffer.reserve(4096 + M->getInstructionCount() * 16);
        raw_svector_ostream out(objBuffer);


        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
        constexpr auto ObjFile = CodeGenFileType::ObjectFile;
        #else
        constexpr auto ObjFile = CGFT_ObjectFile;
        #endif

        legacy::PassManager PM;

        TargetMachine * const TM = C.TargetMachine.get();

        if (LLVM_UNLIKELY(TM->addPassesToEmitFile(PM, out, nullptr, ObjFile))) {
            report_fatal_error(Twine{"Failed to generate object file for ", M->getModuleIdentifier()});
        }

        PM.run(*M);

        auto result = std::make_unique<SmallVectorMemoryBuffer>(std::move(objBuffer), false);

        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(AddObjectCodeMutex);
        AddObjectCodeList.emplace_back(std::move(result));
        END_SCOPED_REGION

        END_SCOPED_REGION

    }

    bool addFinalObjectCodeToLLJIT() {
        size_t e = 0;
        if (!AddObjectCodeInProcess.compare_exchange_weak(e, 1, std::memory_order_release, std::memory_order_relaxed)) {
            return false;
        }
        MemoryBufferVector objCodeList;
        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(AddObjectCodeMutex);
        if (AddObjectCodeList.empty()) {
            return false;
        }
        objCodeList.swap(AddObjectCodeList);
        END_SCOPED_REGION
        for (auto & buffer : objCodeList) {
            cantFail(Engine->addObjectFile(std::move(buffer)));
        }
        AddObjectCodeInProcess.store(0, std::memory_order_release);
        return true;
    }

    void recordDebugPrintResult(Kernel * const kernel, SmallVector<char, 0> && UnoptIR, SmallVector<char, 0> && OptIR, SmallVector<char, 0> && ASM) {
        std::lock_guard<std::mutex> L(DebugPrintMutex);
        DebugPrintResults.emplace_back(kernel, std::move(UnoptIR), std::move(OptIR), std::move(ASM));
    }

    void printDebugOutput() const {
        const auto m = DebugPrintResults.size();
        if (m == 0) {
            return;
        }
        std::vector<size_t> indices(m);
        std::iota(indices.begin(), indices.end(), 0);
        std::sort(indices.begin(), indices.end(), [this](const size_t i, const size_t j) -> bool {
            const auto & A = DebugPrintResults[i];
            const auto & B = DebugPrintResults[j];
            return A.Target->getName().compare(B.Target->getName()) < 0;
        });

        auto makeFdStream = [](const std::string & option) {
            if (LLVM_UNLIKELY(option!= codegen::OmittedOption)) {
                if (option.empty()) {
                    return std::make_unique<raw_fd_ostream>(STDERR_FILENO, false, true);
                } else {
                    std::error_code err;
                    return std::make_unique<raw_fd_ostream>(option, err, sys::fs::OpenFlags::OF_None);
                }
            }
            return std::unique_ptr<raw_fd_ostream>();
        };

        auto unoptimizedOut = makeFdStream(codegen::ShowUnoptimizedIROption);
        auto optimizedOut = makeFdStream(codegen::ShowIROption);
        auto asmOut = makeFdStream(codegen::ShowASMOption);

        for (size_t i = 0; i < m; ++i) {
            const auto & R = DebugPrintResults[indices[i]];
            if (R.UnoptimizedIR.size()) {
                *unoptimizedOut << R.UnoptimizedIR << "\n\n";
            }
            if (R.OptimizedIR.size()) {
                *optimizedOut << R.OptimizedIR << "\n\n";
            }
            if (R.ASM.size()) {
                *asmOut << R.ASM << "\n\n";
            }
        }
    }

private:

    llvm::orc::LLJIT *                              Engine;
    ParabixObjectCache * const                      ObjectCache;

    llvm::orc::SymbolMap *                          DriverLinkedSymbols;

    CPUDriverWorkQueue                              WorkQueue;

    struct CompiledStateTypes {
        StructType * SharedTy = nullptr;
        StructType * ThreadLocalTy = nullptr;
        size_t Compiled = 0;
        CompiledStateTypes() = default;
        CompiledStateTypes(StructType * shared, StructType * threadLocal) : SharedTy(shared), ThreadLocalTy(threadLocal) {}
    };

    StringMap<Kernel *>                             AlreadyCompiled;

    std::vector<CPUDriverContext *>                 Contexts;

    std::mutex                                      PrecompiledStateObjectMutex;

    std::atomic<size_t>                             AddObjectCodeInProcess;
    std::mutex                                      AddObjectCodeMutex;
    MemoryBufferVector                              AddObjectCodeList;

    std::vector<std::thread>                        Threads;

    std::mutex                                      DebugPrintMutex;
    SmallVector<DebugPrintingResult, 0>             DebugPrintResults;

};

} // end of anon namespace

ATTRIBUTE_NO_SANITIZE_ADDRESS
CPUDriver::CPUDriver(std::string && moduleName)
: BaseDriver(std::move(moduleName))
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

    Builder.setObjectLinkingLayerCreator([](ExecutionSession & ES, const Triple & TT) {
        auto objLinker = std::make_unique<ObjectLinkingLayer>(ES, std::make_unique<jitlink::InProcessMemoryManager>(getPageSize()));
        objLinker->setAutoClaimResponsibilityForObjectSymbols(true);
        return objLinker;
    });

    mCPUDriverCompiler = std::make_unique<CPUDriverCompiler>(
                                  llvm::hardware_concurrency(numOfThreads),
                                  *Builder.getJITTargetMachineBuilder(), features, *mAllLinkedSymbols,
                                  mObjectCache.get());

    mEngine = cantFail(Builder.create());


    mCPUDriverCompiler->setEngine(mEngine.get());

    mCPUDriverCompiler->setDriverLinkedSymbolMap(mAllLinkedSymbols.get());

    auto & ES = mEngine->getExecutionSession();

    ES.setDispatchTask([](std::unique_ptr<Task> T) {
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

    const auto layerId = mCPUDriverCompiler->addNewTaskGroup(numKernels);
    for (unsigned i = 0; i < numKernels; ++i) {
        auto & kernel = mUncachedKernel[i];
        mCPUDriverCompiler->addCompilationTask(layerId, kernel.get());
        mCachedKernel.emplace_back(kernel.release());
    }

    mUncachedKernel.clear();

}

void * CPUDriver::finalizeObject(kernel::Kernel * const pk) {

    const auto layerId = mCPUDriverCompiler->addNewTaskGroup(1);

    mCPUDriverCompiler->addMainFunctionTask(layerId, pk);

    auto mainFuncPtr = mCPUDriverCompiler->waitUntilCompleted(pk);


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
        MangleAndInterner M(mEngine->getExecutionSession(), mEngine->getDataLayout());
        auto symbol = M(unmangledName);
        auto addr = orc::ExecutorAddr::fromPtr(functionPointer);
        mAllLinkedSymbols->insert(std::make_pair(symbol, ExecutorSymbolDef{addr, JITSymbolFlags::Exported}));
    }
    return f;
}

bool CPUDriver::HasExternalFunction(llvm::StringRef functionName) const {
    auto & ES = mEngine->getExecutionSession();
    MangleAndInterner mangler(ES, mEngine->getDataLayout());
    SymbolLookupSet syms;
    syms.add(mangler(functionName));
    auto result = ES.lookup(makeJITDylibSearchOrder({&mEngine->getMainJITDylib()}), syms,
                            LookupKind::Static, SymbolState::Ready, NoDependenciesToRegister);
    if (!result) {
        consumeError(result.takeError());
        return false;
    }
    return true;
}

CPUDriver::~CPUDriver() {
    cantFail(mEngine->getExecutionSession().endSession());
}


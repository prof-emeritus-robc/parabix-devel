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
#include <llvm/CodeGen/TargetPassConfig.h>
#include <idisa/passes/function_snippet.h>
#include <llvm/CodeGen/Passes.h>
#include <queue>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#include <numeric>
#include <allocator/threadsafe_slaballocator.h>

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


struct CPUDriverContext : public LLVMContext, public FunctionLinkCallback {
    std::unique_ptr<llvm::TargetMachine>    TargetMachine;
    std::unique_ptr<KernelBuilder>          Builder;
    llvm::Module *                          CurrentModule;
    llvm::orc::LLJIT *                      Engine;
    llvm::DataLayout                        DataLayout;
    llvm::orc::SymbolMap                    NewSymbolList;

    CPUDriverContext(JITTargetMachineBuilder & JTMB, const StringMap<bool> & features)
    : TargetMachine(cantFail(JTMB.createTargetMachine()))
    , Builder(IDISA::GetIDISA_Builder(*this, features))
    , CurrentModule(nullptr)
    , Engine(nullptr)
    , DataLayout(TargetMachine->createDataLayout()) {
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
        const auto m = Buffer.size(); assert (m);
        if (LLVM_UNLIKELY(Count == m)) {
            Buffer.resize(m * 2);
            if (Head <= Tail) {
                std::move_backward(Buffer.begin() + Tail, Buffer.begin() + m, Buffer.begin() + m * 2);
                Tail += m;
            }
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


class CPUDriverJITMemoryManager : public jitlink::JITLinkMemoryManager {
    using Mem = llvm::sys::Memory;
    using Block = llvm::sys::MemoryBlock;

    constexpr static uintptr_t DEFAULT_SLAB_SIZE = 10ULL * 1024ULL * 1024ULL;

    struct SlabNode {
        Block Slab;
        std::atomic<uintptr_t> AllocatedOffset;
        SlabNode * Next;

        SlabNode(const size_t size)
        : Slab([size](){
            std::error_code EC;
            const auto alignedSize = llvm::alignToPowerOf2(size, CBuilder::PAGE_SIZE);
            auto slab = Mem::allocateMappedMemory(alignedSize, nullptr, Mem::MF_READ | Mem::MF_WRITE, EC);
            if (EC) {
                SmallVector<char, 256> tmp;
                raw_svector_ostream msg(tmp);
                msg << "JITMemoryManager: failed to allocate "
                    << size << " bytes: " << llvm::errorCodeToError(EC);
                report_fatal_error(msg.str());
            }
            return slab;
        }())
        , AllocatedOffset(reinterpret_cast<uintptr_t>(Slab.base()))
        , Next(nullptr) {

        }

        ~SlabNode() {
            Mem::releaseMappedMemory(Slab);
        }

    };

public:

    CPUDriverJITMemoryManager()
    : ExecSlab(DEFAULT_SLAB_SIZE)
    , CurrentExecSlab(&ExecSlab)
    , DataSlab(DEFAULT_SLAB_SIZE)
    , CurrentDataSlab(&DataSlab) {

    }

    void allocate(const jitlink::JITLinkDylib * JD, jitlink::LinkGraph & G, OnAllocatedFunction onAllocated) override {
        size_t totalDataSize = 0;
        uint64_t firstDataAlign = 0;
        size_t totalExecSize = 0;
        uint64_t firstExecAlign = 0;

        std::vector<const char*> originalPosition;

        for (auto & S : G.sections()) {

            const auto isExec = (S.getMemProt() & MemProt::Exec) != MemProt::None;

            auto firstAlign = isExec ? firstExecAlign : firstDataAlign;
            auto totalSize = isExec ? totalExecSize : totalDataSize;


            for (auto & B : S.blocks()) {

                originalPosition.push_back(B->getContent().data());


                const auto align = B->getAlignment(); assert (align);
                if (firstAlign == 0) {
                    assert (totalSize == 0);
                    firstAlign = align;
                } else {
                    totalSize = llvm::alignTo(totalSize, align);
                }
                totalSize += B->getSize();
            }

            if (isExec) {
                firstExecAlign = firstAlign;
                totalExecSize = totalSize;
            } else {
                firstDataAlign = firstAlign;
                totalDataSize = totalSize;
            }

        }

        assert (totalExecSize == 0 || firstExecAlign);
        assert (totalDataSize == 0 || firstDataAlign);


        uintptr_t execOffset = 0;
        if (totalExecSize) {
            execOffset = allocateFromSlab(CurrentExecSlab, firstExecAlign, totalExecSize, ExecSlabAllocationMutex);
        }

        uintptr_t dataOffset = 0;
        if (totalDataSize) {
            dataOffset = allocateFromSlab(CurrentDataSlab, firstDataAlign, totalDataSize, DataSlabAllocationMutex);
        }

        auto checkItr = originalPosition.begin();

        for (auto & S : G.sections()) {

            const auto isExec = (S.getMemProt() & MemProt::Exec) != MemProt::None;
            auto offset = isExec ? execOffset : dataOffset;

            for (auto & B : S.blocks()) {
                offset = llvm::alignTo(offset, B->getAlignment());
                const auto s = B->getSize();
                char * const p = reinterpret_cast<char*>(offset);
                assert (*checkItr == B->getContent().data());
                if (B->isZeroFill()) {
                    std::memset(p, 0, s);
                } else if (LLVM_LIKELY(s > 0)) {
                    const char * const existing = B->getContent().data();
                    assert (*checkItr == existing);
                    assert (existing);
                    std::memcpy(p, existing, s);
                    B->setMutableContent(MutableArrayRef{p, s});
                }
                B->setAddress(orc::ExecutorAddr(offset));
                offset += s;
                ++checkItr;
            }
            if (isExec) {
                execOffset = offset;
            } else {
                dataOffset = offset;
            }
        }


        class NoOpInFlightAlloc : public InFlightAlloc {
        public:
            void finalize(OnFinalizedFunction onFinalize) override {
                onFinalize(FinalizedAlloc());
            }
            void abandon(OnAbandonedFunction onAbandon) override {
                onAbandon(Error::success());
            }
        };

        onAllocated(std::make_unique<NoOpInFlightAlloc>());

    }



    void deallocate(std::vector<FinalizedAlloc> Allocs, OnDeallocatedFunction OnDeallocated) override {
        OnDeallocated(Error::success());
    }

    void finalizeExecSlabs() {
        std::lock_guard<std::mutex> L(ExecSlabAllocationMutex);
        SlabNode * n = &ExecSlab;
        for (;;) {
            auto & S = n->Slab;
            const auto start = reinterpret_cast<uintptr_t>(S.base());
            const auto end = n->AllocatedOffset.load(std::memory_order_relaxed);
            if (LLVM_LIKELY(start != end)) {
                const auto ec = Mem::protectMappedMemory(S, Mem::MF_READ | Mem::MF_EXEC);
                if (ec) {
                    report_fatal_error(errorCodeToError(ec));
                    return;
                }
                Mem::InvalidateInstructionCache(S.base(), S.allocatedSize());
            }
            n = n->Next;
            if (LLVM_LIKELY(n == nullptr)) {
                break;
            }
        }
    };


    ~CPUDriverJITMemoryManager() override {
        BEGIN_SCOPED_REGION
        SlabNode * slab = ExecSlab.Next;
        while (slab) {
            SlabNode * next = slab->Next;
            delete slab;
            slab = next;
        }
        ExecSlab.Next = nullptr;
        END_SCOPED_REGION
        BEGIN_SCOPED_REGION
        SlabNode * slab = DataSlab.Next;
        while (slab) {
            SlabNode * next = slab->Next;
            delete slab;
            slab = next;
        }
        DataSlab.Next = nullptr;
        END_SCOPED_REGION
    }

private:

    inline uintptr_t allocateFromSlab(std::atomic<SlabNode *> & CurrentSlab,
                                      const uintptr_t firstBlockAlign, const uintptr_t totalSize,
                                      std::mutex & mutex) {

        SlabNode * currentSlab = CurrentSlab.load(std::memory_order_acquire);

        for (;;) {

            const auto & S = currentSlab->Slab;
            const auto endAddress = reinterpret_cast<uintptr_t>(S.base()) + S.allocatedSize();

            auto & allocatedOffset = currentSlab->AllocatedOffset;
            auto current = allocatedOffset.load(std::memory_order_relaxed);
            for (;;) {
                const auto offset = llvm::alignTo(current, firstBlockAlign);
                const auto nextOffset = offset + totalSize;
                if (LLVM_UNLIKELY(nextOffset > endAddress)) {
                    std::lock_guard<std::mutex> L(mutex);
                    SlabNode * nextSlab = CurrentSlab.load(std::memory_order_acquire);
                    if (nextSlab == currentSlab) {
                        const auto minSize = std::max<uintptr_t>(currentSlab->Slab.allocatedSize(), totalSize * 4);
                        const auto allocSize = llvm::alignToPowerOf2(minSize, CBuilder::PAGE_SIZE);
                        nextSlab = new SlabNode(allocSize);
                        assert (currentSlab->Next == nullptr);
                        currentSlab->Next = nextSlab;
                        CurrentSlab.store(nextSlab, std::memory_order_relaxed);
                    }
                    currentSlab = nextSlab;
                    break;
                }

                if (allocatedOffset.compare_exchange_weak(current, nextOffset, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    return offset;
                }
            }
        }
    }

private:
    SlabNode ExecSlab;
    std::atomic<SlabNode *> CurrentExecSlab;
    std::mutex ExecSlabAllocationMutex;

    SlabNode DataSlab;
    std::atomic<SlabNode *> CurrentDataSlab;
    std::mutex DataSlabAllocationMutex;

};

class CPUDriverCompiler {

    using MemoryBufferVector = std::vector<std::unique_ptr<MemoryBuffer>>;

public:

    CPUDriverCompiler(ThreadPoolStrategy strategy,
                      JITTargetMachineBuilder & JTMB, const StringMap<bool> & features,
                      ParabixObjectCache * objCache)
    : Engine(nullptr)
    , ObjectCache(objCache)
    , WorkQueue(8)
    , Contexts(strategy.ThreadsRequested, nullptr)
    , JITMemoryManager() {

        // NOTE: The pipeline may read information from other kernels to determine their state types and convert them
        // to its own LLVMContext as needed. We preserve the contexts as long as this compiler exists.
        for (size_t i = 0; i < strategy.ThreadsRequested; ++i) {
            Contexts[i] = new CPUDriverContext(JTMB, features);
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
        JITMemoryManager.finalizeExecSlabs();
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

    CPUDriverJITMemoryManager & getJITMemoryManager() {
        return JITMemoryManager;
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

                auto & linker = Engine->getObjLinkingLayer();
                auto & JITLib = Engine->getMainJITDylib();
                cantFail(linker.add(JITLib, std::move(cached)));
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
        Target->linkExternalMethods(builder);


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

        auto optLevel = llvm::CodeGenOptLevel::Default;
        if (LLVM_UNLIKELY(Target->hasAttribute(AttrId::InfrequentlyUsed))) {
            optLevel = codegen::BackEndOptLevel;
        }
        TM->setOptLevel(optLevel);

        legacy::PassManager PM;

        const auto atLeastOpt1 = optLevel != llvm::CodeGenOptLevel::None;

        FunctionSnippetPassManagerProxy FPM(*M, PM, atLeastOpt1);

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
            if (LLVM_UNLIKELY(TM->addPassesToEmitFile(FPM, out, nullptr, ASMFile))) {
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

        if (LLVM_UNLIKELY(TM->addPassesToEmitFile(FPM, out, nullptr, ObjFile))) {
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

        auto & linker = Engine->getObjLinkingLayer();
        auto & JITLib = Engine->getMainJITDylib();
        cantFail(linker.add(JITLib, std::move(objCode)));

        if (LLVM_UNLIKELY(IROutput.size() || OptIROutput.size() || ASMOutput.size())) {
            recordDebugPrintResult(Target, std::move(IROutput), std::move(OptIROutput), std::move(ASMOutput));
        }
    }

    void materializeMain(CPUDriverContext & C, Kernel * Target) {

        TargetMachine * const TM = C.TargetMachine.get();
        auto M = std::make_unique<Module>("main", C);
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

        auto & linker = Engine->getObjLinkingLayer();
        auto & JITLib = Engine->getMainJITDylib();
        cantFail(linker.add(JITLib, std::move(result)));

        END_SCOPED_REGION

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

    CPUDriverJITMemoryManager                       JITMemoryManager;

    std::mutex                                      PrecompiledStateObjectMutex;

    std::vector<std::thread>                        Threads;

    std::mutex                                      DebugPrintMutex;
    SmallVector<DebugPrintingResult, 0>             DebugPrintResults;

};

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
        .setCodeModel(CodeModel::Large)
        .setCodeGenOptLevel(codegen::BackEndOptLevel);

    const size_t numOfThreads = 1;

    mAllLinkedSymbols = std::make_unique<SymbolMap>();


    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));
   // Builder.setNumCompileThreads(0);
    Builder.setCompileFunctionCreator(nullptr);

    mCPUDriverCompiler = std::make_unique<CPUDriverCompiler>(
                                  llvm::hardware_concurrency(numOfThreads),
                                  *Builder.getJITTargetMachineBuilder(), features,
                                  mObjectCache.get());

    Builder.setObjectLinkingLayerCreator([this](ExecutionSession & ES, const Triple & TT) {
        auto objLinker = std::make_unique<ObjectLinkingLayer>(ES, mCPUDriverCompiler->getJITMemoryManager());
        objLinker->setAutoClaimResponsibilityForObjectSymbols(true);
        return objLinker;
    });

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


#include <kernel/pipeline/driver/orc_jit_backend.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/DynamicLibrary.h>           // for LoadLibraryPermanently
#include <llvm/ExecutionEngine/ExecutionEngine.h>  // for EngineBuilder
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/ObjectFileInterface.h>
#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(21, 0, 0)
#include <llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h>
#endif
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
#include <allocator/threadsafe_slaballocator.h>

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

using namespace llvm;
using namespace llvm::orc;
using namespace kernel;

using AttrId = kernel::Attribute::KindId;

// Module::setTargetTriple took a StringRef prior to LLVM 21 and takes a Triple from LLVM 21 onward.
template <typename ModulePtr>
inline void setModuleTargetTriple(ModulePtr && M, const Triple & T) {
    #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(21, 0, 0)
    M->setTargetTriple(T);
    #else
    M->setTargetTriple(T.getTriple());
    #endif
}

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
            auto mangled = M(unmangledName);
            ExecutorSymbolDef def{addr, flags};
            NewSymbolList.insert(std::make_pair(mangled, def));
            // Under ORC's on-request materialization, a kernel belonging to a *different*,
            // later-compiled pipeline can end up linked (and thus have its external symbol
            // dependencies resolved) while an *earlier* pipeline's own lookup() is still
            // pulling in its transitive dependencies -- e.g. via kernel deduplication or
            // simply because both pipelines were constructed before either's compile()
            // waited on completion. If that happens, this symbol must already be visible in
            // the JITDylib right now: batching all definitions until this pipeline's own
            // waitUntilCompleted() (as DriverLinkedSymbols does) is too late for an object
            // that gets added to the linker from a *different* pipeline's materialization
            // first, since ORC resolves/fails a materialization unit's external references
            // once, and a later definition doesn't retroactively unstick that failure. This
            // is unlike MCJIT's fully on-demand external symbol callback, which didn't care
            // about definition-vs-link ordering at all.
            auto & MainJD = Engine->getMainJITDylib();
            orc::SymbolMap single;
            single.insert(std::make_pair(mangled, def));
            auto err = MainJD.define(orc::absoluteSymbols(std::move(single)));
            if (err) {
                handleAllErrors(std::move(err), [](const DuplicateDefinition &) { /* already visible; fine */ });
            }
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
    Declaration = 0,
    MainFunction = 1
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
            // Buffer is full, so Head == Tail (the buffer has wrapped exactly once).
            // Valid data spans [Head, m) then wraps to [0, Head). After growing the
            // backing vector to 2m (which preserves [0, m) and leaves [m, 2m) unused),
            // relocate the wrapped prefix [0, Head) into the newly available space at
            // [m, m + Head) so the whole buffer becomes contiguous starting at Head.
            assert (Head == Tail);
            Buffer.resize(m * 2);
            std::move(Buffer.begin(), Buffer.begin() + Head, Buffer.begin() + m);
            Tail = Head + m;
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
public:
    CPUDriverWorkQueue(size_t initialCapacity) {
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
        cv.notify_all();
    }

    // Blocks until every currently queued/in-flight task (across all Declaration/
    // MainFunction levels) has completed, without telling worker threads to exit.
    // Unlike shutdown(), this may be called repeatedly over the compiler's lifetime
    // -- e.g. once per nested grep sub-pipeline compilation -- since the worker pool
    // remains alive and ready for further work afterward.
    void waitForDrain() {
        std::unique_lock<std::mutex> L(mutex);
        cv.wait(L, [this]{
            for (auto & Q : TaskQueues) {
                if (Q.InFlight) return false;
            }
            return true;
        });
    }

    // Permanently stops all worker threads. Only call once, when no further
    // compilation will ever be requested (i.e. at CPUDriverCompiler teardown).
    void shutdown() {
        std::lock_guard<std::mutex> L(mutex);
        StopRequested = 1;
        if (Sleeping) cv.notify_all();
    }

private:
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
        // How far into this slab has already been mprotect'd to R+X (only meaningful
        // for exec slabs; see finalizeExecSlabs()).
        uintptr_t ProtectedOffset;
        SlabNode * Next;

        // A nearby hint address is critical here: exec and data content for the same
        // object can end up referencing each other (e.g. Mach-O compact unwind info
        // encodes function offsets as 32-bit deltas), so if the exec slab and data
        // slab were allocated independently with no hint, ASLR could place them more
        // than 4GB apart and JIT linking would fail outright. Keeping every slab
        // within a bounded distance of the very first one avoids that.
        SlabNode(const size_t size, const Block * near = nullptr)
        : Slab([size, near](){
            std::error_code EC;
            const auto alignedSize = llvm::alignToPowerOf2(size, CBuilder::PAGE_SIZE);
            auto slab = Mem::allocateMappedMemory(alignedSize, near, Mem::MF_READ | Mem::MF_WRITE, EC);
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
        , ProtectedOffset(reinterpret_cast<uintptr_t>(Slab.base()))
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
    , DataSlab(DEFAULT_SLAB_SIZE, &ExecSlab.Slab)
    , CurrentDataSlab(&DataSlab) {

    }

    void allocate(const jitlink::JITLinkDylib * JD, jitlink::LinkGraph & G, OnAllocatedFunction onAllocated) override {
        size_t totalDataSize = 0;
        uint64_t maxDataAlign = 0;
        size_t totalExecSize = 0;
        uint64_t maxExecAlign = 0;

        std::vector<const char*> originalPosition;

        for (auto & S : G.sections()) {

            const auto isExec = (S.getMemProt() & MemProt::Exec) != MemProt::None;

            auto maxAlign = isExec ? maxExecAlign : maxDataAlign;
            auto totalSize = isExec ? totalExecSize : totalDataSize;


            for (auto & B : S.blocks()) {

                originalPosition.push_back(B->getContent().data());


                const auto align = B->getAlignment(); assert (align);
                if (maxAlign == 0) {
                    assert (totalSize == 0);
                } else {
                    totalSize = llvm::alignTo(totalSize, align);
                }
                maxAlign = std::max<uint64_t>(maxAlign, align);
                totalSize += B->getSize();
            }

            if (isExec) {
                maxExecAlign = maxAlign;
                totalExecSize = totalSize;
            } else {
                maxDataAlign = maxAlign;
                totalDataSize = totalSize;
            }

        }

        assert (totalExecSize == 0 || maxExecAlign);
        assert (totalDataSize == 0 || maxDataAlign);


        uintptr_t execOffset = 0;
        if (totalExecSize) {
            execOffset = allocateFromSlab(CurrentExecSlab, maxExecAlign, totalExecSize, ExecSlabAllocationMutex);
        }

        uintptr_t dataOffset = 0;
        if (totalDataSize) {
            dataOffset = allocateFromSlab(CurrentDataSlab, maxDataAlign, totalDataSize, DataSlabAllocationMutex);
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

    // May be called repeatedly (once per waitUntilCompleted(), which itself may run
    // more than once over the compiler's lifetime -- e.g. nested grep compiles and
    // immediately runs a sub-pipeline per directory). Each call only protects the
    // portion of each exec slab written since the previous call, rounded up to a full
    // page, and advances that slab's bump pointer past the now-protected (and thus
    // non-writable) pages so later allocations never target already-executable memory.
    void finalizeExecSlabs() {
        std::lock_guard<std::mutex> L(ExecSlabAllocationMutex);
        SlabNode * n = &ExecSlab;
        for (;;) {
            auto & S = n->Slab;
            const auto slabEnd = reinterpret_cast<uintptr_t>(S.base()) + S.allocatedSize();
            const auto used = n->AllocatedOffset.load(std::memory_order_relaxed);
            const auto alreadyProtected = n->ProtectedOffset;
            if (LLVM_LIKELY(used > alreadyProtected)) {
                const auto protectEnd = std::min<uintptr_t>(llvm::alignTo(used, CBuilder::PAGE_SIZE), slabEnd);
                const Block toProtect(reinterpret_cast<void*>(alreadyProtected), protectEnd - alreadyProtected);
                const auto ec = Mem::protectMappedMemory(toProtect, Mem::MF_READ | Mem::MF_EXEC);
                if (ec) {
                    report_fatal_error(errorCodeToError(ec));
                    return;
                }
                Mem::InvalidateInstructionCache(toProtect.base(), toProtect.allocatedSize());
                n->ProtectedOffset = protectEnd;
                n->AllocatedOffset.store(protectEnd, std::memory_order_relaxed);
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
                                      const uintptr_t requiredAlign, const uintptr_t totalSize,
                                      std::mutex & mutex) {

        SlabNode * currentSlab = CurrentSlab.load(std::memory_order_acquire);

        for (;;) {

            const auto & S = currentSlab->Slab;
            const auto endAddress = reinterpret_cast<uintptr_t>(S.base()) + S.allocatedSize();

            auto & allocatedOffset = currentSlab->AllocatedOffset;
            auto current = allocatedOffset.load(std::memory_order_relaxed);
            for (;;) {
                const auto offset = llvm::alignTo(current, requiredAlign);
                const auto nextOffset = offset + totalSize;
                if (LLVM_UNLIKELY(nextOffset > endAddress)) {
                    std::lock_guard<std::mutex> L(mutex);
                    SlabNode * nextSlab = CurrentSlab.load(std::memory_order_acquire);
                    if (nextSlab == currentSlab) {
                        const auto minSize = std::max<uintptr_t>(currentSlab->Slab.allocatedSize(), totalSize * 4);
                        const auto allocSize = llvm::alignToPowerOf2(minSize, CBuilder::PAGE_SIZE);
                        // Hint near the original exec slab (rather than currentSlab) so growth keeps
                        // accumulating close to the anchor point instead of drifting slab by slab.
                        nextSlab = new SlabNode(allocSize, &ExecSlab.Slab);
                        assert (currentSlab->Next == nullptr);
                        currentSlab->Next = nextSlab;
                        CurrentSlab.store(nextSlab, std::memory_order_release);
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

        // Drain currently-pending work but leave the worker pool running: this may be
        // called more than once over the compiler's lifetime (e.g. nested grep compiles
        // and immediately runs a small sub-pipeline per directory/.gitignore file, so
        // waitUntilCompleted() is invoked repeatedly against the same shared pool).
        WorkQueue.waitForDrain();

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

    // AlreadyCompiled holds raw Kernel* pointers keyed by cache-name, deduplicating
    // declarations across every P.compile() call sharing this compiler (e.g. nested
    // grep's one sub-pipeline per directory -- see waitUntilCompleted's comment). A
    // kernel it references must not be destroyed while this compiler (and thus the map)
    // is still alive: a later, unrelated compile whose own kernel happens to produce the
    // same cache-name (common for generic utility kernels like MemorySourceKernel) would
    // otherwise dedup against a dangling pointer to an already-destroyed kernel -- a
    // genuine, previously silent use-after-free. But the map can't simply be cleared
    // between compiles either: the JIT engine's symbol table is just as driver-lifetime
    // and shared, and a second, independently compiled kernel with the same name would
    // then fatal-error on a duplicate symbol definition. So instead, move every kernel
    // this map still references out of the caller's normal (per-compile-destroyed)
    // ownership and into this compiler's own, for as long as the compiler exists.
    // Called from OrcJITBackend::finalizeObject, once per P.compile(), on each of
    // mDriver.mCachedKernel/mCompiledKernel, before those are cleared.
    void preserveDedupedKernels(std::vector<std::unique_ptr<Kernel>> & kernels) {
        std::lock_guard<std::mutex> L(PrecompiledStateObjectMutex);
        if (AlreadyCompiled.empty()) return;
        for (auto & k : kernels) {
            if (!k) continue;
            for (auto & entry : AlreadyCompiled) {
                if (entry.second == k.get()) {
                    PreservedForDedup.emplace_back(std::move(k));
                    break;
                }
            }
        }
    }

    void setDriverLinkedSymbolMap(llvm::orc::SymbolMap * symMap) {
        DriverLinkedSymbols = symMap;
    }

    ~CPUDriverCompiler() {
        // No further compilation will be requested at this point; permanently stop the
        // worker pool (waitUntilCompleted() only drains, it never does this) and wait
        // for every thread to actually exit before tearing down their contexts.
        WorkQueue.shutdown();
        for (auto & t : Threads) {
            if (t.joinable()) t.join();
        }
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
        if (LLVM_UNLIKELY(!Target->isCachable())) {
            // Signature-based dedup assumes the signature captures everything that
            // affects the compiled artifact. isCachable() being false means the
            // opposite for kernels with internally generated (e.g. repeating)
            // streamsets: their actual runtime pattern data isn't folded into the
            // signature, so two structurally-identical-but-content-different
            // instances hash the same. Mix in the Target's own address so this
            // instance never gets deduped against an unrelated one -- deduping
            // them shared the wrong pattern data (silent wrong output) and could
            // leave this Target without a live PipelineCompiler when it later
            // needed to regenerate metadata (fatal error).
            nm << '\0' << Target;
        }
        const auto sig = nm.str();

        BEGIN_SCOPED_REGION
        bool added;
        StringMap<Kernel *>::iterator itr;
        Kernel * other = nullptr;

        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(PrecompiledStateObjectMutex);
        std::tie(itr, added) = AlreadyCompiled.insert(std::make_pair(sig, nullptr));
        // Read the existing entry's value under the same lock that protects its
        // write below (record_decl:, f->second = Target) -- ThreadSanitizer
        // confirmed a genuine data race here when this read was done after the
        // lock had already been released.
        if (!added) {
            other = itr->getValue();
        }
        END_SCOPED_REGION

        if (LLVM_UNLIKELY(!added)) {
            // TODO: this isn't safe if we want to discard kernels on restart
            if (other) {
                // other's state types live in a (possibly different) LLVMContext, so
                // Target can't just declareStateTypes() itself -- its own by-name struct
                // reuse only searches its own context. Populate Target's own scalar list
                // to match other's, then point it at other's already-built type.
                //
                // This assumes a matching cache-name/signature guarantees identical scalar
                // composition. That used to be false for kernel classes that don't override
                // hasSignature() (e.g. MemorySourceKernel): two independently-analyzed
                // pipelines could give the "same" kernel different managed-buffer/
                // termination-signal needs, since those scalars used to be added during
                // pipeline compilation (KernelCompiler::addBaseInternalProperties), driven by
                // that pipeline's own buffer-layout analysis. Both scalars are now reserved
                // unconditionally at kernel construction time instead (Kernel::
                // addBaseInternalScalars), so composition is fixed before any pipeline ever
                // sees the kernel, and this assumption actually holds. Given that, Target's
                // scalar list can just be copied from other's (Kernel::copyInternalScalarsFrom)
                // instead of independently recomputed via addInternalProperties() -- avoiding
                // redoing potentially expensive per-kernel analysis (e.g. a PabloKernel's
                // carry-structure analysis) purely to reproduce a result we already have.
                Target->copyInternalScalarsFrom(*other);
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

        Module * pendingObjectModule = nullptr;

        if (LLVM_LIKELY(ObjectCache && Target->isCachable())) {
            std::unique_ptr<MemoryBuffer> cached;
            std::unique_ptr<Module> M;
            std::tie(cached, M) = ObjectCache->loadCachedObjectFile(builder, Target);
            if (M) {

                setModuleTargetTriple(M, TM->getTargetTriple());
                M->setDataLayout(ctx.DataLayout);

                Target->loadCachedKernel(M.get());
                Target->recordScalarFieldIndices();
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
        setModuleTargetTriple(M, TM->getTargetTriple());
        M->setDataLayout(ctx.DataLayout);
        builder.setModule(M);
        ctx.CurrentModule = M;
        ctx.Engine = Engine;
        Target->declareStateTypes(builder);
        pendingObjectModule = M;
        END_SCOPED_REGION

record_decl:
        BEGIN_SCOPED_REGION
        std::lock_guard<std::mutex> L(PrecompiledStateObjectMutex);
        auto f = AlreadyCompiled.find(sig);
        assert (f != AlreadyCompiled.end());
        assert (f->second == nullptr);
        f->second = Target;
        END_SCOPED_REGION

        // Materialize the object body on this same thread/context right away instead of
        // queueing it as a separate ObjectCode task for another worker thread to pick up.
        // declareStateTypes() above can cache raw llvm::Type*/Value* pointers inside the
        // Kernel (e.g. CarryManager's per-scope summary types), and those are only valid
        // in the LLVMContext that created them. A different worker thread has a different
        // LLVMContext, so materializing the body there would silently mix types across
        // contexts -- e.g. CarryManager::castToSummaryType's `carryOutTy == summaryTy`
        // pointer check fails for two structurally-identical i8 types from different
        // contexts, producing an invalid same-width "zext i8 to i8" that only a debug
        // build's IR verifier catches (a release build emits it unnoticed).
        if (pendingObjectModule) {
            materializeObject(ctx, Target, pendingObjectModule);
        }

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
            Target->recordScalarFieldIndices();
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
        setModuleTargetTriple(M, TM->getTargetTriple());
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

        // All kernel bodies compile at codegen::BackEndOptLevel (default None/-O0;
        // override with --backend-optimization-level). InfrequentlyUsed kernels used to
        // get this level unconditionally while other kernels got a hardcoded Default --
        // that hardcoded Default was itself the source of a long-standing, previously
        // undiagnosed compile-time/runtime-quality mismatch on the MCJIT backend (see
        // MCJITBackend's EngineBuilder::create() fix); using one configurable level for
        // every kernel avoids reintroducing that kind of silent inconsistency.
        TM->setOptLevel(codegen::BackEndOptLevel);

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
            // Use a separate PassManager (and a separate run()) for ASM emission.
            // TargetMachine::addPassesToEmitFile installs a full codegen pipeline
            // (isel, regalloc, AsmPrinter); appending a second one for the object
            // file below onto this same PM ran the whole pipeline over every
            // function twice within one PM.run(), and the second (object) pass's
            // AsmPrinter then tried to redefine every symbol the first (ASM) pass
            // had already emitted -- "symbol '...' is already defined". Reproducible
            // with idisa_exerciser --ShowASM=<path>.
            legacy::PassManager asmPM;
            if (LLVM_UNLIKELY(TM->addPassesToEmitFile(asmPM, out, nullptr, ASMFile))) {
                report_fatal_error(Twine{"Failed to generate ASM for ", M->getModuleIdentifier()});
            }
            asmPM.run(*M);

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
        setModuleTargetTriple(M, TM->getTargetTriple());
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

        C.TargetMachine->setOptLevel(codegen::BackEndOptLevel);

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
        // Unlike kernel declarations (deduplicated via AlreadyCompiled), a top-level
        // Program's "_main" is materialized here unconditionally on every compile() call.
        // Two independently-constructed Programs with the same structural signature (e.g.
        // repeated trials in a test harness picking the same parameters) hash to the same
        // name and thus the same "_main" symbol; tolerate that the way the other JITDylib
        // symbol definitions in this file already do, keeping whichever definition linked
        // first since they're compiled from identical IR.
        auto err = linker.add(JITLib, std::move(result));
        if (err) {
            handleAllErrors(std::move(err),
                [](const DuplicateDefinition &) { /* an identical Program was already linked; fine */ },
                [Target](const ErrorInfoBase & err) {
                    SmallVector<char, 100> tmp;
                    raw_svector_ostream msg(tmp);
                    msg << Target->getName() << ": cannot link main function: " << err.message();
                    report_fatal_error(msg.str());
                });
        }

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

    // See preserveDedupedKernels.
    std::vector<std::unique_ptr<Kernel>>            PreservedForDedup;

    std::vector<CPUDriverContext *>                 Contexts;

    CPUDriverJITMemoryManager                       JITMemoryManager;

    std::mutex                                      PrecompiledStateObjectMutex;

    std::vector<std::thread>                        Threads;

    std::mutex                                      DebugPrintMutex;
    SmallVector<DebugPrintingResult, 0>             DebugPrintResults;

};


OrcJITBackend::OrcJITBackend(CPUDriver & driver)
: mDriver(driver)
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


    const size_t numOfThreads = std::max(codegen::CompileThreads, 1U);

    mAllLinkedSymbols = std::make_unique<SymbolMap>();


    auto Builder = orc::LLJITBuilder();
    Builder.setJITTargetMachineBuilder(std::move(JTMB));
   // Builder.setNumCompileThreads(0);
    Builder.setCompileFunctionCreator(nullptr);

    mCPUDriverCompiler = std::make_unique<CPUDriverCompiler>(
                                  llvm::hardware_concurrency(numOfThreads),
                                  *Builder.getJITTargetMachineBuilder(), features,
                                  mDriver.mObjectCache.get());

    // Our custom CPUDriverJITMemoryManager's persistent exec/data slab pools are a
    // linking-speed optimization, but from LLVM 20 they can trigger "__TEXT,__unwind_info,
    // delta to end of functions ... exceeds 32 bits" JIT session errors from JITLink's
    // Mach-O/arm64 compact-unwind handling (confirmed specific to our allocator; LLVM's own
    // InProcessMemoryManager, which allocates a small dedicated region per object rather
    // than long-lived shared pools, does not hit it). --use-custom-jit-memory-manager
    // controls which is used; see toolchain.cpp for its LLVM-version-dependent default.
    #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(21, 0, 0)
    Builder.setObjectLinkingLayerCreator([this](ExecutionSession & ES) {
        auto objLinker = codegen::UseCustomJITMemoryManager
            ? std::make_unique<ObjectLinkingLayer>(ES, mCPUDriverCompiler->getJITMemoryManager())
            : std::make_unique<ObjectLinkingLayer>(ES, cantFail(jitlink::InProcessMemoryManager::Create()));
        objLinker->setAutoClaimResponsibilityForObjectSymbols(true);
        return objLinker;
    });
    #else
    Builder.setObjectLinkingLayerCreator([this](ExecutionSession & ES, const Triple & TT) {
        auto objLinker = codegen::UseCustomJITMemoryManager
            ? std::make_unique<ObjectLinkingLayer>(ES, mCPUDriverCompiler->getJITMemoryManager())
            : std::make_unique<ObjectLinkingLayer>(ES, cantFail(jitlink::InProcessMemoryManager::Create()));
        objLinker->setAutoClaimResponsibilityForObjectSymbols(true);
        return objLinker;
    });
    #endif

    // CPUDriverCompiler manages its own worker-thread pool for compilation, so ORC's
    // own task dispatch must run tasks synchronously in-place rather than on a
    // separate internal thread pool.
    #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(18, 0, 0)
    Builder.setExecutorProcessControl(
        cantFail(SelfExecutorProcessControl::Create(nullptr, std::make_unique<InPlaceTaskDispatcher>())));
    #endif

    mEngine = cantFail(Builder.create());

    mCPUDriverCompiler->setEngine(mEngine.get());

    mCPUDriverCompiler->setDriverLinkedSymbolMap(mAllLinkedSymbols.get());

    #if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(18, 0, 0)
    auto & ES = mEngine->getExecutionSession();

    ES.setDispatchTask([](std::unique_ptr<Task> T) {
        T->run();
    });
    #endif

    auto & MainJD = mEngine->getMainJITDylib();
    MainJD.addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            mEngine->getDataLayout().getGlobalPrefix()
        ))
    );

    mDriver.mBuilder.reset(IDISA::GetIDISA_Builder(mDriver.mMainModule->getContext(), features));
    mDriver.mBuilder->setModule(mDriver.mMainModule);
    mDriver.mBuilder->setFunctionLinkCallback(&mDriver);
}

void OrcJITBackend::generateUncachedKernels() {

    if (mDriver.mUncachedKernel.empty()) return;

    // TODO: we may be able to reduce unnecessary optimization work by having kernel specific optimization passes.

    // NOTE: we currently require DCE and Mem2Reg for each kernel to eliminate any unnecessary scalar -> value
    // mappings made by the base KernelCompiler. That could be done in a more focused manner, however, as each
    // mapping is known.

    const auto numKernels = mDriver.mUncachedKernel.size();

    mDriver.mCachedKernel.reserve(numKernels);

    const auto layerId = mCPUDriverCompiler->addNewTaskGroup(numKernels);
    for (unsigned i = 0; i < numKernels; ++i) {
        auto & kernel = mDriver.mUncachedKernel[i];
        mCPUDriverCompiler->addCompilationTask(layerId, kernel.get());
        mDriver.mCachedKernel.emplace_back(kernel.release());
    }

    mDriver.mUncachedKernel.clear();

}

void * OrcJITBackend::finalizeObject(kernel::Kernel * const pk) {

    const auto layerId = mCPUDriverCompiler->addNewTaskGroup(1);

    mCPUDriverCompiler->addMainFunctionTask(layerId, pk);

    auto mainFuncPtr = mCPUDriverCompiler->waitUntilCompleted(pk);


    assert (mainFuncPtr);

    if (mDriver.getPreservesKernels()) {
        for (auto & kernel : mDriver.mCachedKernel) {
            mDriver.mPreservedKernel.emplace_back(kernel.release());
        }
        for (auto & kernel : mDriver.mCompiledKernel) {
            mDriver.mPreservedKernel.emplace_back(kernel.release());
        }
    } else {
        mDriver.mPreservedKernel.clear();
    }

    // Whatever's left (i.e. wasn't already moved to mPreservedKernel above) is about to
    // be destroyed by the clears below; keep alive anything AlreadyCompiled's dedup
    // cache still points to, since that cache -- and this compiler -- outlive this one
    // compile. See preserveDedupedKernels.
    mCPUDriverCompiler->preserveDedupedKernels(mDriver.mCachedKernel);
    mCPUDriverCompiler->preserveDedupedKernels(mDriver.mCompiledKernel);

    mDriver.mCachedKernel.clear();
    mDriver.mCompiledKernel.clear();

    //    llvm::reportAndResetTimings();
    //    llvm::PrintStatistics();

    return mainFuncPtr;
}

llvm::Function * OrcJITBackend::LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
    assert (&functionType->getContext() == &mDriver.mMainModule->getContext());
    Function * f = mDriver.mMainModule->getFunction(unmangledName);
    if (LLVM_UNLIKELY(f == nullptr)) {
        f = Function::Create(functionType, Function::ExternalLinkage, unmangledName, mDriver.mMainModule);
        MangleAndInterner M(mEngine->getExecutionSession(), mEngine->getDataLayout());
        auto symbol = M(unmangledName);
        auto addr = orc::ExecutorAddr::fromPtr(functionPointer);
        mAllLinkedSymbols->insert(std::make_pair(symbol, ExecutorSymbolDef{addr, JITSymbolFlags::Exported}));
    }
    return f;
}

bool OrcJITBackend::HasExternalFunction(llvm::StringRef functionName) const {
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

OrcJITBackend::~OrcJITBackend() {
    cantFail(mEngine->getExecutionSession().endSession());
}


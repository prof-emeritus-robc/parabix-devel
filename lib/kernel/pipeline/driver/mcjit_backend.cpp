#include <kernel/pipeline/driver/mcjit_backend.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/core/idisa_target.h>
#include <kernel/core/kernel_builder.h>
#include <toolchain/toolchain.h>
#include <objcache/object_cache.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/ObjectCache.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/Timer.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/PassTimingInfo.h>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

using namespace llvm;
using namespace kernel;

using AttrId = kernel::Attribute::KindId;

// MCJITObjectCacheAdapter bridges ParabixObjectCache's explicit loadCachedObjectFile/
// saveCachedObjectFile calls (the calling convention CPUDriverCompiler already uses on
// the ORC side) to MCJIT's mandatory, auto-invoked llvm::ObjectCache callback interface.
// The cache-hit/miss DECISION is made once, up front, in MCJITBackend::generateUncachedKernels
// (reusing Kernel::isCachable()+loadCachedObjectFile exactly as ORC's materializeDecl does);
// this adapter's only job is relaying that decision into ExecutionEngine::addModule/
// finalizeObject's internal per-module codegen dispatch, which is the only hook classic
// MCJIT provides for supplying a precompiled object buffer.
class MCJITObjectCacheAdapter final : public llvm::ObjectCache {
public:

    explicit MCJITObjectCacheAdapter(ParabixObjectCache * cache) : mCache(cache) {}

    // Cache miss: remember which Kernel this Module belongs to, so notifyObjectCompiled
    // (invoked once MCJIT finishes real codegen for it) knows whether/how to save it.
    void registerKernel(const llvm::Module * M, kernel::Kernel * K) {
        mOwners[M] = K;
    }

    // Cache hit: pre-supply the object bytes so MCJIT's internal codegen for this exact
    // Module is skipped entirely (getObject() below hands them back exactly once).
    void registerPrecompiled(const llvm::Module * M, std::unique_ptr<llvm::MemoryBuffer> obj) {
        mPrecompiled[M] = std::move(obj);
    }

    void notifyObjectCompiled(const llvm::Module * M, llvm::MemoryBufferRef Obj) override {
        auto it = mOwners.find(M);
        if (it != mOwners.end() && it->second->isCachable() && mCache) {
            mCache->saveCachedObjectFile(*M, Obj);
        }
    }

    std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module * M) override {
        auto it = mPrecompiled.find(M);
        if (it == mPrecompiled.end()) {
            return nullptr;
        }
        auto buf = std::move(it->second);
        mPrecompiled.erase(it);
        return buf;
    }

private:
    ParabixObjectCache * const                                        mCache;
    llvm::DenseMap<const llvm::Module *, kernel::Kernel *>            mOwners;
    llvm::DenseMap<const llvm::Module *, std::unique_ptr<llvm::MemoryBuffer>> mPrecompiled;
};

MCJITBackend::MCJITBackend(CPUDriver & driver)
: mDriver(driver) {

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
    EngineBuilder builder{std::unique_ptr<Module>(mDriver.mMainModule)};
    builder.setErrorStr(&errMessage);
    builder.setVerifyModules(false);
    builder.setEngineKind(EngineKind::JIT);
    builder.setTargetOptions(codegen::target_Options);
    builder.setOptLevel(codegen::BackEndOptLevel);
    builder.setMAttrs(attrs);

    mTarget = builder.selectTarget();
    if (LLVM_UNLIKELY(mTarget == nullptr)) {
        report_fatal_error("Could not selectTarget for MCJIT");
    }
    // EngineBuilder::create() (no args) is `return create(selectTarget());` -- calling
    // it with no argument would invoke selectTarget() a SECOND time, handing the engine
    // a different TargetMachine instance than the one we're holding as mTarget, so every
    // later mTarget->setOptLevel(...) call (per-kernel opt-level selection, see
    // finalizeObject) would silently affect the wrong, unused TargetMachine. Pass mTarget
    // explicitly so the engine takes ownership of the SAME instance we mutate.
    mEngine.reset(builder.create(mTarget));
    if (LLVM_UNLIKELY(mEngine == nullptr)) {
        report_fatal_error(Twine("Could not create MCJIT ExecutionEngine: ") + errMessage);
    }

    mObjectCacheAdapter = std::make_unique<MCJITObjectCacheAdapter>(mDriver.mObjectCache.get());
    mEngine->setObjectCache(mObjectCacheAdapter.get());
    mEngine->DisableSymbolSearching(false);
    mEngine->DisableLazyCompilation(true);
    mEngine->DisableGVCompilation(true);

    #if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(21, 0, 0)
    auto triple = mTarget->getTargetTriple().getTriple();
    #else
    auto triple = mTarget->getTargetTriple();
    #endif
    const DataLayout DL(mTarget->createDataLayout());
    mDriver.mMainModule->setTargetTriple(triple);
    mDriver.mMainModule->setDataLayout(DL);

    mDriver.mBuilder.reset(IDISA::GetIDISA_Builder(mDriver.mMainModule->getContext(), features));
    mDriver.mBuilder->setModule(mDriver.mMainModule);
    mDriver.mBuilder->setFunctionLinkCallback(&mDriver);
}

MCJITBackend::~MCJITBackend() {
}

void MCJITBackend::generateUncachedKernels() {

    if (mDriver.mUncachedKernel.empty()) return;

    auto & builder = *mDriver.mBuilder;
    mCompiledModules.reserve(mCompiledModules.size() + mDriver.mUncachedKernel.size());

    for (auto & kernelPtr : mDriver.mUncachedKernel) {
        Kernel * const Target = kernelPtr.get();

        NamedRegionTimer T(Target->getSignature(), Target->getName(),
                           "kernel", "Kernel Generation",
                           codegen::TimeKernelsIsEnabled);

        Module * M = nullptr;

        if (mDriver.mObjectCache && Target->isCachable()) {
            std::unique_ptr<MemoryBuffer> cached;
            std::unique_ptr<Module> cachedModule;
            std::tie(cached, cachedModule) = mDriver.mObjectCache->loadCachedObjectFile(builder, Target);
            if (cachedModule) {
                M = cachedModule.release();
                M->setTargetTriple(mDriver.mMainModule->getTargetTriple());
                M->setDataLayout(mDriver.mMainModule->getDataLayout());
                Target->loadCachedKernel(M);
                builder.setModule(M);
                Target->linkExternalMethods(builder);
                mObjectCacheAdapter->registerPrecompiled(M, std::move(cached));
                mCompiledModules.emplace_back(Target, M);
                continue;
            }
        }

        // Cache miss (or caching disabled/inapplicable): generate the kernel body via
        // the same phased API the ORC backend uses (declareStateTypes/generateKernel/
        // addOptimizationPasses+runAllOptimizationPasses are backend-agnostic).
        M = Target->makeEmptyModule(builder);
        M->setTargetTriple(mDriver.mMainModule->getTargetTriple());
        M->setDataLayout(mDriver.mMainModule->getDataLayout());
        builder.setModule(M);
        Target->declareStateTypes(builder);
        Target->linkExternalMethods(builder);
        Target->generateKernel(builder, mTarget, GlobalValue::ExternalLinkage);

        Kernel::SelectedOptimizationPasses passes;
        Target->addOptimizationPasses(builder, passes);
        SmallVector<char, 0> unoptIR, optIR;
        BaseDriver::runAllOptimizationPasses(builder, passes, mTarget, unoptIR, optIR);
        // TODO: --ShowIR/--ShowUnoptimizedIR output capture (unoptIR/optIR above) is not
        // yet wired to a debug-output stream under MCJIT mode; ORC's equivalent lives in
        // CPUDriverCompiler::printDebugOutput. Low priority: these flags are debugging aids,
        // not correctness-affecting, and unwired here only means the captured buffers are
        // discarded rather than mis-shown.

        if (mDriver.mObjectCache && Target->isCachable()) {
            mObjectCacheAdapter->registerKernel(M, Target);
        }

        mCompiledModules.emplace_back(Target, M);
    }

    mDriver.mCachedKernel.reserve(mDriver.mCachedKernel.size() + mDriver.mUncachedKernel.size());
    for (auto & kernel : mDriver.mUncachedKernel) {
        mDriver.mCachedKernel.emplace_back(kernel.release());
    }
    mDriver.mUncachedKernel.clear();

    builder.setModule(mDriver.mMainModule);

    llvm::reportAndResetTimings();
    llvm::PrintStatistics();
}

void * MCJITBackend::finalizeObject(kernel::Kernel * const pk) {

    using ModuleSet = SmallVector<Module *, 32>;

    ModuleSet Infrequent;
    ModuleSet Normal;

    for (const auto & entry : mCompiledModules) {
        Kernel * const Target = entry.first;
        Module * const M = entry.second;
        if (LLVM_UNLIKELY(Target->hasAttribute(AttrId::InfrequentlyUsed))) {
            Infrequent.emplace_back(M);
        } else {
            Normal.emplace_back(M);
        }
    }

    auto addModules = [&](const ModuleSet & S, const CodeGenOptLevel level) {
        if (S.empty()) return;
        mTarget->setOptLevel(level);
        for (Module * M : S) {
            mEngine->addModule(std::unique_ptr<Module>(M));
        }
        mEngine->finalizeObject();
    };

    auto removeModules = [&](const ModuleSet & S) {
        for (Module * M : S) {
            mEngine->removeModule(M);
        }
    };

    // Both tiers compile at codegen::BackEndOptLevel (default None/-O0; override with
    // --backend-optimization-level). Normal kernels used to get a hardcoded Default here
    // while Infrequent ones got BackEndOptLevel -- see OrcJITBackend::materializeObject
    // for why that hardcoded Default was itself a bug source, now avoided by using one
    // configurable level for every kernel.
    {
        NamedRegionTimer T("object-generation", "object-generation", "object", "Object Generation", codegen::TimeKernelsIsEnabled);
        addModules(Infrequent, codegen::BackEndOptLevel);
        addModules(Normal, codegen::BackEndOptLevel);
    }

    auto mainModule = std::make_unique<Module>("main", mDriver.getContext());
    mainModule->setTargetTriple(mDriver.mMainModule->getTargetTriple());
    mainModule->setDataLayout(mDriver.mMainModule->getDataLayout());

    auto & builder = *mDriver.mBuilder;
    builder.setModule(mainModule.get());
    pk->linkExternalMethods(builder);
    Function * const main = pk->addOrDeclareMainFunction(builder, Kernel::AddInternal);
    builder.setModule(mDriver.mMainModule);

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

    mTarget->setOptLevel(codegen::BackEndOptLevel);
    Module * const mainModulePtr = mainModule.get();
    mEngine->addModule(std::move(mainModule));

    mDriver.mCachedKernel.clear();
    mDriver.mCompiledKernel.clear();
    mCompiledModules.clear();

    mEngine->finalizeObject();
    auto mainFnPtr = mEngine->getFunctionAddress(main->getName().str());

    removeModules(Normal);
    removeModules(Infrequent);
    mEngine->removeModule(mainModulePtr);
    mEngine->removeModule(mDriver.mMainModule);

    return reinterpret_cast<void *>(mainFnPtr);
}

llvm::Function * MCJITBackend::LinkFunction(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
    Module * const mod = mDriver.mBuilder->getModule();
    if (LLVM_UNLIKELY(mod == nullptr)) {
        report_fatal_error(Twine("LinkFunction(") + unmangledName + ") cannot be called until after addKernel");
    }
    Function * f = mod->getFunction(unmangledName);
    if (LLVM_UNLIKELY(f == nullptr)) {
        f = Function::Create(functionType, Function::ExternalLinkage, unmangledName, mod);
        mEngine->updateGlobalMapping(f, functionPointer);
    }
    return f;
}

bool MCJITBackend::HasExternalFunction(llvm::StringRef unmangledName) const {
    return RTDyldMemoryManager::getSymbolAddressInProcess(unmangledName.str()) != 0;
}

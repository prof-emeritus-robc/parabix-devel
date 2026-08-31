#include <kernel/core/kernel_compiler.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/driver/driver.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/Twine.h>
#include <boost/intrusive/detail/math.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/container/flat_map.hpp>
#include <kernel/core/streamsetptr.h>
#include <codegen/TypeBuilder.h>
#include <kernel/illustrator/illustrator.h>


#include <kernel/pipeline/driver/driver.h>

using namespace llvm;
using namespace boost;
using boost::intrusive::detail::floor_log2;
using boost::container::flat_set;
using boost::container::flat_map;

namespace kernel {

using AttrId = Attribute::KindId;
using Rational = ProcessingRate::Rational;
using RateId = ProcessingRate::KindId;
using StreamSetPort = Kernel::StreamSetPort;
using PortType = Kernel::PortType;

constexpr static auto BUFFER_HANDLE_SUFFIX = "_buffer";
constexpr static auto TERMINATION_SIGNAL = "__termination_signal";

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

// TODO: this check is a bit too strict in general; if the pipeline could request data/
// EOF padding from the MemorySource kernel, it would be possible to re-enable.

// TODO: split the init/final into two methods each, one to do allocation/init, and the
// other final/deallocate? Would potentially allow us to reuse the kernel/stream set
// memory in the nested engine if each init method memzero'ed them. Would need to change
// the "main" method.

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructStateTypes
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::constructStateTypes(KernelBuilder & b) {
    auto const oc = b.getCompiler();
    b.setCompiler(this);
    constructStreamSetBuffers(b);
    #ifndef NDEBUG
    for (const auto & buffer : mStreamSetInputBuffers) {
        assert ("input buffer not set by constructStreamSetBuffers" && buffer.get());
    }
    for (const auto & buffer : mStreamSetOutputBuffers) {
        assert ("output buffer not set by constructStreamSetBuffers" && buffer.get());
    }
    #endif
    addBaseInternalProperties(b);
    mTarget->addInternalProperties(b);
    mTarget->constructStateTypes(b);
    b.setCompiler(oc);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateKernel
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::generateKernel(KernelBuilder & b, TargetMachine * TM, GlobalValue::LinkageTypes linkageType) {
    // NOTE: make sure to keep and reset the original compiler here. A kernel could generate new kernels and
    // reuse the same KernelBuilder to do so; this could result in unexpected behaviour if the this function
    // exits without restoring the original compiler state.
    auto const oc = b.getCompiler();
    b.setCompiler(this);
//    mTarget->addKernelDeclarations(b, TM, linkageType);
    callGenerateInitializeMethod(b, linkageType);
    if (LLVM_UNLIKELY(mStreamSetInputBuffers.empty())) {
        callGenerateExpectedOutputSizeMethod(b, linkageType);
    }
    if (LLVM_UNLIKELY(mTarget->allocatesInternalStreamSets())) {
        callGenerateAllocateSharedInternalStreamSets(b, linkageType);
    }
    callGenerateDoSegmentMethod(b, TM, linkageType);
    if (LLVM_UNLIKELY(mTarget->getThreadLocalStateType())) {
        callGenerateInitializeThreadLocalMethod(b, linkageType);
        if (LLVM_UNLIKELY(mTarget->allocatesInternalStreamSets())) {
            callGenerateAllocateThreadLocalInternalStreamSets(b, linkageType);
        }
        callGenerateFinalizeThreadLocalMethod(b, linkageType);
    }
    callGenerateFinalizeMethod(b, linkageType);
    mTarget->addAdditionalFunctions(b);
    b.setCompiler(oc);

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addBaseInternalProperties
  ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::addBaseInternalProperties(KernelBuilder & b) {
     // If an output is a managed buffer, store its handle.
    auto & C = b.getContext();
    const auto n = mOutputStreamSets.size();
    for (unsigned i = 0; i < n; ++i) {
        const Binding & output = mOutputStreamSets[i];
        Type * const handleTy = CBuilder::convertTypeToLLVMContext(C, mStreamSetOutputBuffers[i]->getHandleType(b));
        const auto isLocal = Kernel::isLocalBuffer(output);
        if (LLVM_UNLIKELY(isLocal.any())) {
            mTarget->addInternalScalar(handleTy, output.getName() + BUFFER_HANDLE_SUFFIX);
        } else {
            mTarget->addNonPersistentScalar(handleTy, output.getName() + BUFFER_HANDLE_SUFFIX);
        }
    }
    IntegerType * const sizeTy = b.getSizeTy();
    if (mTarget->hasAttribute(AttrId::InternallySynchronized) || mTarget->canSetTerminateSignal()) {
        mTarget->addInternalScalar(sizeTy, TERMINATION_SIGNAL);
    } else {
        mTarget->addNonPersistentScalar(sizeTy, TERMINATION_SIGNAL);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructStreamSetBuffers
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::constructStreamSetBuffers(KernelBuilder & b) {

    mStreamSetInputBuffers.clear();
    const auto numOfInputStreams = mInputStreamSets.size();
    mStreamSetInputBuffers.resize(numOfInputStreams);
    for (unsigned i = 0; i < numOfInputStreams; ++i) {
        const Binding & input = mInputStreamSets[i];
        mStreamSetInputBuffers[i].reset(new ExternalBuffer(i, b, input.getType(), 0));
    }
    mStreamSetOutputBuffers.clear();
    const auto numOfOutputStreams = mOutputStreamSets.size();
    mStreamSetOutputBuffers.resize(numOfOutputStreams);
    for (unsigned i = 0; i < numOfOutputStreams; ++i) {
        const Binding & output = mOutputStreamSets[i];

        StreamSetBuffer * buffer = nullptr;
        if (LLVM_UNLIKELY(Kernel::isManagedBuffer(output))) {
            const auto isReturnedBuffer = output.hasAttribute(AttrId::ReturnedBuffer);
            buffer = new ManagedDynamicBuffer(i + numOfInputStreams, b, output.getType(), isReturnedBuffer, 0);
        } else {
            buffer = new ExternalBuffer(i + numOfInputStreams, b, output.getType(), 0);
        }

        mStreamSetOutputBuffers[i].reset(buffer);
    }
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief reset
 ** ------------------------------------------------------------------------------------------------------------- */
template <typename Vec>
inline void reset(Vec & vec, const size_t n) {
    vec.resize(n);
    std::fill_n(vec.begin(), n, nullptr);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateInitializeMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateInitializeMethod(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) {
    mCurrentMethod = mTarget->getInitializeFunction(b, true, linkageType);
    assert (mCurrentMethod->empty());
    mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
    b.SetInsertPoint(mEntryPoint);
    auto arg = mCurrentMethod->arg_begin();
    const auto arg_end = mCurrentMethod->arg_end();
    auto nextArg = [&]() {
        assert (arg != arg_end);
        Value * const v = &*arg;
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };
    assert (getHandle() == nullptr);

    const auto ea = codegen::DebugOptionIsSet(codegen::EnableAsserts);

    StructType * const sharedStateTy = mTarget->getSharedStateType(b.getContext());

    if (LLVM_UNLIKELY(ea)) {

        Value * const providedSharedStateTySize = nextArg();

        Constant * sharedStateTySize = nullptr;
        if (LLVM_LIKELY(sharedStateTy)) {
            sharedStateTySize = b.getTypeSize(sharedStateTy);
        } else {
            sharedStateTySize = ConstantInt::getAllOnesValue(b.getSizeTy());
        }
        Value * const correctSharedTySize = b.CreateICmpUGE(providedSharedStateTySize, sharedStateTySize);

        b.CreateAssert(correctSharedTySize,
                       " expected state type object of size %" PRIu64 " but received one of size %" PRIu64,
                       sharedStateTySize, providedSharedStateTySize);

        Value * const providedThreadLocalTySize = nextArg();

        Constant * threadLocalTySize = nullptr;
        if (mTarget->getThreadLocalStateType()) {
            threadLocalTySize = b.getTypeSize(mTarget->getThreadLocalStateType());
        } else {
            threadLocalTySize = ConstantInt::getAllOnesValue(b.getSizeTy());
        }

        Value * const correctThreadLocalTySize = b.CreateICmpUGE(providedThreadLocalTySize, threadLocalTySize);

        b.CreateAssert(correctThreadLocalTySize,
                       " expected state type object of size %" PRIu64 " but received one of size %" PRIu64,
                       threadLocalTySize, providedThreadLocalTySize);
    }

    mSharedHandle = nullptr;
    mThreadLocalHandle = nullptr;



    if (LLVM_LIKELY(sharedStateTy)) {
        setHandle(nextArg());
        if (LLVM_UNLIKELY(ea)) {
            auto & dl = b.getModule()->getDataLayout();

            const auto align = CBuilder::getAlignOf(dl, sharedStateTy);
            if (LLVM_LIKELY(align > 1U)) {
            Value * handleInt = b.CreatePtrToInt(getHandle(), b.getSizeTy());
            b.CreateAssertZero(b.CreateURem(handleInt, b.getSize(align)),
                               "%s shared handle addresss (x%" PRIx64 ") is misaligned for shared state type (%" PRIu64 ")",
                               b.GetString("InitializeShared"), handleInt, b.getSize(align));
            }
        }
    }


    initializeScalarMap(b, InitializeOptions::DoNotIncludeThreadLocalScalars);
    for (const auto & binding : mInputScalars) {
        b.setScalarField(binding.getName(), nextArg());
    }
    bindAdditionalInitializationArguments(b, arg, arg_end);
    assert (arg == arg_end);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect) && sharedStateTy)) {
        b.CreateMProtect(sharedStateTy, mSharedHandle, CBuilder::Protect::WRITE);
    }
    // TODO: we could permit shared managed buffers here if we passed in the buffer
    // into the init method. However, since there are no uses of this in any written
    // program, we currently prohibit it.
    initializeOwnedBufferHandles(b, InitializeOptions::DoNotIncludeThreadLocalScalars);
    // any kernel can set termination on initialization
    Type * termSignalTy;
    std::tie(mTerminationSignalPtr, termSignalTy) = getScalarFieldPtr(b, TERMINATION_SIGNAL);
    b.CreateStore(b.getSize(KernelBuilder::TerminationCode::None), mTerminationSignalPtr);
    mTarget->generateInitializeMethod(b);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect) && sharedStateTy)) {
        b.CreateMProtect(sharedStateTy, mSharedHandle, CBuilder::Protect::READ);
    }
    b.CreateRet(b.CreateLoad(termSignalTy, mTerminationSignalPtr));
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief bindFamilyInitializationArguments
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::callGenerateExpectedOutputSizeMethod(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) {
    assert (mTarget->getNumOfStreamInputs() == 0);
    mCurrentMethod = mTarget->getExpectedOutputSizeFunction(b, true, linkageType);
    assert (mCurrentMethod->empty());
    mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
    b.SetInsertPoint(mEntryPoint);
    auto arg = mCurrentMethod->arg_begin();
    #ifndef NDEBUG
    const auto arg_end = mCurrentMethod->arg_end();
    #endif
    auto nextArg = [&]() {
        assert (arg != arg_end);
        Value * const v = &*arg;
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };
    StructType * sharedStateTy = mTarget->getSharedStateType(b.getContext());
    if (LLVM_LIKELY(sharedStateTy)) {
        setHandle(nextArg());
    }
    initializeScalarMap(b, InitializeOptions::DoNotIncludeThreadLocalScalars);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect) && sharedStateTy)) {
        b.CreateMProtect(sharedStateTy, mSharedHandle, CBuilder::Protect::WRITE);
    }
    Value * const retVal = mTarget->generateExpectedOutputSizeMethod(b);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect) && sharedStateTy)) {
        b.CreateMProtect(sharedStateTy, mSharedHandle, CBuilder::Protect::READ);
    }
    b.CreateRet(retVal);
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief bindFamilyInitializationArguments
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::bindAdditionalInitializationArguments(KernelBuilder & /* b */, ArgIterator & /* arg */, const ArgIterator & /* arg_end */) {

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateInitializeThreadLocalMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateInitializeThreadLocalMethod(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) {

    assert (mSharedHandle == nullptr && mThreadLocalHandle == nullptr);
    mCurrentMethod = mTarget->getInitializeThreadLocalFunction(b, true, linkageType);
    assert (mCurrentMethod->empty());
    mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
    b.SetInsertPoint(mEntryPoint);
    auto arg = mCurrentMethod->arg_begin();
    auto nextArg = [&]() {
        assert (arg != mCurrentMethod->arg_end());
        Value * const v = &*arg;
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };
    PointerType * const ptrTy = PointerType::getUnqual(b.getContext());
    if (LLVM_LIKELY(mTarget->getSharedStateType())) {
        setHandle(nextArg());
    }
    StructType * const threadLocalTy = mTarget->getThreadLocalStateType();
    Value * const providedState = nextArg();
    BasicBlock * const allocThreadLocal = BasicBlock::Create(b.getContext(), "allocThreadLocalState", mCurrentMethod);
    BasicBlock * const initThreadLocal = BasicBlock::Create(b.getContext(), "initThreadLocalState", mCurrentMethod);
    b.CreateCondBr(b.CreateIsNull(providedState), allocThreadLocal, initThreadLocal);

    b.SetInsertPoint(allocThreadLocal);
    auto & DL = b.getModule()->getDataLayout();
    Constant * const threadLocalTySize = b.getTypeSize(threadLocalTy);
    const auto align = DL.getABITypeAlign(threadLocalTy).value();
    assert (boost::gcd<size_t>(align, b.getPageSize()) == align);
    Value * allocedState = b.CreatePageAlignedMalloc(threadLocalTySize);
    b.CreateMemZero(allocedState, threadLocalTySize, align);
    b.CreateBr(initThreadLocal);

    b.SetInsertPoint(initThreadLocal);
    PHINode * const threadLocal = b.CreatePHI(ptrTy, 2);
    threadLocal->addIncoming(providedState, mEntryPoint);
    threadLocal->addIncoming(allocedState, allocThreadLocal);

    const auto ea = codegen::DebugOptionIsSet(codegen::EnableAsserts);
    if (LLVM_UNLIKELY(ea)) {
        auto & dl = b.getModule()->getDataLayout();
        const auto align = CBuilder::getAlignOf(dl, threadLocalTy);
        if (LLVM_LIKELY(align > 1U)) {
        Value * handleInt = b.CreatePtrToInt(threadLocal, b.getSizeTy());
        b.CreateAssertZero(b.CreateURem(handleInt, b.getSize(align)),
                           "%s thread local handle addresss (x%" PRIx64 ") is misaligned for state type (%" PRIu64 ")",
                           b.GetString("InitializeThreadLocal"), handleInt, b.getSize(align));
        }
    }
    mThreadLocalHandle = threadLocal;
    initializeScalarMap(b, InitializeOptions::IncludeThreadLocalScalars);
    mTarget->generateInitializeThreadLocalMethod(b);
    b.CreateRet(threadLocal);

    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callAllocateSharedInternalStreamSets
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateAllocateSharedInternalStreamSets(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) {
    // NOTE: the kernel compiler must call this AFTER initialization
    if (LLVM_UNLIKELY(mTarget->allocatesInternalStreamSets())) {
        assert (mSharedHandle == nullptr && mThreadLocalHandle == nullptr);
        mCurrentMethod = mTarget->getAllocateSharedInternalStreamSetsFunction(b, true, linkageType);
        assert (mCurrentMethod->empty());
        mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
        b.SetInsertPoint(mEntryPoint);
        auto arg = mCurrentMethod->arg_begin();
        auto nextArg = [&]() {
            assert (arg != mCurrentMethod->arg_end());
            Value * const v = &*arg;
            assert (&v->getContext() == &b.getContext());
            assert (&v->getType()->getContext() == &b.getContext());
            std::advance(arg, 1);
            return v;
        };
        if (LLVM_LIKELY(mTarget->getSharedStateType())) {
            setHandle(nextArg());
        }
        Value * const expectedNumOfStrides = nextArg();
        const auto imss = (mTarget->getKernelFlags() & Kernel::KernelFlags::HasInternallyManagedStreamSet);
        if (LLVM_UNLIKELY(imss && codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers))) {
            mReportExpansionCallback = nextArg();
            mPipelineHandle = nextArg();
        }
        initializeScalarMap(b, InitializeOptions::DoNotIncludeThreadLocalScalars);
        initializeOwnedBufferHandles(b, InitializeOptions::DoNotIncludeThreadLocalScalars, expectedNumOfStrides);
        mTarget->generateAllocateSharedInternalStreamSetsMethod(b, expectedNumOfStrides);
        b.CreateRetVoid();
        // b.getDriver().declareFunctionSymbol(mCurrentMethod);
        clearInternalStateAfterCodeGen();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callAllocateThreadLocalInternalStreamSets
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateAllocateThreadLocalInternalStreamSets(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) {
    if (LLVM_UNLIKELY(mTarget->allocatesInternalStreamSets())) {
        assert (mSharedHandle == nullptr && mThreadLocalHandle == nullptr);
        mCurrentMethod = mTarget->getAllocateThreadLocalInternalStreamSetsFunction(b, true, linkageType);
        assert (mCurrentMethod->empty());
        mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
        b.SetInsertPoint(mEntryPoint);
        auto arg = mCurrentMethod->arg_begin();
        auto nextArg = [&]() {
            assert (arg != mCurrentMethod->arg_end());
            Value * const v = &*arg;
            assert (&v->getContext() == &b.getContext());
            assert (&v->getType()->getContext() == &b.getContext());
            std::advance(arg, 1);
            return v;
        };
        if (LLVM_LIKELY(mTarget->getSharedStateType())) {
            setHandle(nextArg());
        }
        setThreadLocalHandle(nextArg());
        Value * const expectedNumOfStrides = nextArg();
        initializeScalarMap(b, InitializeOptions::IncludeThreadLocalScalars);
        initializeOwnedBufferHandles(b, InitializeOptions::IncludeThreadLocalScalars);
        mTarget->generateAllocateThreadLocalInternalStreamSetsMethod(b, expectedNumOfStrides);
        b.CreateRetVoid();
        // b.getDriver().declareFunctionSymbol(mCurrentMethod);
        clearInternalStateAfterCodeGen();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLCMOfFixedRateInputs
 ** ------------------------------------------------------------------------------------------------------------- */
/* static */ Rational KernelCompiler::getLCMOfFixedRateInputs(const Kernel * const target) {
    Rational rateLCM(1);
    bool first = true;
    for (const Binding & input : target->getInputStreamSetBindings()) {
        const ProcessingRate & rate = input.getRate();
        if (LLVM_LIKELY(rate.isFixed())) {
            if (first) {
                rateLCM = rate.getRate();
                first = false;
            } else {
                rateLCM = lcm(rateLCM, rate.getRate());
            }
        }
    }
    return rateLCM;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setDoSegmentProperties
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::setDoSegmentProperties(KernelBuilder & b, const ArrayRef<Value *> args) {

    // WARNING: any change to this must be reflected in Kernel::addDoSegmentDeclaration,
    // Kernel::getDoSegmentFields, KernelCompiler::getDoSegmentProperties,
    // PipelineCompiler::buildKernelCallArgumentList and PipelineKernel::addOrDeclareMainFunction

    auto arg = args.begin();
    auto nextArg = [&]() {
        assert (arg != args.end());
        Value * const v = *arg; assert (v);
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };

    const auto enableAsserts = codegen::DebugOptionIsSet(codegen::EnableAsserts);

    StructType * sharedStateTy = mTarget->getSharedStateType();

    if (LLVM_LIKELY(sharedStateTy)) {
        setHandle(nextArg());
        if (LLVM_UNLIKELY(enableAsserts)) {
            b.CreateAssert(getHandle(), "%s: shared handle cannot be null", b.GetString(getName()));

            auto & dl = b.getModule()->getDataLayout();
            const auto align = CBuilder::getAlignOf(dl, sharedStateTy);
            if (LLVM_LIKELY(align > 1U)) {
            Value * handleInt = b.CreatePtrToInt(getHandle(), b.getSizeTy());
            b.CreateAssertZero(b.CreateURem(handleInt, b.getSize(align)),
                               "%s shared handle addresss (x%" PRIx64 ") is misaligned for shared state type (%" PRIu64 ")",
                               b.GetString("DoSegment"), handleInt, b.getSize(align));
            }
        }
    }

    StructType * threadLocalStateTy = mTarget->getThreadLocalStateType();

    if (LLVM_UNLIKELY(threadLocalStateTy)) {
        setThreadLocalHandle(nextArg());
        if (LLVM_UNLIKELY(enableAsserts)) {
            b.CreateAssert(getThreadLocalHandle(), "%s: thread local handle cannot be null", b.GetString(getName()));

            auto & dl = b.getModule()->getDataLayout();
            const auto align = CBuilder::getAlignOf(dl, threadLocalStateTy);
            if (LLVM_LIKELY(align > 1U)) {
            Value * handleInt = b.CreatePtrToInt(getThreadLocalHandle(), b.getSizeTy());
            b.CreateAssertZero(b.CreateURem(handleInt, b.getSize(align)),
                               "%s thread local handle addresss (x%" PRIx64 ") is misaligned for state type (%" PRIu64 ")",
                               b.GetString("DoSegment"), handleInt, b.getSize(align));
            }
        }
    }
    const auto internallySynchronized = mTarget->hasAttribute(AttrId::InternallySynchronized);
    // TODO: the simplest way of ensuring we can allow external I/O to be passed though the main pipeline
    // even if there are multiple consumers of the input with differing processing rates is to special
    // case the outermost pipeline such that all I/O is always addressible. This creates a problem,
    // however, in that we will not be able to optimize a single kernel program by having the main
    // function call it directly instead of a pipeline. Given that this is not a realistic use-case,
    // we're ignoring that limitation for now.
    const auto isPipeline = (mTarget->getTypeId() == Kernel::TypeId::Pipeline);
    const auto isMainPipeline = isPipeline && !internallySynchronized;

    Rational fixedRateLCM{0};
    mFixedRateFactor = nullptr;

    if (LLVM_UNLIKELY(internallySynchronized)) {
        mExternalSegNo = nextArg();
    }
    mRawNumOfStrides = nextArg();

    if (LLVM_UNLIKELY(isMainPipeline)) {
        mIsFinal = b.getTrue();
        mNumOfStrides = mRawNumOfStrides;
    } else {
        mIsFinal = b.CreateIsNull(mRawNumOfStrides);
        if (LLVM_UNLIKELY(mTarget->hasAttribute(AttrId::MustExplicitlyTerminate))) {
            // mIsFinal = nullptr;
            mNumOfStrides = mRawNumOfStrides;
        } else {
            // mIsFinal = b.CreateIsNull(mRawNumOfStrides);
            mNumOfStrides = b.CreateSelect(mIsFinal, b.getSize(1), mRawNumOfStrides);
        }
        if (LLVM_LIKELY(mTarget->hasFixedRateIO())) {
            fixedRateLCM = getLCMOfFixedRateInputs(mTarget);
            mFixedRateFactor = nextArg();
        }
        #ifdef ENABLE_PAPI
        if (LLVM_UNLIKELY(codegen::PapiCounterOptions.compare(codegen::OmittedOption) != 0)) {
            mPAPIEventSetId = nextArg();
        }
        #endif
    }

    initializeScalarMap(b, InitializeOptions::IncludeThreadLocalScalars);

    // NOTE: the disadvantage of passing the stream pointers as a parameter is that it becomes more difficult
    // to access a stream set from a LLVM function call. We could create a stream-set aware function creation
    // and call system here but that is not an ideal way of handling this.

    const auto numOfInputs = getNumOfStreamInputs();

    const auto checkStreamSet = codegen::DebugOptionIsSet(codegen::EnableAsserts, codegen::EnableStreamSetAsserts);

    IntegerType * const sizeTy = b.getSizeTy();
    for (unsigned i = 0; i < numOfInputs; i++) {

        /// ----------------------------------------------------
        /// virtual base address
        /// ----------------------------------------------------
        StreamSetBuffer * const buffer = mStreamSetInputBuffers[i].get();

        const Binding & input = mInputStreamSets[i];
        Value * const virtualBaseAddress = nextArg();
        Value * const localHandle = b.CreateAllocaAtEntryPoint(buffer->getHandleType(b));

        buffer->setHandle(localHandle); assert (localHandle);
        buffer->setBaseAddress(b, virtualBaseAddress);

        if (LLVM_UNLIKELY(checkStreamSet) && LLVM_LIKELY(!input.hasAttribute(AttrId::AllowsUnalignedAccess))) {
            auto & dl = b.getModule()->getDataLayout();
            Type * intPtrTy = dl.getIntPtrType(b.getContext());
            Value * vbaInt = b.CreatePtrToInt(buffer->getBaseAddress(b), intPtrTy);
            Constant * alignInt = ConstantInt::get(intPtrTy, b.getAlignOf(dl, buffer->getType(b)));
            Value * modVBA = b.CreateURem(vbaInt, alignInt);
            b.CreateAssertZero(modVBA, "%s virtual base address 0x%" PRIx64 " is not a multiple of alignment 0x%" PRIx64,
                               b.GetString(mInputStreamSets[i].getName()), vbaInt, alignInt);
        }

        if (LLVM_UNLIKELY(internallySynchronized)) {
            Value * const closed = nextArg();
            mInputIsClosed[i] = b.CreateIsNotNull(closed);
        } else {
            mInputIsClosed[i] = mIsFinal;
        }

        /// ----------------------------------------------------
        /// processed item count
        /// ----------------------------------------------------

        const ProcessingRate & rate = input.getRate();
        Value * processed = nullptr;
        if (isMainPipeline || isAddressable(input)) {
            mProcessedInputItemPtr[i] = nextArg();
            processed = b.CreateLoad(sizeTy, mProcessedInputItemPtr[i]);
        } else {
            if (LLVM_LIKELY(isCountable(input))) {
                processed = nextArg();
            } else { // isRelative
                const auto port = getStreamPort(rate.getReference());
                assert (port.Type == PortType::Input && port.Number < i);
                assert (mProcessedInputItemPtr[port.Number]);
                Value * const ref = b.CreateLoad(sizeTy, mProcessedInputItemPtr[port.Number]);
                processed = b.CreateMulRational(ref, rate.getRate());
            }
            // NOTE: we create a redundant alloca to store the input param so that
            // Mem2Reg can convert it into a PHINode if the item count is updated in
            // a loop; otherwise, it will be discarded in favor of the param itself.
            Value * const processedItems = b.CreateAllocaAtEntryPoint(sizeTy);
            b.CreateStore(processed, processedItems);
            mProcessedInputItemPtr[i] = processedItems;
        }
        /// ----------------------------------------------------
        /// accessible item count
        /// ----------------------------------------------------
        Value * accessible = nullptr;
        if (LLVM_UNLIKELY(isMainPipeline || requiresItemCount(input))) {
            accessible = nextArg();
        } else {
            accessible = b.CreateCeilUMulRational(mFixedRateFactor, rate.getRate() / fixedRateLCM);
        }
        assert (accessible);
        assert (accessible->getType() == sizeTy);
        mAccessibleInputItems[i] = accessible;
        /// ----------------------------------------------------
        /// available
        /// ----------------------------------------------------
        Value * const avail = b.CreateAdd(processed, accessible);
        mAvailableInputItems[i] = avail;
        /// ----------------------------------------------------
        /// capacity
        /// ----------------------------------------------------
        Value * capacity = avail;
        if (LLVM_UNLIKELY(checkStreamSet)) {
            capacity = nextArg(); assert (capacity->getType()->isIntegerTy());
        }
        buffer->setCapacity(b, capacity);
    }

    // set all of the output buffers
    const auto numOfOutputs = getNumOfStreamOutputs();

    const auto canTerminate = canSetTerminateSignal();

    for (unsigned i = 0; i < numOfOutputs; i++) {

        /// ----------------------------------------------------
        /// logical buffer base address
        /// ----------------------------------------------------
        StreamSetBuffer * const buffer = mStreamSetOutputBuffers[i].get();

        const Binding & output = mOutputStreamSets[i];
        const auto isLocal =  Kernel::isLocalBuffer(output);
        if (LLVM_UNLIKELY(isLocal.isShared())) {
            Value * const handle = nextArg();
            assert (buffer->isDynamic());
            buffer->setHandle(handle);
        } else if (LLVM_UNLIKELY(isMainPipeline || isLocal.any())) {
            // If an output is a managed buffer, the address is stored within the state instead
            // of being passed in through the function call.
            mUpdatableOutputBaseVirtualAddressPtr[i] = nextArg();
            Value * handle = getScalarFieldPtr(b, output.getName() + BUFFER_HANDLE_SUFFIX).first;
            buffer->setHandle(handle);
        } else {
            assert (isa<ExternalBuffer>(buffer));
            Value * const virtualBaseAddress = nextArg(); assert (virtualBaseAddress->getType()->isPointerTy());
            Value * const localHandle = b.CreateAllocaAtEntryPoint(buffer->getHandleType(b));
            buffer->setHandle(localHandle);
            buffer->setBaseAddress(b, virtualBaseAddress);
        }

        if (LLVM_UNLIKELY(checkStreamSet) && LLVM_LIKELY(!output.hasAttribute(AttrId::AllowsUnalignedAccess))) {
            auto & dl = b.getModule()->getDataLayout();
            Type * intPtrTy = dl.getIntPtrType(b.getContext());
            Value * vbaInt = b.CreatePtrToInt(buffer->getBaseAddress(b), intPtrTy);
            Constant * alignInt = ConstantInt::get(intPtrTy, b.getAlignOf(dl, buffer->getType(b)));
            Value * modVBA = b.CreateURem(vbaInt, alignInt);
            b.CreateAssertZero(modVBA, "%s virtual base address 0x%" PRIx64 " is not a multiple of alignment 0x%" PRIx64,
                               b.GetString(mOutputStreamSets[i].getName()), vbaInt, alignInt);
        }

        /// ----------------------------------------------------
        /// produced item count
        /// ----------------------------------------------------
        const ProcessingRate & rate = output.getRate();
        Value * produced = nullptr;
        if (LLVM_LIKELY(canTerminate || isMainPipeline || isAddressable(output))) {
            mProducedOutputItemPtr[i] = nextArg();
            produced = b.CreateLoad(sizeTy, mProducedOutputItemPtr[i]);
        } else {
            if (LLVM_LIKELY(isCountable(output))) {
                produced = nextArg();
            } else { // isRelative
                // For now, if something is produced at a relative rate to another stream in a kernel that
                // may terminate, its final item count is inherited from its reference stream and cannot
                // be set independently. Should they be independent at early termination?
                const auto port = getStreamPort(rate.getReference());
                assert (port.Type == PortType::Input || (port.Type == PortType::Output && port.Number < i));
                const auto & items = (port.Type == PortType::Input) ? mProcessedInputItemPtr : mProducedOutputItemPtr;
                Value * const ref = b.CreateLoad(sizeTy, items[port.Number]);
                produced = b.CreateMulRational(ref, rate.getRate());
            }
            Value * const producedItems = b.CreateAllocaAtEntryPoint(sizeTy);
            b.CreateStore(produced, producedItems);
            mProducedOutputItemPtr[i] = producedItems;
        }
        assert (produced);
        assert (produced->getType() == sizeTy);
        mInitiallyProducedOutputItems[i] = produced;

        /// ----------------------------------------------------
        /// writable / consumed item count
        /// ----------------------------------------------------
        Value * writable = nullptr;
        assert (buffer->isDynamic() || !isLocal.isManaged());
        if (isLocal.any()) {
            Value * const consumed = nextArg();
            assert (consumed->getType() == sizeTy);
            mConsumedOutputItems[i] = consumed;
            if (LLVM_UNLIKELY(checkStreamSet && !isLocal.isShared())) {
                mUpdatableOutputCapacityPtr[i] = nextArg();
                assert (mUpdatableOutputCapacityPtr[i]->getType()->isPointerTy());
            }
            buffer->freePendingDeletions(b, consumed);
            writable = buffer->getLinearlyWritableItems(b, produced, consumed, nullptr);
            assert (writable && writable->getType() == sizeTy);
        } else {
            if (isMainPipeline || requiresItemCount(output)) {
                writable = nextArg();
                assert (writable && writable->getType() == sizeTy);
            } else if (mFixedRateFactor) {
                writable = b.CreateCeilUMulRational(mFixedRateFactor, rate.getRate() / fixedRateLCM);
                assert (writable && writable->getType() == sizeTy);
            }
            /// ----------------------------------------------------
            /// capacity
            /// ----------------------------------------------------
            Value * capacity = nullptr;
            if (LLVM_UNLIKELY(checkStreamSet)) {
                capacity = nextArg(); assert (capacity->getType()->isIntegerTy());
            } else if (writable) {
                capacity = b.CreateAdd(produced, writable);
            } else {
                capacity = ConstantInt::getAllOnesValue(sizeTy);
            }
            buffer->setCapacity(b, capacity);
        }
        mWritableOutputItems[i] = writable;
    }

    const auto hasManagedOutput = (mTarget->getKernelFlags() & Kernel::KernelFlags::HasInternallyManagedStreamSet);

    if (LLVM_UNLIKELY(hasManagedOutput && codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers))) {
        mReportExpansionCallback = nextArg();
        mPipelineHandle = nextArg();
    }
    assert (arg == args.end());

    // initialize the termination signal if this kernel can set it
    mTerminationSignalPtr = nullptr;
    if (internallySynchronized || canTerminate) {
        mTerminationSignalPtr = getScalarFieldPtr(b, TERMINATION_SIGNAL).first;
        if (LLVM_UNLIKELY(enableAsserts)) {
            Value * const unterminated =
                b.CreateICmpEQ(b.CreateLoad(sizeTy, mTerminationSignalPtr), b.getSize(KernelBuilder::TerminationCode::None));
            b.CreateAssert(unterminated, getName() + ".doSegment was called after termination?");
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getDoSegmentProperties
 *
 * Reverse of the setDoSegmentProperties operation; used by the PipelineKernel when constructing internal threads
 * to simplify passing of the state data.
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Value *> KernelCompiler::getDoSegmentProperties(KernelBuilder & b) const {

    // WARNING: any change to this must be reflected in addDoSegmentDeclaration, getDoSegmentFields,
    // setDoSegmentProperties, and PipelineCompiler::writeKernelCall

    std::vector<Value *> props;

    Function * const doSegFunc = mTarget->getDoSegmentFunction(b, true, GlobalValue::ExternalLinkage);

    props.reserve(doSegFunc->getNumOperands());
    if (LLVM_LIKELY(mSharedHandle)) {
        props.push_back(mSharedHandle); assert (mSharedHandle);
    }
    if (LLVM_UNLIKELY(mThreadLocalHandle)) {
        props.push_back(mThreadLocalHandle); assert (mThreadLocalHandle);
    }
    const auto internallySynchronized = mTarget->hasAttribute(AttrId::InternallySynchronized);
    const auto isPipeline = (mTarget->getTypeId() == Kernel::TypeId::Pipeline);
    const auto isMainPipeline = isPipeline && !internallySynchronized;

    if (LLVM_UNLIKELY(internallySynchronized)) {
        props.push_back(mExternalSegNo);
    }

    props.push_back(mNumOfStrides); assert (mNumOfStrides);

    if (LLVM_LIKELY(!isMainPipeline)) {
        if (LLVM_LIKELY(mTarget->hasFixedRateIO())) {
            props.push_back(mFixedRateFactor);
        }
        #ifdef ENABLE_PAPI
        if (LLVM_UNLIKELY(codegen::PapiCounterOptions.compare(codegen::OmittedOption) != 0)) {
            props.push_back(mPAPIEventSetId);
        }
        #endif
    }

    const auto checkStreamSet = codegen::DebugOptionIsSet(codegen::EnableAsserts, codegen::EnableStreamSetAsserts);

    IntegerType * const sizeTy = b.getSizeTy();
    const auto numOfInputs = getNumOfStreamInputs();
    for (unsigned i = 0; i < numOfInputs; i++) {
        /// ----------------------------------------------------
        /// logical buffer base address
        /// ----------------------------------------------------
        const auto & buffer = mStreamSetInputBuffers[i];
        props.push_back(buffer->getBaseAddress(b));
        /// ----------------------------------------------------
        /// is closed
        /// ----------------------------------------------------
        if (LLVM_UNLIKELY(internallySynchronized)) {
            props.push_back(mInputIsClosed[i]);
        }
        /// ----------------------------------------------------
        /// processed item count
        /// ----------------------------------------------------
        const Binding & input = mInputStreamSets[i];
        if (isMainPipeline || isAddressable(input)) {
            props.push_back(mProcessedInputItemPtr[i]);
        } else if (LLVM_LIKELY(isCountable(input))) {
            props.push_back(b.CreateLoad(sizeTy, mProcessedInputItemPtr[i]));
        }
        /// ----------------------------------------------------
        /// accessible item count
        /// ----------------------------------------------------
        if (isMainPipeline || requiresItemCount(input)) {
            props.push_back(mAccessibleInputItems[i]);
        }
        if (LLVM_UNLIKELY(checkStreamSet)) {
            props.push_back(buffer->getCapacity(b));
        }
    }

    // set all of the output buffers
    const auto numOfOutputs = getNumOfStreamOutputs();
    const auto canTerminate = canSetTerminateSignal();

    for (unsigned i = 0; i < numOfOutputs; i++) {
        /// ----------------------------------------------------
        /// logical buffer base address
        /// ----------------------------------------------------
        const auto & buffer = mStreamSetOutputBuffers[i];
        const Binding & output = mOutputStreamSets[i];
        const auto isLocal = Kernel::isLocalBuffer(output);

        Value * handle = nullptr;
        if (LLVM_UNLIKELY(isLocal.isShared())) {
            handle = buffer->getHandle();
        } else if (LLVM_UNLIKELY(isMainPipeline || isLocal.any())) {
            // If an output is a managed buffer, the address is stored within the state instead
            // of being passed in through the function call.
            handle = mUpdatableOutputBaseVirtualAddressPtr[i];
        } else {
            handle = buffer->getBaseAddress(b);
        }
        props.push_back(handle);

        /// ----------------------------------------------------
        /// produced item count
        /// ----------------------------------------------------
        if (LLVM_LIKELY(canTerminate || isMainPipeline ||isAddressable(output))) {
            props.push_back(mProducedOutputItemPtr[i]);
        } else if (LLVM_LIKELY(isCountable(output))) {
            props.push_back(b.CreateLoad(sizeTy, mProducedOutputItemPtr[i]));
        }
        /// ----------------------------------------------------
        /// writable / consumed item count
        /// ----------------------------------------------------
        if (LLVM_UNLIKELY(isLocal.any())) {
            props.push_back(mConsumedOutputItems[i]);
            if (LLVM_UNLIKELY(checkStreamSet && !isLocal.isShared())) {
                props.push_back(mUpdatableOutputCapacityPtr[i]);
            }
        } else {
            if (isMainPipeline || requiresItemCount(output)) {
                props.push_back(mWritableOutputItems[i]);
            }
            if (LLVM_UNLIKELY(checkStreamSet)) {
                props.push_back(buffer->getCapacity(b));
            }
        }
    }
    if (LLVM_UNLIKELY(mReportExpansionCallback != nullptr)) {
        assert (codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers));
        props.push_back(mReportExpansionCallback);
        assert (mPipelineHandle);
        props.push_back(mPipelineHandle);
    }
    return props;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateDoSegmentMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateDoSegmentMethod(KernelBuilder & b, llvm::TargetMachine * TM, GlobalValue::LinkageTypes linkageType) {

    assert (mInputStreamSets.size() == mStreamSetInputBuffers.size());
    assert (mOutputStreamSets.size() == mStreamSetOutputBuffers.size());

    mCurrentMethod = mTarget->getDoSegmentFunction(b, true, linkageType);
    assert (mCurrentMethod->empty());
    mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
    b.SetInsertPoint(mEntryPoint);
    assert (mCurrentMethod == b.GetInsertBlock()->getParent());

    BEGIN_SCOPED_REGION
    Vec<Value *, 64> args;
    args.reserve(mCurrentMethod->arg_size());
    for (auto ArgI = mCurrentMethod->arg_begin(); ArgI != mCurrentMethod->arg_end(); ++ArgI) {
        args.push_back(&(*ArgI));
    }
    setDoSegmentProperties(b, args);
    END_SCOPED_REGION

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b.CreateMProtect(mTarget->getSharedStateType(), mSharedHandle, CBuilder::Protect::WRITE);
    }
    assert (mCurrentMethod == b.GetInsertBlock()->getParent());
    mTarget->generateKernelMethod(b, TM);
    assert (mCurrentMethod == b.GetInsertBlock()->getParent());

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b.CreateMProtect(mTarget->getSharedStateType(), mSharedHandle, CBuilder::Protect::READ);
    }

    const auto numOfOutputs = getNumOfStreamOutputs();

    IntegerType * const sizeTy = b.getSizeTy();

    for (unsigned i = 0; i < numOfOutputs; i++) {
        // Write the virtual base address out to inform the pipeline of any changes
        const auto & buffer = mStreamSetOutputBuffers[i];
        if (mUpdatableOutputBaseVirtualAddressPtr[i]) {
            Value * const baseAddress = buffer->getBaseAddress(b);
            if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
                SmallVector<char, 256> tmp;
                raw_svector_ostream out(tmp);
                const Binding & output = mOutputStreamSets[i];
                out << getName() << ":%s is returning a virtual base address "
                                    "computed from a null base address.";
                b.CreateAssert(baseAddress, out.str(), b.GetString(output.getName()));
            }
            Value * vba = buffer->getVirtualBasePtr(b, baseAddress, mConsumedOutputItems[i]);

            assert (isFromCurrentFunction(b, mUpdatableOutputBaseVirtualAddressPtr[i], true));

            b.CreateStore(vba, mUpdatableOutputBaseVirtualAddressPtr[i]);
        }
        if (LLVM_UNLIKELY(mUpdatableOutputCapacityPtr[i])) {
            assert (isFromCurrentFunction(b, mUpdatableOutputCapacityPtr[i], true));
            Value * const capacity = buffer->getCapacity(b);
            b.CreateStore(capacity, mUpdatableOutputCapacityPtr[i]);
        }
    }

    // return the termination signal (if one exists)
    if (mTerminationSignalPtr) {
        assert (isFromCurrentFunction(b, mTerminationSignalPtr, true));
        b.CreateRet(b.CreateAlignedLoad(sizeTy, mTerminationSignalPtr, sizeof(size_t)));
        mTerminationSignalPtr = nullptr;
    } else {
        b.CreateRetVoid();
    }
    assert (mCurrentMethod == b.GetInsertBlock()->getParent());
    // b.getDriver().declareFunctionSymbol(mCurrentMethod);
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateFinalizeThreadLocalMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateFinalizeThreadLocalMethod(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) {
    mCurrentMethod = mTarget->getFinalizeThreadLocalFunction(b, true, linkageType);
    assert (mCurrentMethod->empty());
    mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
    b.SetInsertPoint(mEntryPoint);
    auto arg = mCurrentMethod->arg_begin();
    auto nextArg = [&]() {
        assert (arg != mCurrentMethod->arg_end());
        Value * const v = &*arg;
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };
    if (LLVM_LIKELY(mTarget->getSharedStateType())) {
        setHandle(nextArg());
    }
    mCommonThreadLocalHandle = nextArg();
    mThreadLocalHandle = nextArg();
    initializeScalarMap(b, InitializeOptions::IncludeAndAutomaticallyAccumulateThreadLocalScalars);
    mTarget->generateFinalizeThreadLocalMethod(b);
    b.CreateRetVoid();
    // b.getDriver().declareFunctionSymbol(mCurrentMethod);
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief callGenerateFinalizeMethod
 ** ------------------------------------------------------------------------------------------------------------- */
inline void KernelCompiler::callGenerateFinalizeMethod(KernelBuilder & b, llvm::GlobalValue::LinkageTypes linkageType) {
    mCurrentMethod = mTarget->getFinalizeFunction(b, true, linkageType);
    assert (mCurrentMethod->empty());
    mEntryPoint = BasicBlock::Create(b.getContext(), "entry", mCurrentMethod);
    b.SetInsertPoint(mEntryPoint);
    auto arg = mCurrentMethod->arg_begin();
    auto nextArg = [&]() {
        assert (arg != mCurrentMethod->arg_end());
        Value * const v = &*arg;
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };
    if (LLVM_LIKELY(mTarget->getSharedStateType())) {
        setHandle(nextArg());
    }
    if (LLVM_LIKELY(mTarget->getThreadLocalStateType())) {
        setThreadLocalHandle(nextArg());
    }
    assert (arg == mCurrentMethod->arg_end());
    initializeScalarMap(b, InitializeOptions::IncludeThreadLocalScalars);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b.CreateMProtect(mTarget->getSharedStateType(), mSharedHandle,CBuilder::Protect::WRITE);
    }
    initializeOwnedBufferHandles(b, InitializeOptions::DoNotIncludeThreadLocalScalars);
    mTarget->generateFinalizeMethod(b); // may be overridden by the Kernel subtype
    const auto outputs = getFinalOutputScalars(b);
    if (outputs.empty()) {
        b.CreateRetVoid();
    } else {
        const auto n = outputs.size();
        if (n == 1) {
            b.CreateRet(outputs[0]);
        } else {
            b.CreateAggregateRet(outputs.data(), n);
        }
    }
    // b.getDriver().declareFunctionSymbol(mCurrentMethod);
    clearInternalStateAfterCodeGen();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFinalOutputScalars
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Value *> KernelCompiler::getFinalOutputScalars(KernelBuilder & b) {
    const auto n = mOutputScalars.size();
    std::vector<Value *> outputs(n);
    for (unsigned i = 0; i < n; ++i) {
        auto ref = getScalarFieldPtr(b, mOutputScalars[i].getName());
        outputs[i] = b.CreateLoad(ref.second, ref.first);
    }
    return outputs;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addToGroupCountMap
 ** ------------------------------------------------------------------------------------------------------------- */
static void addToGroupCountMap(flat_map<size_t, size_t> & groups, const size_t groupNum) {
    auto f = groups.find(groupNum);
    if (f == groups.end()) {
        groups.emplace(groupNum, 1U);
    } else {
        f->second++;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief computePartialSumOfGroupCounts
 ** ------------------------------------------------------------------------------------------------------------- */
static size_t computePartialSumOfGroupCounts(flat_map<size_t, size_t> & groups, const size_t initialCount) {
    if (groups.empty()) return 0UL;
    auto itr = groups.begin();
    const auto end = groups.end();
    #ifndef NDEBUG
    auto prior = itr->first;
    #endif
    auto partSum = itr->second + initialCount;
    itr->second = initialCount;
    while (++itr != end) {
        #ifndef NDEBUG
        assert (prior < itr->first);
        prior = itr->first;
        #endif
        const auto groupCount = itr->second;
        itr->second = partSum;
        partSum += groupCount;
    }
    return partSum;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeScalarMap
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::initializeScalarMap(KernelBuilder & b, const InitializeOptions options) {

    Module * const m = b.getModule();
    StructType * const sharedTy =  mTarget->getSharedStateType(b.getContext());
    StructType * const threadLocalTy = mTarget->getThreadLocalStateType(b.getContext());
    assert (sharedTy == nullptr || &sharedTy->getContext() == &b.getContext());
    assert (threadLocalTy == nullptr || &threadLocalTy->getContext() == &b.getContext());

    auto & DL = m->getDataLayout();

    #ifndef NDEBUG
    auto verifyStateType = [&](Value * const handle, StructType * const stateType) {
        if (handle == nullptr && stateType == nullptr) {
            return true;
        }
        if (handle == nullptr || stateType == nullptr) {
            if (handle) {
                errs() << "handle has null expected type\n";
            } else {
                errs() << "type has null expected handle\n";
            }
            return false;
        }
        assert (!stateType->isOpaque());
        assert (stateType->isSized());
        assert (stateType->isPacked());
        return true;
    };
    assert ("incorrect shared handle/type!" && verifyStateType(mSharedHandle, sharedTy));
    if (options == InitializeOptions::IncludeThreadLocalScalars) {
        assert ("incorrect thread local handle/type!" && verifyStateType(mThreadLocalHandle, threadLocalTy));
    }
    #endif

    mScalarFieldMap.clear();
    mScalarAliasMap.clear();

    auto addToScalarFieldMap = [&](StringRef bindingName, Value * const scalar, Type * const expectedType, Type * const actualType) {
        assert (&b.getContext() == &actualType->getContext());
        const auto i = mScalarFieldMap.insert(std::make_pair(bindingName, std::make_pair(scalar, actualType)));
        if (LLVM_UNLIKELY(!i.second)) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);
            out << "Kernel " << getName() << " contains two scalar or alias fields named " << bindingName;
            report_fatal_error(Twine(out.str()));
        }
        #ifndef NDEBUG
        Type * const ty = CBuilder::convertTypeToLLVMContext(b.getContext(), expectedType);
        if (LLVM_UNLIKELY(actualType != ty)) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);
            out << "Scalar " << getName() << '.' << bindingName << " was expected to be a ";
            ty->print(out);
            out << " but was stored as a ";
            actualType->print(out);
            report_fatal_error(Twine(out.str()));
        }
        #endif
    };

    flat_map<size_t, size_t> sharedGroups;
    flat_map<size_t, size_t> threadLocalGroups;

    bool hasThreadLocalAccum = false;

    for (const auto & scalar : mInternalScalars) {
        assert (scalar.getValueType());
        switch (scalar.getScalarType()) {
            case ScalarType::Internal:
                addToGroupCountMap(sharedGroups, scalar.getGroup());
                break;
            case ScalarType::ThreadLocal:
                if (options == InitializeOptions::DoNotIncludeThreadLocalScalars) continue;
                addToGroupCountMap(threadLocalGroups, scalar.getGroup());
                if (options != InitializeOptions::IncludeAndAutomaticallyAccumulateThreadLocalScalars) continue;
                if (scalar.getAccumulationRule() != Kernel::ThreadLocalScalarAccumulationRule::DoNothing) {
                    assert (mCommonThreadLocalHandle && "no main thread local given?");
                    hasThreadLocalAccum = true;
                }
                break;
            default: break;
        }
    }

    const auto totalSharedGroupCount = computePartialSumOfGroupCounts(sharedGroups, mInputScalars.size());
    computePartialSumOfGroupCounts(threadLocalGroups, 0U);

    BasicBlock * combineToMainThreadLocal = nullptr;

    if (LLVM_UNLIKELY(hasThreadLocalAccum)) {
        combineToMainThreadLocal = b.CreateBasicBlock("combineToMainThreadLocal");
    }

    FixedArray<Value *, 2> indices;
    indices[0] = b.getInt32(0);
    auto enumerate = [&](const Bindings & bindings, const size_t initialIndex) {
        auto index = initialIndex;
        for (const auto & binding : bindings) {
            assert (sharedTy);
            const auto k = index * 2 + 1;
            assert (k < sharedTy->getStructNumElements());
            Type * const actualType = sharedTy->getStructElementType(k);
            assert (&actualType->getContext() == &sharedTy->getContext());
            indices[1] = b.getInt32(k);
            Value * const scalar = b.CreateInBoundsGEP(sharedTy, mSharedHandle, indices);
            addToScalarFieldMap(binding.getName(), scalar, binding.getType(), actualType);
            ++index;
        }
    };

    BasicBlock * combineExit = combineToMainThreadLocal;

    enumerate(mInputScalars, 0U);

    for (const auto & binding : mInternalScalars) {
        Value * scalar = nullptr;
        Type * scalarType = nullptr;

        switch (binding.getScalarType()) {
            case ScalarType::Internal:
                assert (mSharedHandle);
                BEGIN_SCOPED_REGION
                auto f = sharedGroups.find(binding.getGroup());
                assert (f != sharedGroups.end());
                const auto index = f->second++;
                const auto k = index * 2 + 1;
                assert (k < sharedTy->getStructNumElements());
                scalarType = sharedTy->getStructElementType(k);
                indices[1] = b.getInt32(k);
                scalar = b.CreateInBoundsGEP(sharedTy, mSharedHandle, indices);
                END_SCOPED_REGION
                break;
            case ScalarType::ThreadLocal:
                if (options == InitializeOptions::DoNotIncludeThreadLocalScalars) continue;
                assert (mThreadLocalHandle);
                BEGIN_SCOPED_REGION

                auto f = threadLocalGroups.find(binding.getGroup());
                assert (f != threadLocalGroups.end());
                const auto index = f->second++;
                const auto k = index * 2 + 1;
                assert (k < threadLocalTy->getStructNumElements());
                scalarType = threadLocalTy->getStructElementType(k);
                indices[1] = b.getInt32(k);
                scalar = b.CreateInBoundsGEP(threadLocalTy, mThreadLocalHandle, indices);

                if (LLVM_UNLIKELY(options == InitializeOptions::IncludeAndAutomaticallyAccumulateThreadLocalScalars)) {

                    Value * const mainScalar = b.CreateGEP(threadLocalTy, mCommonThreadLocalHandle, indices);

                    using AccumRule = Kernel::ThreadLocalScalarAccumulationRule;

                    if (binding.getAccumulationRule() != AccumRule::DoNothing) {

                        const auto ip = b.saveIP();
                        b.SetInsertPoint(combineExit);

                        if (isa<ArrayType>(scalarType)) {
                            ArrayType * const arrayTy = cast<ArrayType>(scalarType);

                            unsigned depth = 2;
                            for (ArrayType * aTy = arrayTy;;) {
                                Type * const eTy = aTy->getArrayElementType();
                                if (eTy->isArrayTy()) {
                                    aTy = cast<ArrayType>(eTy);
                                    ++depth;
                                } else {
                                    assert (eTy->isIntOrIntVectorTy());
                                    break;
                                }
                            }

                            const auto size = depth;

                            ConstantInt * const i32_ZERO = b.getInt32(0);
                            ConstantInt * const i32_ONE = b.getInt32(1);

                            SmallVector<Value *, 4> indices(size);
                            indices[0] = i32_ZERO;


                            std::function<BasicBlock *(unsigned, Type *)> recursiveAccum = [&](const unsigned idx, Type * const elemTy) {
                                assert (idx <= size);
                                assert (indices.size() == size);

                                BasicBlock * const entry = b.GetInsertBlock();

                                if (idx == size) {
                                    Value * const scalarPtr = b.CreateGEP(scalarType, scalar, indices);
                                    const auto align = CBuilder::getAlignOf(DL, elemTy);
                                    Value * const scalarVal = b.CreateAlignedLoad(elemTy, scalarPtr, align);
                                    assert (scalarVal->getType()->isIntOrIntVectorTy());
                                    Value * const mainScalarPtr = b.CreateGEP(scalarType, mainScalar, indices);
                                    Value * mainScalarVal = b.CreateAlignedLoad(elemTy, mainScalarPtr, align);
                                    assert (scalarVal->getType() == mainScalarVal->getType());
                                    switch (binding.getAccumulationRule()) {
                                        case AccumRule::Sum:
                                            mainScalarVal = b.CreateAdd(scalarVal, mainScalarVal, "sum");
                                            break;
                                        default: llvm_unreachable("unexpected thread-local scalar accumulation rule");
                                    }
                                    b.CreateStore(mainScalarVal, mainScalarPtr);
                                    return entry;
                                } else {

                                    BasicBlock * const loop = b.CreateBasicBlock();
                                    b.CreateBr(loop);

                                    b.SetInsertPoint(loop);
                                    PHINode * const idxPhi = b.CreatePHI(b.getInt32Ty(), 2);
                                    idxPhi->addIncoming(i32_ZERO, entry);
                                    assert (idx < indices.size());
                                    indices[idx] = idxPhi;

                                    BasicBlock * const loopExit =
                                        recursiveAccum(idx + 1U, cast<ArrayType>(elemTy)->getArrayElementType());

                                    BasicBlock * const exit = b.CreateBasicBlock();
                                    Value * const nextIdx = b.CreateAdd(idxPhi, i32_ONE);
                                    idxPhi->addIncoming(nextIdx, loopExit);

                                    const auto m = cast<ArrayType>(elemTy)->getNumElements();
                                    if (LLVM_UNLIKELY(m == 0)) {
                                        report_fatal_error(Twine(getName()) + ": cannot automatically accumulate a 0-element scalar");
                                    }

                                    b.CreateCondBr(b.CreateICmpNE(nextIdx, b.getInt32(m)), loop, exit);

                                    b.SetInsertPoint(exit);
                                    return exit;
                                }
                            };

                            combineExit = recursiveAccum(1, arrayTy);
                        } else {
                            Value * const scalarVal = b.CreateLoad(scalarType, scalar);
                            Value * mainScalarVal = b.CreateLoad(scalarType, mainScalar);
                            switch (binding.getAccumulationRule()) {
                                case Kernel::ThreadLocalScalarAccumulationRule::Sum:
                                    mainScalarVal = b.CreateAdd(scalarVal, mainScalarVal);
                                    break;
                                default: llvm_unreachable("unexpected thread-local scalar accumulation rule");
                            }
                            b.CreateStore(mainScalarVal, mainScalar);
                        }
                        b.restoreIP(ip);
                    }



                }
                END_SCOPED_REGION
                break;
            case ScalarType::NonPersistent:
                BEGIN_SCOPED_REGION
                scalarType = CBuilder::convertTypeToLLVMContext(b.getContext(), binding.getValueType());
                scalar = b.CreateAlloca(scalarType);
                const auto align = DL.getABITypeAlign(scalarType);
                cast<AllocaInst>(scalar)->setAlignment(align);
                b.CreateAlignedStore(Constant::getNullValue(scalarType), cast<AllocaInst>(scalar), align.value());
                END_SCOPED_REGION
                break;
            default: llvm_unreachable("I/O scalars cannot be internal");
        }

        assert (scalar);

        addToScalarFieldMap(binding.getName(), scalar, binding.getValueType(), scalarType);
    }

    enumerate(mOutputScalars, totalSharedGroupCount);

    // finally add any aliases
    for (const auto & alias : mScalarAliasMap) {
        const auto f = mScalarFieldMap.find(alias.second);
        if (f != mScalarFieldMap.end()) {
            addToScalarFieldMap(alias.first, f->second.first, f->second.second, f->second.second);
        }
    }

    if (LLVM_UNLIKELY(hasThreadLocalAccum)) {
        BasicBlock * const exit = b.CreateBasicBlock("afterThreadLocalAccumulation");
        Value * const cond = b.CreateICmpEQ(mThreadLocalHandle, mCommonThreadLocalHandle);
        b.CreateCondBr(cond, exit, combineToMainThreadLocal);
        b.SetInsertPoint(combineExit);
        b.CreateBr(exit);
        b.SetInsertPoint(exit);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addAlias
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::addAlias(llvm::StringRef alias, llvm::StringRef scalarName) {
    mScalarAliasMap.emplace_back(alias, scalarName);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeBindingMap
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::initializeIOBindingMap() {

    auto enumerate = [&](const Bindings & bindings, const BindingType type) {
        const auto n = bindings.size();
        for (unsigned i = 0; i < n; ++i) {
            const auto & binding = bindings[i];
            mBindingMap.insert(std::make_pair(binding.getName(), BindingMapEntry{type, i}));
        }
    };

    enumerate(mInputScalars, BindingType::ScalarInput);
    enumerate(mOutputScalars, BindingType::ScalarOutput);
    enumerate(mInputStreamSets, BindingType::StreamInput);
    enumerate(mOutputStreamSets, BindingType::StreamOutput);

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeOwnedBufferHandles
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::initializeOwnedBufferHandles(KernelBuilder & b, const InitializeOptions /* options */, Value * const expectedNumOfStrides) {
    const auto numOfOutputs = getNumOfStreamOutputs();
    for (unsigned i = 0; i < numOfOutputs; i++) {
        const Binding & output = mOutputStreamSets[i];
        const auto isLocal = Kernel::isLocalBuffer(output);
        if (LLVM_UNLIKELY(isLocal.any())) {
            auto handle = getScalarFieldPtr(b, output.getName() + BUFFER_HANDLE_SUFFIX);
            const auto & buffer = mStreamSetOutputBuffers[i]; assert (buffer.get());
            buffer->setHandle(handle.first);
//            assert (isLocal.isManaged() == Kernel::isManagedBuffer(output));
//            assert (buffer->isDynamic() || !isLocal.isManaged());
            if (LLVM_UNLIKELY(isLocal.isManaged() && expectedNumOfStrides)) {
                assert (buffer->isDynamic());
                Rational R{mTarget->getStride(), b.getBitBlockWidth()};
                const auto & ub = output.getRate().getUpperBound();
                if (ub.numerator() > 0) {
                    R *= ub;
                }
                Value * const bufferScale = b.CreateCeilUMulRational(expectedNumOfStrides, R);
                buffer->allocateBuffer(b, bufferScale, mReportExpansionCallback, mPipelineHandle, b.getSize(i));
            }
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getBinding
 ** ------------------------------------------------------------------------------------------------------------- */
const BindingMapEntry & KernelCompiler::getBinding(const BindingType type, const llvm::StringRef name) const {

    const auto f = mBindingMap.find(name);
    if (f != mBindingMap.end()) {
        const BindingMapEntry & entry = f->second;
        assert (entry.Type == type);
        return entry;
    }

    SmallVector<char, 256> tmp;
    raw_svector_ostream out(tmp);
    out << "Kernel " << getName() << " does not contain an ";
    switch (type) {
        case BindingType::ScalarInput:
        case BindingType::StreamInput:
            out << "input"; break;
        case BindingType::ScalarOutput:
        case BindingType::StreamOutput:
            out << "output"; break;
    }
    out << ' ';
    switch (type) {
        case BindingType::ScalarInput:
        case BindingType::ScalarOutput:
            out << "scalar"; break;
        case BindingType::StreamInput:
        case BindingType::StreamOutput:
            out << "streamset"; break;
    }
    out << " named \"" << name << "\"\n"
           "Currently contains:";


    auto listAvailableBindings = [&](const Bindings & bindings) {
        if (LLVM_UNLIKELY(bindings.empty())) {
            out << "<no bindings>";
        } else {
            char joiner = ' ';
            for (const auto & binding : bindings) {
                out << joiner << binding.getName();
                joiner = ',';
            }
        }
        out << '\n';
    };

    switch (type) {
        case BindingType::ScalarInput:
            listAvailableBindings(mInputScalars); break;
        case BindingType::ScalarOutput:
            listAvailableBindings(mOutputScalars); break;
        case BindingType::StreamInput:
            listAvailableBindings(mInputStreamSets); break;
        case BindingType::StreamOutput:
            listAvailableBindings(mOutputStreamSets); break;
    }

    report_fatal_error(Twine(out.str()));
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getStreamPort
 ** ------------------------------------------------------------------------------------------------------------- */
StreamSetPort KernelCompiler::getStreamPort(const StringRef name) const {

    // NOTE: temporary refactoring step to limit changes outside of the kernel class

    static_assert(static_cast<unsigned>(BindingType::StreamInput) == static_cast<unsigned>(PortType::Input), "");
    static_assert(static_cast<unsigned>(BindingType::StreamOutput) == static_cast<unsigned>(PortType::Output), "");

    const auto f = mBindingMap.find(name);
    if (LLVM_LIKELY(f != mBindingMap.end())) {

        const BindingMapEntry & entry = f->second;
        switch (entry.Type) {
            case BindingType::StreamInput:
            case BindingType::StreamOutput:
                return StreamSetPort(static_cast<PortType>(entry.Type), entry.Index);
            default: break;
        }
    }

    SmallVector<char, 256> tmp;
    raw_svector_ostream out(tmp);
    out << "Kernel " << getName() << " does not contain a streamset named: \"" << name << "\"";
    report_fatal_error(Twine(out.str()));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getScalarFieldPtr
 ** ------------------------------------------------------------------------------------------------------------- */
KernelCompiler::ScalarRef KernelCompiler::getScalarFieldPtr(KernelBuilder & b, const StringRef name) const {
    if (LLVM_UNLIKELY(mScalarFieldMap.empty())) {
        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << "Scalar map for " << getName() << " was not initialized prior to calling getScalarFieldPtr";
        assert (false);
        report_fatal_error(Twine(out.str()));
    } else {
        const auto f = mScalarFieldMap.find(name);
        if (LLVM_UNLIKELY(f == mScalarFieldMap.end())) {
            #ifdef NDEBUG
            SmallVector<char, 1024> tmp;
            raw_svector_ostream out(tmp);
            #else
            auto & out = errs();
            #endif
            out << "Scalar map for " << getName() << " does not contain " << name << "\n\n"
                "Currently contains:";
            char spacer = ' ';
            for (const auto & entry : mScalarFieldMap) {
                out << spacer << entry.getKey();
                spacer = ',';
            }
            #ifdef NDEBUG
            report_fatal_error(Twine(out.str()));
            #else
            out << "\n";
            assert (false);
            #endif
        }
        ScalarRef result = f->second;
        assert (isFromCurrentFunction(b, result.first, false));
        return result;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getScalarFieldPtr
 ** ------------------------------------------------------------------------------------------------------------- */
KernelCompiler::ScalarRef KernelCompiler::getScalarFieldPtr(KernelBuilder & b, Value * const handle, const ScalarType type, const StringRef name) const {

    // TODO: if we have a scalar map we could extract the indices from the gep even if its not in the same function?

    assert (type == ScalarType::Internal || type == ScalarType::ThreadLocal);

    if (LLVM_UNLIKELY(mScalarFieldMap.empty())) {

        flat_map<size_t, size_t> groups;

        for (const auto & scalar : mInternalScalars) {
            assert (scalar.getValueType());
            if (type == scalar.getScalarType()) {
                addToGroupCountMap(groups, scalar.getGroup());
            }
        }

        computePartialSumOfGroupCounts(groups, mInputScalars.size());


        for (const auto & binding : mInternalScalars) {

            if (type == binding.getScalarType()) {

                auto f = groups.find(binding.getGroup());
                assert (f != groups.end());
                const auto index = f->second++;

                if (name.compare(binding.getName()) == 0) {
                    StructType * stateTy = nullptr;
                    if (type == ScalarType::Internal) {
                        stateTy = mTarget->getSharedStateType(b.getContext()); assert(stateTy);
                    } else {
                        stateTy = mTarget->getThreadLocalStateType(b.getContext()); assert(stateTy);
                    }

                    const auto k = index * 2 + 1;

                    FixedArray<Value *, 2> indices;
                    indices[0] = b.getInt32(0);
                    indices[1] = b.getInt32(k);
                    assert (k < stateTy->getStructNumElements());

                    assert (isFromCurrentFunction(b, handle, false));
                    Value * ptr = b.CreateGEP(stateTy, handle, indices); assert (ptr);
                    assert (stateTy->getStructElementType(k) == binding.getValueType());
                    return ScalarRef{ptr, binding.getValueType()};
                }
            }
        }
        return ScalarRef{nullptr, nullptr};

    } else {

        auto f = mScalarFieldMap.find(name);
        if (LLVM_UNLIKELY(f == mScalarFieldMap.end())) {
            return ScalarRef{nullptr, nullptr};
        }
        const auto & ref = f->second;

        GetElementPtrInst * const gep = cast<GetElementPtrInst>(ref.first);
        assert (gep->getNumIndices() == 2);
        assert (gep->hasAllConstantIndices());

        FixedArray<Value *, 2> indices;
        indices[0] = gep->getOperand(1);
        indices[1] = gep->getOperand(2);
        Value * ptr = b.CreateGEP(gep->getSourceElementType(), handle, indices); assert (ptr);

        return ScalarRef{ptr, cast<Type>(ref.second)};

    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getScalarValuePtr
 ** ------------------------------------------------------------------------------------------------------------- */
bool KernelCompiler::hasScalarField(const llvm::StringRef name) const {
    return mScalarFieldMap.count(name) != 0;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLowerBound
 ** ------------------------------------------------------------------------------------------------------------- */
Rational KernelCompiler::getLowerBound(const Binding & binding) const {
    const ProcessingRate & rate = binding.getRate();
    if (rate.hasReference()) {
        return rate.getLowerBound() * getLowerBound(getStreamBinding(rate.getReference()));
    } else {
        return rate.getLowerBound();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getUpperBound
 ** ------------------------------------------------------------------------------------------------------------- */
Rational KernelCompiler::getUpperBound(const Binding & binding) const {
    const ProcessingRate & rate = binding.getRate();
    if (rate.hasReference()) {
        return rate.getUpperBound() * getUpperBound(getStreamBinding(rate.getReference()));
    } else {
        return rate.getUpperBound();
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief requiresOverflow
 ** ------------------------------------------------------------------------------------------------------------- */
bool KernelCompiler::requiresOverflow(const Binding & binding) const {
    const ProcessingRate & rate = binding.getRate();
    if (rate.isFixed() || binding.hasAttribute(AttrId::BlockSize)) {
        return false;
    } else if (rate.isRelative()) {
        return requiresOverflow(getStreamBinding(rate.getReference()));
    } else {
        return true;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief clearInternalStateAfterCodeGen
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::clearInternalStateAfterCodeGen() {
    // TODO: this function is more of a sanity check to ensure we don't have a pointer
    // to an out-of-scope LLVM-value. It should be possible to remove it.
    mScalarFieldMap.clear();
    mSharedHandle = nullptr;
    mThreadLocalHandle = nullptr;
    mCommonThreadLocalHandle = nullptr;
    mExternalSegNo = nullptr;
    mCurrentMethod = nullptr;
    mEntryPoint = nullptr;
    mIsFinal = nullptr;
    mNumOfStrides = nullptr;
    mTerminationSignalPtr = nullptr;
    const auto numOfInputs = getNumOfStreamInputs();
    reset(mInputIsClosed, numOfInputs);
    reset(mProcessedInputItemPtr, numOfInputs);
    reset(mAccessibleInputItems, numOfInputs);
    reset(mAvailableInputItems, numOfInputs);
    const auto numOfOutputs = getNumOfStreamOutputs();
    reset(mProducedOutputItemPtr, numOfOutputs);
    reset(mInitiallyProducedOutputItems, numOfOutputs);
    reset(mWritableOutputItems, numOfOutputs);
    reset(mConsumedOutputItems, numOfOutputs);
    reset(mUpdatableOutputBaseVirtualAddressPtr, numOfOutputs);
    reset(mUpdatableOutputCapacityPtr, numOfOutputs);
    for (const auto & buffer : mStreamSetInputBuffers) {
        buffer->setHandle(nullptr);
    }
    for (const auto & buffer : mStreamSetOutputBuffers) {
        buffer->setHandle(nullptr);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief registerIllustrator
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::registerIllustrator(KernelBuilder & b,
                                         llvm::Constant * kernelName, llvm::Constant * streamName,
                                         const size_t rows, const size_t cols, const size_t itemWidth, const MemoryOrdering ordering,
                                         IllustratorTypeId illustratorTypeId, const char replacement0, const char replacement1,
                                         const ArrayRef<size_t> loopIds) const {


    auto init = mTarget->getInitializeFunction(b, true, GlobalValue::ExternalLinkage);
    assert (init);
    auto arg = init->arg_begin();
    auto nextArg = [&]() {
        assert (arg != init->arg_end());
        Value * const v = &*arg;
        assert (&v->getContext() == &b.getContext());
        assert (&v->getType()->getContext() == &b.getContext());
        std::advance(arg, 1);
        return v;
    };
    assert (mTarget->getSharedStateType());
    Value * handle = nextArg();
    Instruction * ret = nullptr;
    for (auto & bb : *init) {
        assert (bb.getTerminator());
        if (isa<ReturnInst>(bb.getTerminator())) {
            ret = bb.getTerminator();
            break;
        }
    }
    assert (ret && "no return statement found in initialize kernel function?");
    Value * illustratorObject = nullptr;
    for (const auto & binding : mInputScalars) {
        Value * inputArg = nextArg();
        if (binding.getName() == KERNEL_ILLUSTRATOR_CALLBACK_OBJECT) {
            illustratorObject = inputArg;
            break;
        }
    }
    assert (illustratorObject && "no illustrator object found?");

    auto ip = b.saveIP();

    b.SetInsertPoint(ret->getPrevNode());

    registerIllustrator(b, illustratorObject, kernelName, streamName, handle, rows, cols, itemWidth, ordering, illustratorTypeId, replacement0, replacement1, loopIds);

    b.restoreIP(ip);
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief registerIllustrator
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::registerIllustrator(KernelBuilder & b,
                                         Value * illustratorObject,
                                         Constant * kernelName, Constant * streamName, Value * handle,
                                         const size_t rows, const size_t cols, const size_t itemWidth, const MemoryOrdering ordering,
                                         IllustratorTypeId illustratorTypeId,
                                         const char replacement0, const char replacement1,
                                         const ArrayRef<size_t> loopIds) const {

    assert (isFromCurrentFunction(b, illustratorObject, false));
    assert (isFromCurrentFunction(b, handle, false));

    Function * regFunc = b.getModule()->getFunction(KERNEL_REGISTER_ILLUSTRATOR_CALLBACK); assert (regFunc);
    FixedArray<Value *, 12> args;
    args[0] = illustratorObject;
    args[1] = kernelName;
    args[2] = streamName;
    args[3] = handle;
    args[4] = b.getSize(rows);
    args[5] = b.getSize(cols);
    args[6] = b.getSize(itemWidth);
    args[7] = b.getInt8((unsigned)ordering);
    args[8] = b.getInt8((unsigned)illustratorTypeId);
    args[9] = b.getInt8(replacement0);
    args[10] = b.getInt8(replacement1);

    IntegerType * const sizeTy = b.getSizeTy();
    Constant * loopIdConstant = nullptr;
    if (loopIds.empty()) {
        loopIdConstant = ConstantPointerNull::get(PointerType::getUnqual(b.getContext()));
    } else {
        const auto n = loopIds.size();
        SmallVector<Constant *, 8> ids(n + 1);
        for (size_t i = 0; i < n; ++i) {
            assert (loopIds[i] != 0);
            ids[i] = b.getSize(loopIds[i]);
        }
        ids[n] = b.getSize(0);
        ArrayType * arTy = ArrayType::get(sizeTy, n + 1);
        Constant * ar = ConstantArray::get(arTy, ids);
        GlobalVariable * const gv = new GlobalVariable(*b.getModule(), arTy, true, GlobalValue::ExternalLinkage, ar);
        loopIdConstant = gv;
    }
    args[11] = loopIdConstant;
    b.CreateCall(regFunc->getFunctionType(), regFunc, args);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief captureStreamData
 ** ------------------------------------------------------------------------------------------------------------- */
void KernelCompiler::captureStreamData(KernelBuilder & b, Constant * kernelName, Constant * streamName, Value * handle,
                                       Value * strideNum,
                                       Type * type, const MemoryOrdering ordering,
                                       Value * streamData, Value * from, Value * to)  const {

    FixedArray<Value *, 9> args;
    args[0] = b.getScalarField(KERNEL_ILLUSTRATOR_CALLBACK_OBJECT);
    args[1] = kernelName;
    args[2] = streamName;
    args[3] = handle;
    args[4] = strideNum;
    args[5] = streamData;
    args[6] = from;
    args[7] = to;
    args[8] = b.getSize(b.getBitBlockWidth());

    Function * func = b.getModule()->getFunction(KERNEL_ILLUSTRATOR_CAPTURE_CALLBACK); assert (func);
    b.CreateCall(func->getFunctionType(), func, args);

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
KernelCompiler::KernelCompiler(not_null<Kernel *> kernel) noexcept
: mTarget(kernel)
, mInputStreamSets(kernel->mInputStreamSets)
, mOutputStreamSets(kernel->mOutputStreamSets)
, mInputScalars(kernel->mInputScalars)
, mOutputScalars(kernel->mOutputScalars)
, mInternalScalars(kernel->mInternalScalars) {
    initializeIOBindingMap();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief destructor
 ** ------------------------------------------------------------------------------------------------------------- */
KernelCompiler::~KernelCompiler() {

}

}

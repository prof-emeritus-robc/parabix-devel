/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/core/kernel.h>
#include <kernel/core/kernel_compiler.h>
#include <kernel/pipeline/driver/driver.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <boost/container/flat_set.hpp>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Format.h>
#include <llvm/Analysis/ConstantFolding.h>
#if BOOST_VERSION < 106600
#include <boost/uuid/sha1.hpp>
#else
#include <boost/uuid/detail/sha1.hpp>
#endif
#include <boost/intrusive/detail/math.hpp>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

using namespace llvm;
using namespace boost;
using boost::container::flat_set;
using IDISA::FixedVectorType;


using boost::intrusive::detail::floor_log2;
using boost::intrusive::detail::is_pow2;
using boost::uuids::detail::sha1;

namespace kernel {

using AttrId = Attribute::KindId;
using Rational = ProcessingRate::Rational;
using RateId = ProcessingRate::KindId;
using StreamSetPort = Kernel::StreamSetPort;
using PortType = Kernel::PortType;

constexpr static auto INITIALIZE_SUFFIX = "_Initialize";
constexpr static auto INITIALIZE_THREAD_LOCAL_SUFFIX = "_InitializeThreadLocal";
constexpr static auto GET_EXPECTED_OUTPUT_SIZE_SUFFIX = "_GetExpectedOutputSize";
constexpr static auto ALLOCATE_SHARED_INTERNAL_STREAMSETS_SUFFIX = "_AllocateSharedInternalStreamSets";
constexpr static auto ALLOCATE_THREAD_LOCAL_INTERNAL_STREAMSETS_SUFFIX = "_AllocateThreadLocalInternalStreamSets";
constexpr static auto DO_SEGMENT_SUFFIX = "_DoSegment";
constexpr static auto FINALIZE_THREAD_LOCAL_SUFFIX = "_FinalizeThreadLocal";
constexpr static auto FINALIZE_SUFFIX = "_Finalize";

constexpr static auto SIGNATURE = "signature";

#ifdef ENABLE_PAPI
const static auto PAPI_INITIALIZE_EVENTSET = "_PAPIInitializeEventSet";
#endif

const static auto SHARED_SUFFIX = "_shared_state";
const static auto THREAD_LOCAL_SUFFIX = "_thread_local";
constexpr static auto STATE_TYPE_METADATA_SUFFIX = "_state_types";

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(21, 0, 0)
#define addNoCaptureAttr(arg) \
            arg->addAttr(llvm::Attribute::get(b.getContext(), llvm::Attribute::NoCapture))
#else
#define addNoCaptureAttr(arg) \
            arg->addAttr(llvm::Attribute::getWithCaptureInfo(b.getContext(), llvm::CaptureInfo::none()))
#endif

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief nullIfEmpty
 ** ------------------------------------------------------------------------------------------------------------- */
inline StructType * nullIfEmpty(StructType * type) {
    return (type && type->isEmptyTy()) ? nullptr : type;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief concat
 ** ------------------------------------------------------------------------------------------------------------- */
inline StringRef concat(StringRef A, StringRef B, SmallVector<char, 256> & tmp) {
    Twine C = A + B;
    tmp.clear();
    C.toVector(tmp);
    return StringRef(tmp.data(), tmp.size());
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getTypeByName
 ** ------------------------------------------------------------------------------------------------------------- */
inline StructType * getTypeByName(Module * const m, StringRef name) {
    return StructType::getTypeByName(m->getContext(), name);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief isLocalBuffer
 ** ------------------------------------------------------------------------------------------------------------- */
/* static */ Kernel::LocalBufferFlagSet Kernel::isLocalBuffer(const Binding & output) {
    // NOTE: if this function is modified, fix the PipelineCompiler to match it.
    LocalBufferFlagSet fs;
    for (const auto & attr : output.getAttributes()) {
        switch (attr.getKind()) {
            case AttrId::SharedManagedBuffer:
                fs.Flags |= LocalBufferFlagSet::LBF_Shared;
                break;
            case AttrId::ManagedBuffer:
                fs.Flags |= LocalBufferFlagSet::LBF_Managed;
                break;
            case AttrId::ReturnedBuffer:
                fs.Flags |= LocalBufferFlagSet::LBF_Returned;
                break;
            default: break;
        }
    }
    if (LLVM_UNLIKELY(output.getRate().isUnknown())) {
        fs.Flags |= LocalBufferFlagSet::LBF_Managed;
    }
    assert (fs.isManaged() == isManagedBuffer(output));
    return fs;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief isManagedBuffer
 ** ------------------------------------------------------------------------------------------------------------- */
/* static */ bool Kernel::isManagedBuffer(const Binding & output) {
    for (const auto & attr : output.getAttributes()) {
        switch (attr.getKind()) {
            case AttrId::ManagedBuffer:
                return true;
            default: break;
        }
    }
    return output.getRate().isUnknown();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief requiresExplicitPartialFinalStride
 ** ------------------------------------------------------------------------------------------------------------- */
bool Kernel::requiresExplicitPartialFinalStride() const {
    if (LLVM_UNLIKELY(hasAttribute(AttrId::InternallySynchronized))) {
        return false;
    }
    const auto m = getNumOfStreamOutputs();
    for (unsigned i = 0; i < m; ++i) {
        const Binding & output = getOutputStreamSetBinding(i);
        for (const Attribute & attr : output.getAttributes()) {
            switch (attr.getKind()) {
                case AttrId::Add:
                case AttrId::Delayed:
                    if (LLVM_LIKELY(attr.amount() > 0)) {
                        return true;
                    }
                case AttrId::Deferred:
                    return true;
                default: break;
            }
        }
    }
    return false;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief hasFixedRateIO
 ** ------------------------------------------------------------------------------------------------------------- */
bool Kernel::hasFixedRateIO() const {
    for (const auto & input : mInputStreamSets) {
        const ProcessingRate & rate = input.getRate();
        if (rate.isFixed()) {
            return true;
        }
    }
    for (const auto & output : mOutputStreamSets) {
        const ProcessingRate & rate = output.getRate();
        if (rate.isFixed()) {
            return true;
        }
    }
    return false;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief canSetTerminateSignal
 ** ------------------------------------------------------------------------------------------------------------ */
bool Kernel::canSetTerminateSignal() const {
    for (const Attribute & attr : getAttributes()) {
        switch (attr.getKind()) {
            case AttrId::MayFatallyTerminate:
            case AttrId::CanTerminateEarly:
            case AttrId::MustExplicitlyTerminate:
                return true;
            default: continue;
        }
    }
    return false;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief instantiateKernelCompiler
 ** ------------------------------------------------------------------------------------------------------------- */
std::unique_ptr<KernelCompiler> Kernel::instantiateKernelCompiler(KernelBuilder & /* b */) {
    return std::make_unique<KernelCompiler>(const_cast<Kernel *>(this));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makeCacheName
 ** ------------------------------------------------------------------------------------------------------------- */
std::string Kernel::makeCacheName(KernelBuilder & b) {
    std::string cacheName;
    raw_string_ostream out(cacheName);
    out << getName() << '_' << b.getBuilderUniqueName();
#if 0
    auto appendStreamSetType = [&](const char code, const Bindings & bindings) {
        for (const auto & binding : bindings) {
            const auto r = static_cast<const StreamSet *>(binding.getRelationship();
            out << '_' << code << r->getNumElements() << 'x' << r->getFieldWidth();
        }
    };
    appendStreamSetType('I', mInputStreamSets);
    appendStreamSetType('O', mOutputStreamSets);
    auto appendScalarType = [&](const char code, const Bindings & bindings) {
        for (const auto & binding : bindings) {
            const auto r = static_cast<const Scalar *>(binding.getRelationship();
            out << '_' << code << r->getFieldWidth();
        }
    };
    appendScalarType('I', mInputScalars);
    appendScalarType('O', mOutputScalars);

    for (const auto & internal : mInternalScalars) {
        out << 'X' << (unsigned)internal.getScalarType()
            << '.' << internal.getGroup();
        internal.getType().print(out);
    }
#endif
    out.flush();
    return cacheName;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief makeEmptyModule
 ** ------------------------------------------------------------------------------------------------------------- */
Module * Kernel::makeEmptyModule(KernelBuilder & b) {
    // NOTE: this assumes that the KernelBuilder used to make the module has the same config as the
    // one that generates it later. Would be better if this didn't but that will require redesigning
    // the compilation and object cache interface.
    auto & C = b.getContext();
    Module * const m = new Module(makeCacheName(b), C);
    if (LLVM_UNLIKELY(hasSignature() && isCachable())) {
         NamedMDNode * const md = m->getOrInsertNamedMetadata(SIGNATURE);
         assert (md->getNumOperands() == 0);
         const auto signature = getSignature();
         MDString * const sig = MDString::get(C, signature);
         md->addOperand(MDNode::get(m->getContext(), {sig}));
    }
    return m;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief declareStateTypes
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::declareStateTypes(KernelBuilder & b) {
    auto oc = b.getCompiler();
    assert (mCompiler.get() == nullptr);
    mCompiler = instantiateKernelCompiler(b);
    b.setCompiler(mCompiler.get());
    mCompiler->constructStateTypes(b);
    b.setCompiler(oc);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateKernel
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::generateKernel(KernelBuilder & b, llvm::TargetMachine * TM, llvm::GlobalValue::LinkageTypes linkageType) {
    auto oc = b.getCompiler();
    assert (mCompiler);
    b.setCompiler(mCompiler.get());
    mCompiler->generateKernel(b, TM, linkageType);
    b.setCompiler(oc);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief loadCachedKernel
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::loadCachedKernel(const Module * m) {

    SmallVector<char, 256> tmp;
//    auto strShared = concat(getName(), SHARED_SUFFIX, tmp);
//    mSharedStateType = StructType::getTypeByName(m->getContext(), strShared);
//    auto strThreadLocal= concat(getName(), THREAD_LOCAL_SUFFIX, tmp);
//    mThreadLocalStateType = StructType::getTypeByName(m->getContext(), strThreadLocal);

    auto structTypeMetadata = m->getNamedMetadata(getName() + STATE_TYPE_METADATA_SUFFIX);
    assert (structTypeMetadata);
    assert (structTypeMetadata->getNumOperands() == 1);
    MDNode * structTypes = structTypeMetadata->getOperand(0);
    assert (structTypes->getNumOperands() == 2);
    Type * shType = cast<ConstantAsMetadata>(structTypes->getOperand(0))->getType(); assert (shType);
    mSharedStateType = nullIfEmpty(cast<StructType>(shType));
    assert (mSharedStateType == nullptr ||
            (!mSharedStateType->isOpaque() && &mSharedStateType->getContext() == &m->getContext()));
    Type * tlType = cast<ConstantAsMetadata>(structTypes->getOperand(1))->getType(); assert (tlType);
    mThreadLocalStateType = nullIfEmpty(cast<StructType>(tlType));
    assert (mThreadLocalStateType == nullptr ||
            (!mThreadLocalStateType->isOpaque() && &mThreadLocalStateType->getContext() == &m->getContext()));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief linkExternalMethods
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::linkExternalMethods(KernelBuilder & b) {
    Module * const m = b.getModule(); assert (m);
    if (mFlags & Kernel::KernelFlags::HasInternallyManagedStreamSet) {
        StreamSetBuffer::linkFunctions(b);
    }
    b.LinkAllNecessaryExternalFunctions();
    if (LLVM_UNLIKELY(getKernelFlags() & Kernel::KernelFlags::RequiresIllustratorObject)) {
        PointerType * voidPtrTy = PointerType::getUnqual(b.getContext());
        IntegerType * int8Ty = b.getInt8Ty();
        PointerType * int8PtrTy = b.getInt8PtrTy();
        IntegerType * sizeTy = b.getSizeTy();
        Type * const voidTy = b.getVoidTy();

        BEGIN_SCOPED_REGION
        FixedArray<Type *, 12> params;
        params[0] = voidPtrTy;
        params[1] = int8PtrTy;
        params[2] = int8PtrTy;
        params[3] = voidPtrTy;
        params[4] = sizeTy; // num of outer rows/columns
        params[5] = sizeTy; // inner data size
        params[6] = sizeTy; // item width
        params[7] = int8Ty; // memory ordering
        params[8] = int8Ty; // illustrator type
        params[9] = int8Ty; // replacement 0
        params[10] = int8Ty; // replacement 1
        params[11] = PointerType::getUnqual(b.getContext()); // loopId array
        FunctionType * regFunc = FunctionType::get(voidTy, params, false);
        b.LinkFunction(KERNEL_REGISTER_ILLUSTRATOR_CALLBACK, regFunc, (void*)&illustratorRegisterCapturedData);
        END_SCOPED_REGION

        BEGIN_SCOPED_REGION
        FixedArray<Type *, 9> params;
        params[0] = voidPtrTy;
        params[1] = int8PtrTy;
        params[2] = int8PtrTy;
        params[3] = voidPtrTy;
        params[4] = sizeTy;
        params[5] = voidPtrTy;
        params[6] = sizeTy;
        params[7] = sizeTy;
        params[8] = sizeTy;
        FunctionType * func = FunctionType::get(voidTy, params, false);
        b.LinkFunction(KERNEL_ILLUSTRATOR_CAPTURE_CALLBACK, func, (void*)&illustratorCaptureStreamData);
        END_SCOPED_REGION
    }
    for (const auto & link : mPendingFunctionLinks) {
        Type * const funcTy = CBuilder::convertTypeToLLVMContext(b.getContext(), link.FuncType);
        b.LinkFunction(link.UnmanagedName, cast<FunctionType>(funcTy), link.FuncPointer);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructStateTypes
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::constructStateTypes(KernelBuilder & b) {
    Module * const m = b.getModule();

    SmallVector<char, 256> tmpMeta;
    auto strMeta = concat(getName(), STATE_TYPE_METADATA_SUFFIX, tmpMeta);
    NamedMDNode * const structTypeMetadata = m->getOrInsertNamedMetadata(strMeta);
    assert (structTypeMetadata->getNumOperands() == 0);

    StructType * sharedStateType = nullptr;
    StructType * threadLocalStateType = nullptr;

    auto & C = m->getContext();
    assert (&b.getContext() == &C);

    SmallVector<char, 256> tmpShared;
    auto strShared = concat(getName(), SHARED_SUFFIX, tmpShared);
    sharedStateType = StructType::getTypeByName(C, strShared);
    assert (sharedStateType == nullptr || &sharedStateType->getContext() == &b.getContext());

    SmallVector<char, 256> tmpThreadLocal;
    auto strThreadLocal = concat(getName(), THREAD_LOCAL_SUFFIX, tmpThreadLocal);
    threadLocalStateType = StructType::getTypeByName(C, strThreadLocal);
    assert (threadLocalStateType == nullptr || &threadLocalStateType->getContext() == &b.getContext());

    auto isOpaqueType = [&](StructType * const st) -> bool {
        return st ? st->isOpaque() : false;
    };

    if (LLVM_LIKELY((sharedStateType == nullptr && threadLocalStateType == nullptr)
                    || isOpaqueType(sharedStateType)
                    || isOpaqueType(threadLocalStateType))) {

        flat_set<unsigned> sharedGroups;
        flat_set<unsigned> threadLocalGroups;

        for (const auto & scalar : mInternalScalars) {
            assert (scalar.getValueType());
            switch (scalar.getScalarType()) {
                case ScalarType::Internal:
                    sharedGroups.insert(scalar.getGroup());
                    break;
                case ScalarType::ThreadLocal:
                    threadLocalGroups.insert(scalar.getGroup());
                    break;
                default: break;
            }
        }

        using TypesVec = std::vector<Type *>;

        using VecOfTypes = std::vector<TypesVec>;

        VecOfTypes shared(sharedGroups.size() + 2);
        VecOfTypes threadLocal(threadLocalGroups.size());


        auto addScalar = [&](VecOfTypes & S, const unsigned group, Type * const type) {
            assert (group < S.size());
            S[group].push_back(CBuilder::convertTypeToLLVMContext(C, type));
        };

        size_t sharedGroupCount = 0;
        size_t threadLocalGroupCount = 0;

        for (const auto & scalar : mInputScalars) {
            addScalar(shared, 0, scalar.getType());
             ++sharedGroupCount;
        }

        for (const auto & scalar : mInternalScalars) {
            assert (scalar.getValueType());

            auto getGroupIndex = [&](const flat_set<unsigned> & groups) {
                const auto f = groups.find(scalar.getGroup());
                assert (f != groups.end());
                return (unsigned)std::distance(groups.begin(), f);
            };

            switch (scalar.getScalarType()) {
                case ScalarType::Internal:
                    addScalar(shared, getGroupIndex(sharedGroups) + 1, scalar.getValueType());
                     ++sharedGroupCount;
                    break;
                case ScalarType::ThreadLocal:
                    addScalar(threadLocal, getGroupIndex(threadLocalGroups), scalar.getValueType());
                    ++threadLocalGroupCount;
                    break;
                default: break;
            }
        }

        assert (shared[sharedGroups.size() + 1].empty());
        for (const auto & scalar : mOutputScalars) {
            addScalar(shared, sharedGroups.size() + 1, scalar.getType());
            ++sharedGroupCount;
        }

        IntegerType * const int8Ty = b.getInt8Ty();

        const uintptr_t cacheAlignment = b.getCacheAlignment();

        auto & dl = m->getDataLayout();

        auto makeStructType = [&](StructType * st, VecOfTypes & structTypeVec, const size_t count,
                                  StringRef name, const bool addGroupCacheLinePadding) -> StructType * {

            if (count == 0) return nullptr;

            const auto n = structTypeVec.size();

            std::vector<Type *> fields(count * 2);

            uintptr_t byteOffset = 0;
            size_t k = 0;

            for (unsigned i = 0; i < n; ++i) {
                const auto & L = structTypeVec[i];
                const auto m = L.size();
                for (size_t j = 0; j != m; ++j) {
                    Type * const type = L[j]; assert(type);
                    assert (&type->getContext() == &b.getContext());
                    uintptr_t align = CBuilder::getAlignOf(dl, type);
                    assert (align > 0);
                    if (j == 0 && addGroupCacheLinePadding) {
                        align = boost::lcm(align, cacheAlignment);
                    }
                    const auto offset = (byteOffset % align);
                    assert (i != 0 || j != 0 || offset == 0);
                    const auto padding = (offset == 0ULL) ? 0ULL : (align - offset);
                    byteOffset += padding + CBuilder::getTypeSize(dl, type);
                    Type * const paddingTy = ArrayType::get(int8Ty, padding);
                    assert (&paddingTy->getContext() == &b.getContext());
                    assert (k < fields.size());
                    fields[k++] = paddingTy;
                    assert (k < fields.size());
                    fields[k++] = type;
                }
            }


            assert (k == fields.size());

            if (LLVM_UNLIKELY(byteOffset == 0)) return nullptr;

            if (st == nullptr) {
                st = StructType::create(C, fields, name, true);
            } else {
                assert (&st->getContext() == &b.getContext());
                assert (st->isOpaque());
                st->setBody(fields, true);
                assert (!st->isOpaque() && st->isPacked());
            }

            #ifndef NDEBUG
            assert (st->getStructNumElements() == k);
            const StructLayout * const sl = dl.getStructLayout(st);
            const auto structTypeSize = CBuilder::getTypeSize(dl, st);
            assert ("expected stuct size does not match type size?" && sl->getSizeInBytes() == structTypeSize);
            assert ("expected stuct size does not match byte offset?" && structTypeSize == byteOffset);
            for (size_t i = 0; i < k; ++i) {
                const auto align = CBuilder::getAlignOf(dl, st->getElementType(i));
                assert ((sl->getElementOffset(i) %  align) == 0);
            }
            #endif

            return st;
        };

        // NOTE: StructType::create always creates a new type even if an identical one exists.
        const auto allowStructPadding = !codegen::DebugOptionIsSet(codegen::DisableCacheAlignedKernelStructs);
        if (sharedStateType == nullptr || sharedStateType->isOpaque()) {
            sharedStateType = makeStructType(sharedStateType, shared, sharedGroupCount, strShared, allowStructPadding);
            assert (nullIfEmpty(sharedStateType) == sharedStateType);
        }
        if (threadLocalStateType == nullptr || threadLocalStateType->isOpaque()) {
            threadLocalStateType = makeStructType(threadLocalStateType, threadLocal, threadLocalGroupCount, strThreadLocal, false);
            assert (nullIfEmpty(threadLocalStateType) == threadLocalStateType);
        }
        if (LLVM_UNLIKELY(InfoOptionIsSet(codegen::PrintKernelSizes))) {
            errs() << "KERNEL: " << mKernelName
                   << " SHARED STATE: " << CBuilder::getTypeSize(dl, sharedStateType) << " bytes"
                      ", THREAD LOCAL STATE: "  << CBuilder::getTypeSize(dl, threadLocalStateType) << " bytes\n";
        }
    }

    auto makeTypeMetadata = [&](StructType * st, StringRef name) -> Metadata * {
        if (st == nullptr) {
            st = StructType::create(b.getContext(), name);
        }
        return ConstantAsMetadata::get(Constant::getNullValue(st));
    };

    FixedArray<Metadata *, 2> stateTypes;
    stateTypes[0] = makeTypeMetadata(sharedStateType, strShared);
    stateTypes[1] = makeTypeMetadata(threadLocalStateType, strThreadLocal);
    structTypeMetadata->addOperand(MDNode::get(m->getContext(), stateTypes));
    assert (structTypeMetadata->getNumOperands() == 1);

    mSharedStateType = sharedStateType;
    mThreadLocalStateType = threadLocalStateType;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addKernelDeclarations
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::addKernelDeclarations(KernelBuilder & b, TargetMachine * TM, GlobalValue::LinkageTypes linkageType) {
    addInitializeDeclaration(b, linkageType);
    if (LLVM_UNLIKELY(mInputStreamSets.empty())) {
        addExpectedOutputSizeDeclaration(b, linkageType);
    }
    if (LLVM_UNLIKELY(allocatesInternalStreamSets())) {
        addAllocateSharedInternalStreamSetsDeclaration(b, linkageType);
    }
    addDoSegmentDeclaration(b, linkageType);
    if (mThreadLocalStateType) {
        addInitializeThreadLocalDeclaration(b, linkageType);
        if (LLVM_UNLIKELY(allocatesInternalStreamSets())) {
            addAllocateThreadLocalInternalStreamSetsDeclaration(b, linkageType);
        }
        addFinalizeThreadLocalDeclaration(b, linkageType);
    }
    addFinalizeDeclaration(b, linkageType);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addSymbols
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::addSymbols(orc::MangleAndInterner & mangler, orc::SymbolLookupSet &symbols) const {
    SmallVector<char, 256> tmp;
    auto addSym = [&](StringRef suffix) {
        auto sym = mangler(concat(getName(), suffix, tmp));
        symbols.add(sym, orc::SymbolLookupFlags::RequiredSymbol);
    };
    addSym(INITIALIZE_SUFFIX);
    addSym(DO_SEGMENT_SUFFIX);
    addSym(FINALIZE_SUFFIX);
    if (LLVM_UNLIKELY(mInputStreamSets.empty())) {
        addSym(GET_EXPECTED_OUTPUT_SIZE_SUFFIX);
    }
    if (LLVM_UNLIKELY(allocatesInternalStreamSets())) {
        addSym(ALLOCATE_SHARED_INTERNAL_STREAMSETS_SUFFIX);
    }
    if (mThreadLocalStateType) {
        addSym(INITIALIZE_THREAD_LOCAL_SUFFIX);
        if (LLVM_UNLIKELY(allocatesInternalStreamSets())) {
            addSym(ALLOCATE_THREAD_LOCAL_INTERNAL_STREAMSETS_SUFFIX);
        }
        addSym(FINALIZE_THREAD_LOCAL_SUFFIX);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInitializeFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getInitializeFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), INITIALIZE_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addInitializeDeclaration(b, linkageType);
    }
    assert ((f == nullptr && !alwayReturnDeclaration) || f->getLinkage() == linkageType);
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addInitializeDeclaration
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addInitializeDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {

    Module * const m = b.getModule();

    SmallVector<char, 256> tmp;
    const auto funcName = concat(getName(), INITIALIZE_SUFFIX, tmp);
    Function * initFunc = m->getFunction(funcName);
    if (LLVM_LIKELY(initFunc == nullptr)) {
        InitArgTypes params;
        const auto ea = codegen::DebugOptionIsSet(codegen::EnableAsserts);

        IntegerType * const sizeTy = b.getSizeTy();
        if (LLVM_UNLIKELY(ea)) {
            params.push_back(sizeTy);
            params.push_back(sizeTy);
        }

        if (LLVM_LIKELY(mSharedStateType)) {
            params.push_back(PointerType::getUnqual(b.getContext()));
        }
        for (const Binding & binding : mInputScalars) {
            params.push_back(CBuilder::convertTypeToLLVMContext(b.getContext(), binding.getType()));
        }
        addAdditionalInitializationArgTypes(b, params);
        FunctionType * const initType = FunctionType::get(sizeTy, params, false);
        initFunc = Function::Create(initType, linkageType, funcName, m);
        initFunc->setCallingConv(CallingConv::C);
        initFunc->setVisibility(GlobalValue::DefaultVisibility);
        initFunc->setDoesNotRecurse();
        if (LLVM_UNLIKELY(ea)) {
            initFunc->setUWTableKind(UWTableKind::Default);
        }

        auto arg = initFunc->arg_begin();
        auto setNextArgName = [&](const StringRef name) {
            assert (arg != initFunc->arg_end());
            arg->setName(name);
            std::advance(arg, 1);
        };

        if (LLVM_UNLIKELY(ea)) {
            setNextArgName(".sharedSize");
            setNextArgName(".threadLocal");
        }

        if (LLVM_LIKELY(mSharedStateType)) {
            addNoCaptureAttr(arg);
            setNextArgName("shared");
        }
        for (const Binding & binding : mInputScalars) {
            setNextArgName(binding.getName());
        }
    }
    assert (&b.getContext() == &initFunc->getContext());
    return initFunc;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getExpectedOutputSizeFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getExpectedOutputSizeFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), GET_EXPECTED_OUTPUT_SIZE_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addExpectedOutputSizeDeclaration(b, linkageType);
    }
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addExpectedOutputSizeDeclaration
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addExpectedOutputSizeDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {
    SmallVector<char, 256> tmp;
    const auto funcName = concat(getName(), GET_EXPECTED_OUTPUT_SIZE_SUFFIX, tmp);
    Module * const m = b.getModule();
    Function * func = m->getFunction(funcName);
    if (LLVM_LIKELY(func == nullptr)) {
        SmallVector<Type *, 1> params;
        if (LLVM_LIKELY(mSharedStateType)) {
            params.push_back(PointerType::getUnqual(b.getContext()));
        }
        FunctionType * const funcType = FunctionType::get(b.getSizeTy(), params, false);
        func = Function::Create(funcType, linkageType, funcName, m);
        func->setCallingConv(CallingConv::C);
        func->setVisibility(GlobalValue::DefaultVisibility);
        func->setDoesNotRecurse();

        auto arg = func->arg_begin();
        auto setNextArgName = [&](const StringRef name) {
            assert (arg != func->arg_end());
            arg->setName(name);
            std::advance(arg, 1);
        };
        if (LLVM_LIKELY(mSharedStateType)) {
            addNoCaptureAttr(arg);
            setNextArgName("shared");
        }
    }
    assert (&b.getContext() == &func->getContext());
    return func;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addFamilyInitializationArgTypes
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::addAdditionalInitializationArgTypes(KernelBuilder & /* b */, InitArgTypes & /* argTypes */) const {

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInitializeThreadLocalFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getInitializeThreadLocalFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), INITIALIZE_THREAD_LOCAL_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addInitializeThreadLocalDeclaration(b, linkageType);
    }
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addInitializeThreadLocalDeclaration
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addInitializeThreadLocalDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {
    Function * func = nullptr;
    assert (mThreadLocalStateType);
    SmallVector<char, 256> tmp;
    const auto funcName = concat(getName(), INITIALIZE_THREAD_LOCAL_SUFFIX, tmp);
    Module * const m = b.getModule();
    func = m->getFunction(funcName);
    if (LLVM_LIKELY(func == nullptr)) {
        SmallVector<Type *, 2> params;
        PointerType * const ptrTy = PointerType::getUnqual(b.getContext());
        if (LLVM_LIKELY(mSharedStateType)) {
            params.push_back(ptrTy);
        }
        params.push_back(ptrTy);
        FunctionType * const funcType = FunctionType::get(ptrTy, params, false);
        func = Function::Create(funcType, linkageType, funcName, m);
        func->setCallingConv(CallingConv::C);
        func->setVisibility(GlobalValue::DefaultVisibility);
        func->setDoesNotRecurse();
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
            func->setUWTableKind(UWTableKind::Default);
        }

        auto arg = func->arg_begin();
        auto setNextArgName = [&](const StringRef name) {
            assert (arg != func->arg_end());
            arg->setName(name);
            std::advance(arg, 1);
        };
        if (LLVM_LIKELY(mSharedStateType)) {
            addNoCaptureAttr(arg);
            setNextArgName("shared");
        }
        addNoCaptureAttr(arg);
        setNextArgName("threadlocal");
        assert (arg == func->arg_end());
    }
    assert (func);
    assert (&b.getContext() == &func->getContext());
    return func;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getAllocateInternalStreamSets
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getAllocateSharedInternalStreamSetsFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), ALLOCATE_SHARED_INTERNAL_STREAMSETS_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addAllocateSharedInternalStreamSetsDeclaration(b, linkageType);
    }
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addAllocateInternalStreamSets
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addAllocateSharedInternalStreamSetsDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {
    Function * func = nullptr;
    if (allocatesInternalStreamSets()) {
        SmallVector<char, 256> tmp;
        const auto funcName = concat(getName(), ALLOCATE_SHARED_INTERNAL_STREAMSETS_SUFFIX, tmp);
        Module * const m = b.getModule();
        func = m->getFunction(funcName);

        if (LLVM_LIKELY(func == nullptr)) {

            PointerType * const ptrTy = PointerType::getUnqual(b.getContext());
            SmallVector<Type *, 6> params;
            if (LLVM_LIKELY(mSharedStateType)) {
                params.push_back(ptrTy);
            }
            params.push_back(b.getSizeTy());
            const auto tdb = (getKernelFlags() & KernelFlags::HasInternallyManagedStreamSet) && codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers);
            if (LLVM_UNLIKELY(tdb)) {
                params.push_back(ptrTy);
                params.push_back(ptrTy);
            }
            FunctionType * const funcType = FunctionType::get(b.getVoidTy(), params, false);
            func = Function::Create(funcType, linkageType, funcName, m);
            func->setCallingConv(CallingConv::C);
            func->setVisibility(GlobalValue::DefaultVisibility);
            func->setDoesNotRecurse();
            if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
                func->setUWTableKind(UWTableKind::Default);
            }

            auto arg = func->arg_begin();
            auto setNextArgName = [&](const StringRef name) {
                assert (arg != func->arg_end());
                arg->setName(name);
                std::advance(arg, 1);
            };
            if (LLVM_LIKELY(mSharedStateType)) {
                addNoCaptureAttr(arg);
                setNextArgName("shared");
            }
            setNextArgName("expectedNumOfStrides");
            if (LLVM_UNLIKELY(tdb)) {
                setNextArgName("reportExpansion");
                setNextArgName("pipelineHandle");
            }
            assert (arg == func->arg_end());
        }
        assert (func);
        assert (&b.getContext() == &func->getContext());
    }
    return func;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateAllocateSharedInternalStreamSetsMethod
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::generateAllocateSharedInternalStreamSetsMethod(KernelBuilder & /* b */, Value * /* expectedNumOfStrides */) {

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateAllocateSharedInternalStreamSetsMethod
 ** ------------------------------------------------------------------------------------------------------------- */
Value * Kernel::generateExpectedOutputSizeMethod(KernelBuilder & b) {
    return b.getSize(0);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getAllocateThreadLocalInternalStreamSetsFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getAllocateThreadLocalInternalStreamSetsFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), ALLOCATE_THREAD_LOCAL_INTERNAL_STREAMSETS_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addAllocateThreadLocalInternalStreamSetsDeclaration(b, linkageType);
    }
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addAllocateThreadLocalInternalStreamSetsDeclaration
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addAllocateThreadLocalInternalStreamSetsDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {
    Function * func = nullptr;
    if (allocatesInternalStreamSets()) {
        SmallVector<char, 256> tmp;
        const auto funcName = concat(getName(), ALLOCATE_THREAD_LOCAL_INTERNAL_STREAMSETS_SUFFIX, tmp);
        Module * const m = b.getModule();
        func = m->getFunction(funcName);
        if (LLVM_LIKELY(func == nullptr)) {

            SmallVector<Type *, 3> params;

            if (mThreadLocalStateType) {
                PointerType * ptrTy = PointerType::getUnqual(b.getContext());
                if (LLVM_LIKELY(mSharedStateType)) {
                    params.push_back(ptrTy);
                }
                params.push_back(ptrTy);
                params.push_back(b.getSizeTy());
            }

            FunctionType * const funcType = FunctionType::get(b.getVoidTy(), params, false);
            func = Function::Create(funcType, linkageType, funcName, m);
            func->setCallingConv(CallingConv::C);
            func->setVisibility(GlobalValue::DefaultVisibility);
            func->setDoesNotRecurse();
            if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
                func->setUWTableKind(UWTableKind::Default);
            }
            auto arg = func->arg_begin();
            auto setNextArgName = [&](const StringRef name) {
                assert (arg != func->arg_end());
                arg->setName(name);
                std::advance(arg, 1);
            };

            if (mThreadLocalStateType) {
                if (LLVM_LIKELY(mSharedStateType)) {
                    addNoCaptureAttr(arg);
                    setNextArgName("shared");
                }
                setNextArgName("threadLocal");
                setNextArgName("expectedNumOfStrides");
            }
            assert (arg == func->arg_end());
        }
        assert (func);
        assert (&b.getContext() == &func->getContext());
    }
    return func;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateAllocateThreadLocalInternalStreamSetsMethod
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::generateAllocateThreadLocalInternalStreamSetsMethod(KernelBuilder & /* b */, Value * /* expectedNumOfStrides */) {

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getDoSegmentFields
 ** ------------------------------------------------------------------------------------------------------------- */
std::vector<Type *> Kernel::getDoSegmentFields(KernelBuilder & b) const {

    // WARNING: any change to this must be reflected in addDoSegmentDeclaration,
    // KernelCompiler::getDoSegmentProperties, KernelCompiler::setDoSegmentProperties,
    // PipelineCompiler::buildKernelCallArgumentList and PipelineKernel::addOrDeclareMainFunction

    IntegerType * const sizeTy = b.getSizeTy();
    PointerType * const ptrTy = PointerType::getUnqual(b.getContext());
    const auto n = mInputStreamSets.size();
    const auto m = mOutputStreamSets.size();

    std::vector<Type *> fields;
    fields.reserve(4 + 3 * (n + m));
    if (LLVM_LIKELY(mSharedStateType)) {
        fields.push_back(ptrTy);  // handle
    }
    if (LLVM_UNLIKELY(mThreadLocalStateType)) {
        fields.push_back(ptrTy);  // handle
    }
    const auto internallySynchronized = hasAttribute(AttrId::InternallySynchronized);
    const auto isPipeline = (getTypeId() == TypeId::Pipeline);
    const auto isMainPipeline = isPipeline && !internallySynchronized;

    if (LLVM_LIKELY(!isMainPipeline)) {
        if (LLVM_UNLIKELY(internallySynchronized)) {
            fields.push_back(sizeTy); // external SegNo
        }
        fields.push_back(sizeTy); // numOfStrides
        if (LLVM_LIKELY(hasFixedRateIO())) {
            fields.push_back(sizeTy); // fixed rate factor
        }
        #ifdef ENABLE_PAPI
        if (LLVM_UNLIKELY(codegen::PapiCounterOptions.compare(codegen::OmittedOption) != 0)) {
            fields.push_back(b.getInt32Ty()); // eventSetId
        }
        #endif
    } else {
        fields.push_back(sizeTy); // segmentSize
    }


    const auto checkStreamSet = codegen::DebugOptionIsSet(codegen::EnableAsserts, codegen::EnableStreamSetAsserts);

    for (unsigned i = 0; i < n; ++i) {
        const Binding & input = mInputStreamSets[i];
        // virtual base input address
        fields.push_back(ptrTy);
        if (LLVM_UNLIKELY(internallySynchronized)) {
            fields.push_back(sizeTy); // is closed
        }
        // processed input items
        if (isMainPipeline || isAddressable(input)) {
            fields.push_back(ptrTy); // updatable
        } else if (isCountable(input)) {
            fields.push_back(sizeTy); // constant
        }
        // accessible input items
        if (isMainPipeline || requiresItemCount(input)) {
            fields.push_back(sizeTy);
        }
        if (LLVM_UNLIKELY(checkStreamSet)) {
            fields.push_back(sizeTy); // safe read limit
        }
    }

    const auto hasTerminationSignal = canSetTerminateSignal();

    for (unsigned i = 0; i < m; ++i) {
        const Binding & output = mOutputStreamSets[i];

        const auto isLocal = Kernel::isLocalBuffer(output);

        // shared dynamic buffer handle or virtual base output address
        fields.push_back(ptrTy);

        //TODO: if an I/O rate is deferred and this is internally synchronized, we need both item counts

        // produced output items
        if (hasTerminationSignal || isMainPipeline || isAddressable(output)) {
            fields.push_back(ptrTy); // updatable
        } else if (isCountable(output)) {
            fields.push_back(sizeTy); // constant
        }
        // Although local buffers are considered to be owned by (and thus the memory managed by) this
        // kernel, the OptimizationBranch kernel delegates owned buffer management to its branch
        // pipelines. This means there are at least two seperate owners for a buffer; however, we know
        // by construction only one branch can be executing and any kernels within the pipelines must
        // be synchronized; thus at most one branch could resize a buffer at any particular moment.
        // However, we need to share the current state of the buffer between the branches to ensure
        // that we are not using an old buffer allocation.
        if (isLocal.any()) {
            fields.push_back(sizeTy); // consumed
            if (LLVM_UNLIKELY(checkStreamSet && !isLocal.isShared())) {
                fields.push_back(ptrTy); // updatable capacity
            }
        } else {
            if (isMainPipeline || requiresItemCount(output)) {
                fields.push_back(sizeTy); // writable item count
            }
            if (LLVM_UNLIKELY(checkStreamSet)) {
                fields.push_back(sizeTy); // safe write limit
            }
        }
    }

    if (LLVM_UNLIKELY((getKernelFlags() & KernelFlags::HasInternallyManagedStreamSet) && codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers))) {
        fields.push_back(ptrTy); // reportExpansionCallback
        fields.push_back(ptrTy); // pipelineHandle
    }

    return fields;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addDoSegmentDeclaration
 ** ------------------------------------------------------------------------------------------------------------ */
Function * Kernel::addDoSegmentDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {

    // WARNING: any change to this must be reflected in getDoSegmentProperties, setDoSegmentProperties,
    // getDoSegmentFields, and PipelineCompiler::writeKernelCall

    SmallVector<char, 256> tmp;
    const auto funcName = concat(getName(), DO_SEGMENT_SUFFIX, tmp);
    Module * const m = b.getModule();
    Function * doSegment = m->getFunction(funcName);
    if (LLVM_LIKELY(doSegment == nullptr)) {

        const auto internallySynchronized = hasAttribute(AttrId::InternallySynchronized);

        Type * const retTy = (internallySynchronized || canSetTerminateSignal()) ? b.getSizeTy() : b.getVoidTy();
        FunctionType * const doSegmentType = FunctionType::get(retTy, getDoSegmentFields(b), false);
        doSegment = Function::Create(doSegmentType, linkageType, funcName, m);
        doSegment->setCallingConv(CallingConv::C);
        doSegment->setVisibility(GlobalValue::DefaultVisibility);
        doSegment->setDoesNotRecurse();
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
            doSegment->setUWTableKind(UWTableKind::Default);
        }
        auto arg = doSegment->arg_begin();
        auto setNextArgName = [&](const StringRef name) {
            assert (arg);
            assert (arg != doSegment->arg_end());
            arg->setName(name);
            // TODO: add WriteOnly attributes for the appropriate I/O parameters?
            std::advance(arg, 1);
        };
        if (LLVM_LIKELY(mSharedStateType)) {
            addNoCaptureAttr(arg);
            setNextArgName("shared");
        }
        if (LLVM_UNLIKELY(mThreadLocalStateType)) {
            addNoCaptureAttr(arg);
            setNextArgName("threadLocal");
        }

        const auto isPipeline = (getTypeId() == TypeId::Pipeline);
        const auto isMainPipeline = isPipeline && !internallySynchronized;

        if (LLVM_UNLIKELY(isMainPipeline)) {
            setNextArgName("segmentSize");
        } else {
            if (LLVM_UNLIKELY(internallySynchronized)) {
                setNextArgName("segNo");
            }
            setNextArgName("numOfStrides");
            if (hasFixedRateIO()) {
                setNextArgName("fixedRateFactor");
            }
            #ifdef ENABLE_PAPI
            if (LLVM_UNLIKELY(codegen::PapiCounterOptions.compare(codegen::OmittedOption) != 0)) {
                setNextArgName("eventSetId");
            }
            #endif
        }

        const auto checkStreamSet = codegen::DebugOptionIsSet(codegen::EnableAsserts, codegen::EnableStreamSetAsserts);

        for (unsigned i = 0; i < mInputStreamSets.size(); ++i) {
            const Binding & input = mInputStreamSets[i];
            setNextArgName(input.getName());
            if (LLVM_UNLIKELY(internallySynchronized)) {
                setNextArgName(input.getName() + "_closed");
            }
            if (LLVM_LIKELY(isMainPipeline || isAddressable(input) || isCountable(input))) {
                setNextArgName(input.getName() + "_processed");
            }
            if (isMainPipeline || requiresItemCount(input)) {
                setNextArgName(input.getName() + "_accessible");
            }
            if (LLVM_UNLIKELY(checkStreamSet)) {
                setNextArgName(input.getName() + "_capacity");
            }
        }

        const auto hasTerminationSignal = canSetTerminateSignal();

        for (unsigned i = 0; i < mOutputStreamSets.size(); ++i) {
            const Binding & output = mOutputStreamSets[i];
            setNextArgName(output.getName());
            if (LLVM_LIKELY(hasTerminationSignal || isAddressable(output) || isCountable(output))) {
                setNextArgName(output.getName() + "_produced");
            }
            const auto isLocal = Kernel::isLocalBuffer(output);
            if (LLVM_UNLIKELY(isLocal.any())) {
                setNextArgName(output.getName() + "_consumed");
                if (LLVM_UNLIKELY(checkStreamSet && !isLocal.isShared())) {
                    setNextArgName(output.getName() + "_updatableCapacity");
                }
            } else {
                if (isMainPipeline || requiresItemCount(output)) {
                    setNextArgName(output.getName() + "_writable");
                }
                if (LLVM_UNLIKELY(checkStreamSet)) {
                    setNextArgName(output.getName() + "_capacity");
                }
            }
        }

        const auto hasManagedOutput = (getKernelFlags() & KernelFlags::HasInternallyManagedStreamSet);

        if (LLVM_UNLIKELY(hasManagedOutput && codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers))) {
            setNextArgName("reportExpansionCallback");
            setNextArgName("pipelineHandle");
        }
    }
    assert (&b.getContext() == &doSegment->getContext());
    return doSegment;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getDoSegmentFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getDoSegmentFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), DO_SEGMENT_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addDoSegmentDeclaration(b, linkageType);
    }
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFinalizeThreadLocalFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getFinalizeThreadLocalFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), FINALIZE_THREAD_LOCAL_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addFinalizeThreadLocalDeclaration(b, linkageType);
    }
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addFinalizeThreadLocalDeclaration
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addFinalizeThreadLocalDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {
    Function * func = nullptr;

    SmallVector<char, 256> tmp;
    const auto funcName = concat(getName(), FINALIZE_THREAD_LOCAL_SUFFIX, tmp);
    Module * const m = b.getModule();
    func = m->getFunction(funcName);
    if (LLVM_LIKELY(func == nullptr)) {
        SmallVector<Type *, 2> params;
        PointerType * const ptrTy = PointerType::getUnqual(b.getContext());
        if (mThreadLocalStateType) {
            if (LLVM_LIKELY(mSharedStateType)) {
                params.push_back(ptrTy);
            }
            params.push_back(ptrTy); // common/main thread local
            params.push_back(ptrTy); // current thread local
        }
        FunctionType * const funcType = FunctionType::get(b.getVoidTy(), params, false);
        func = Function::Create(funcType, linkageType, funcName, m);
        func->setCallingConv(CallingConv::C);
        func->setVisibility(GlobalValue::DefaultVisibility);
        func->setDoesNotRecurse();
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
            func->setUWTableKind(UWTableKind::Default);
        }

        auto arg = func->arg_begin();
        auto setNextArgName = [&](const StringRef name) {
            assert (arg != func->arg_end());
            arg->setName(name);
            std::advance(arg, 1);
        };
        if (mThreadLocalStateType) {
            if (LLVM_LIKELY(mSharedStateType)) {
                setNextArgName("shared");
            }
            setNextArgName("main_thread_local");
            setNextArgName("thread_local");
        }
        assert (arg == func->arg_end());
    }
    assert (&b.getContext() == &func->getContext());

    return func;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFinalizeFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::getFinalizeFunction(KernelBuilder & b, const bool alwayReturnDeclaration, GlobalValue::LinkageTypes linkageType) const {
    const Module * const module = b.getModule();
    SmallVector<char, 256> tmp;
    Function * f = module->getFunction(concat(getName(), FINALIZE_SUFFIX, tmp));
    if (LLVM_UNLIKELY(f == nullptr && alwayReturnDeclaration)) {
        f = addFinalizeDeclaration(b, linkageType);
    }
    assert (b.getModule() == f->getParent());
    assert (&b.getContext() == &f->getContext());
    return f;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addFinalizeDeclaration
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addFinalizeDeclaration(KernelBuilder & b, GlobalValue::LinkageTypes linkageType) const {
    SmallVector<char, 256> tmp;
    const auto funcName = concat(getName(), FINALIZE_SUFFIX, tmp);
    Module * const m = b.getModule();
    Function * terminateFunc = m->getFunction(funcName);
    if (LLVM_LIKELY(terminateFunc == nullptr)) {
        Type * resultType = nullptr;
        if (mOutputScalars.empty()) {
            resultType = b.getVoidTy();
        } else {
            const auto n = mOutputScalars.size();
            SmallVector<Type *, 16> outputType(n);
            auto & C = b.getContext();
            for (unsigned i = 0; i < n; ++i) {
                outputType[i] = CBuilder::convertTypeToLLVMContext(C, mOutputScalars[i].getType());
            }
            if (n == 1) {
                resultType = outputType[0];
            } else {
                resultType = StructType::get(C, outputType);
            }
        }
        PointerType * const ptrTy = PointerType::getUnqual(b.getContext());
        std::vector<Type *> params;
        if (LLVM_LIKELY(mSharedStateType)) {
            params.push_back(ptrTy);
        }
        if (LLVM_LIKELY(mThreadLocalStateType)) {
            params.push_back(ptrTy);
        }
        FunctionType * const terminateType = FunctionType::get(resultType, params, false);
        terminateFunc = Function::Create(terminateType, linkageType, funcName, m);
        terminateFunc->setCallingConv(CallingConv::C);
        terminateFunc->setVisibility(GlobalValue::DefaultVisibility);
        terminateFunc->setDoesNotRecurse();
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
            terminateFunc->setUWTableKind(UWTableKind::Default);
        }

        auto args = terminateFunc->arg_begin();
        if (LLVM_LIKELY(mSharedStateType)) {
            (args++)->setName("shared");
        }
        if (LLVM_LIKELY(mThreadLocalStateType)) {
            (args++)->setName("threadLocal");
        }
        assert (args == terminateFunc->arg_end());
    }
    assert (&b.getContext() == &terminateFunc->getContext());
    return terminateFunc;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addOrDeclareMainFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Function * Kernel::addOrDeclareMainFunction(KernelBuilder & /* b */, const MainMethodGenerationType /* method */) const {
    report_fatal_error("Only PipelineKernels can declare a main method.");
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief createInstance
 ** ------------------------------------------------------------------------------------------------------------- */
Value * Kernel::createInstance(KernelBuilder & b) const {

    if (LLVM_LIKELY(mSharedStateType)) {
        auto & DL = b.getModule()->getDataLayout();
        Constant *  const stateTySize = b.getSize(b.getTypeSize(DL, mSharedStateType));
        Value * const stateObj = b.CreatePageAlignedMalloc(stateTySize);
        const auto align = DL.getABITypeAlign(mSharedStateType).value();
        b.CreateMemZero(stateObj, stateTySize, align);
        return stateObj;
    }

    llvm_unreachable("createInstance should not be called on stateless kernels");
    return nullptr;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initializeThreadLocalInstance
 ** ------------------------------------------------------------------------------------------------------------- */
Value * Kernel::initializeThreadLocalInstance(KernelBuilder & b, ArrayRef<Value *> args) const {
    Value * instance = nullptr;
    if (mThreadLocalStateType) {
        Function * const init = getInitializeThreadLocalFunction(b, true, GlobalValue::ExternalLinkage);
        instance = b.CreateCall(init->getFunctionType(), init, args);
    }
    return instance;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief finalizeThreadLocalInstance
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::finalizeThreadLocalInstance(KernelBuilder & b, ArrayRef<Value *> args) const {
    if (mThreadLocalStateType) {
        Function * const init = getFinalizeThreadLocalFunction(b, true, GlobalValue::ExternalLinkage); assert (init);
        b.CreateCall(init->getFunctionType(), init, args);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief finalizeInstance
 ** ------------------------------------------------------------------------------------------------------------- */
Value * Kernel::finalizeInstance(KernelBuilder & b, ArrayRef<Value *> args) const {
    Function * const termFunc = getFinalizeFunction(b, true, GlobalValue::ExternalLinkage);
    Value * result = b.CreateCall(termFunc->getFunctionType(), termFunc, args);
    if (mOutputScalars.empty()) {
        assert (!result || result->getType()->isVoidTy());
        result = nullptr;
    }
    return result;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructFamilyKernels
 ** ------------------------------------------------------------------------------------------------------------- */
Value * Kernel::constructFamilyKernels(KernelBuilder & b, InitArgs & hostArgs, ParamMap & params, NestedStateObjs & toFree) const {

    // TODO: need to test for termination on init call

    Value * handle = nullptr;
    BEGIN_SCOPED_REGION
    InitArgs initArgs;

    auto addInitArg = [&](Value * value) {
        assert (&value->getContext() == &b.getContext());
        initArgs.push_back(value);
    };

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
        Constant * sharedStateTySize = nullptr;
        if (mSharedStateType) {
            sharedStateTySize = b.getTypeSize(mSharedStateType);
        } else {
            sharedStateTySize = ConstantInt::getAllOnesValue(b.getSizeTy());
        }
        addInitArg(sharedStateTySize);

        Constant * threadLocalTySize = nullptr;
        if (mThreadLocalStateType) {
            threadLocalTySize = b.getTypeSize(mThreadLocalStateType);
        } else {
            threadLocalTySize = ConstantInt::getAllOnesValue(b.getSizeTy());
        }
        addInitArg(threadLocalTySize);
    }


    if (LLVM_LIKELY(mSharedStateType)) {
        handle = createInstance(b);
        addInitArg(handle);
        toFree.push_back(handle);
    }
    for (const Binding & input : mInputScalars) {
        const auto val = params.get(input.getRelationship());
        if (LLVM_UNLIKELY(val == nullptr)) {
            SmallVector<char, 512> tmp;
            raw_svector_ostream out(tmp);
            out << "Could not find paramater for " << getName() << ':' << input.getName()
                << " from the provided program parameters";
            report_fatal_error(StringRef(out.str()));
        }
        addInitArg(val);
    }

    errs() << " FAMILY " << getName() << " -> " << b.getModule()->getModuleIdentifier() << "\n";
    Function * const init = getInitializeFunction(b, true, GlobalValue::ExternalLinkage);
    assert (&init->getContext() == &b.getContext());

    // If we're calling this with a family call, then the family kernels associated with it
    // must be passed into the function itself.

    recursivelyConstructFamilyKernels(b, initArgs, params, toFree);

    if (hasInternallyGeneratedStreamSets()) {
        for (const auto & rs : getInternallyGeneratedStreamSets()) {
            ParamMap::PairEntry entry;
            if (LLVM_UNLIKELY(!params.get(rs, entry))) {
                SmallVector<char, 512> tmp;
                raw_svector_ostream out(tmp);
                out << "Could not find paramater for "
                    << "internally generated streamset"
                    << " from the provided program parameters";
                report_fatal_error(StringRef(out.str()));
            }
            addInitArg(entry.first);
            addInitArg(entry.second);
        }
    }

    assert (init->getFunctionType()->getNumParams() == initArgs.size());
    b.CreateCall(init->getFunctionType(), init, initArgs);
    END_SCOPED_REGION

    PointerType * const voidPtrTy = PointerType::getUnqual(b.getContext());
    Value * const voidPtr = ConstantPointerNull::get(voidPtrTy);

    hostArgs.reserve(7);

    auto addHostArg = [&](Value * ptr) {
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
            b.CreateAssert(ptr, "constructFamilyKernels cannot pass a null value to pipeline");
        }
        hostArgs.push_back(ptr);
    };

    auto addHostVoidArg = [&]() {
        hostArgs.push_back(voidPtr);
    };
    #ifndef NDEBUG
    const auto originalNumOfHoseArgs = hostArgs.size();
    #endif
    if (LLVM_LIKELY(mSharedStateType)) {
        addHostArg(handle);
    } else {
        addHostVoidArg();
    }
    const auto tl = (mThreadLocalStateType) != 0;
    const auto ai = allocatesInternalStreamSets();
    if (ai) {
        addHostArg(getAllocateSharedInternalStreamSetsFunction(b, true, GlobalValue::ExternalLinkage));
    } else {
        addHostVoidArg();
    }
    if (tl) {
        addHostArg(getInitializeThreadLocalFunction(b, true, GlobalValue::ExternalLinkage));
        if (ai) {
            addHostArg(getAllocateThreadLocalInternalStreamSetsFunction(b, true, GlobalValue::ExternalLinkage));
        } else {
            addHostVoidArg();
        }
    } else {
        addHostVoidArg();
        addHostVoidArg();
    }
    addHostArg(getDoSegmentFunction(b, true, GlobalValue::ExternalLinkage));
    if (tl) {
        addHostArg(getFinalizeThreadLocalFunction(b, true, GlobalValue::ExternalLinkage));
    } else {
        addHostVoidArg();
    }

    // TODO: queue these in a list of termination functions to add to main?
    addHostArg(getFinalizeFunction(b, true, GlobalValue::ExternalLinkage));

    assert (hostArgs.size() == (originalNumOfHoseArgs + 7));

    return handle;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief recursivelyConstructFamilyKernels
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::recursivelyConstructFamilyKernels(KernelBuilder & b, InitArgs & args, ParamMap & params, NestedStateObjs & toFree) const {
    /* do nothing */
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief recursivelyListFamilyKernels
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::recursivelyListFamilyKernels(raw_ostream & familyName) const {
    /* do nothing */
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief runOptimizationPasses
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::addOptimizationPasses(KernelBuilder & b, SelectedOptimizationPasses & passes) const {
    /* do nothing */
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getStringHash
 *
 * Create a fixed length string hash of the given str
 ** ------------------------------------------------------------------------------------------------------------- */
std::string Kernel::getStringHash(const StringRef str) {

    sha1::digest_type digest;

    constexpr auto length = sizeof(sha1::digest_type);

    sha1 sha1;
    sha1.process_bytes(str.data(), str.size());
    sha1.get_digest(digest);

    std::string buffer;
    buffer.reserve(length * 2 + 1);
    raw_string_ostream out(buffer);

    constexpr static char hex_table[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd','e', 'f'};

    const uint8_t * const d = reinterpret_cast<uint8_t *>(digest);
    for (size_t i = 0; i < length; ++i) {
        const auto c = d[i];
        out << hex_table[(c & 0xF)] << hex_table[(c >> 4)];
    }

    out.flush();

    return buffer;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getSharedStateType
 ** ------------------------------------------------------------------------------------------------------------- */
llvm::StructType * Kernel::getSharedStateType(llvm::LLVMContext & C) const {
    if (mSharedStateType) {
        return static_cast<llvm::StructType *>(CBuilder::convertTypeToLLVMContext(C, mSharedStateType));
    } else {
        return nullptr;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getThreadLocalStateType
 ** ------------------------------------------------------------------------------------------------------------- */
llvm::StructType * Kernel::getThreadLocalStateType(llvm::LLVMContext & C) const {
    if (mThreadLocalStateType) {
        return static_cast<llvm::StructType *>(CBuilder::convertTypeToLLVMContext(C, mThreadLocalStateType));
    } else {
        return nullptr;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setInputStreamSetAt
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::setInputStreamSetAt(const unsigned i, StreamSet * const value) {
    mInputStreamSets[i].setRelationship(value);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setOutputStreamSetAt
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::setOutputStreamSetAt(const unsigned i, StreamSet * const value) {
    mOutputStreamSets[i].setRelationship(value);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setInputScalarAt
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::setInputScalarAt(const unsigned i, Scalar * const value) {
    mInputScalars[i].setRelationship(value);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief setOutputScalarAt
 ** ------------------------------------------------------------------------------------------------------------- */
void Kernel::setOutputScalarAt(const unsigned i, Scalar * const value) {
    mOutputScalars[i].setRelationship(value);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateKernelMethod
 ** ------------------------------------------------------------------------------------------------------------- */
void SegmentOrientedKernel::generateKernelMethod(KernelBuilder & b, llvm::TargetMachine * TM) {
    generateDoSegmentMethod(b);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief hasAnySharedOrOutputScalars
 ** ------------------------------------------------------------------------------------------------------------- */
bool Kernel::noMutableSharedScalars() const {
    for (const InternalScalar & s : mInternalScalars) {
        if (LLVM_LIKELY(s.getScalarType() == ScalarType::Internal)) {
            return false;
        }
    }
    return mOutputScalars.empty();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFamilyName
 ** ------------------------------------------------------------------------------------------------------------- */
std::string Kernel::getFamilyName() const {
    std::string tmp;
    raw_string_ostream out(tmp);
    char flags = '0';
    if (getSharedStateType()) {
        flags += 1;
    }
    if (getThreadLocalStateType()) {
        flags += 2;
    }
    if (LLVM_UNLIKELY(allocatesInternalStreamSets())) {
        flags |= 4;
    }
    const char code = 'a' + flags;
    out << 'F' << code << getStride() << ',';
    AttributeSet::print(out);
    for (const Binding & input : mInputScalars) {
        out << ",IV("; input.print(this, out); out << ')';
    }
    for (const Binding & input : mInputStreamSets) {
        out << ",IS("; input.print(this, out); out << ')';
    }
    for (const Binding & output : mOutputStreamSets) {
        out << ",OS("; output.print(this, out); out << ')';
    }
    for (const Binding & output : mOutputScalars) {
        out << ",OV("; output.print(this, out); out << ')';
    }
    recursivelyListFamilyKernels(out);
    out.flush();
    return tmp;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief annotateKernelNameWithDebugFlags
 ** ------------------------------------------------------------------------------------------------------------- */
/* static */ std::string Kernel::annotateKernelNameWithDebugFlags(const TypeId id, const unsigned flags, std::string && name) {
    raw_string_ostream buffer(name);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
        buffer << "_EA";
    } else {
        if (LLVM_UNLIKELY(id == TypeId::Pipeline || id == TypeId::PopCountKernel)) {
            // TODO: look into cleaner method for this
            if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnablePipelineAsserts))) {
                buffer << "_EP";
            }
        }
        if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableStreamSetAsserts))) {
            buffer << "_ES";
        }
    }
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        buffer << "_MP";
    }
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::DisableIndirectBranch))) {
        buffer << "_Ibranch";
    }
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::DisableCacheAlignedKernelStructs))) {
        buffer << "_DCacheAlign";
    }
    if (LLVM_UNLIKELY(codegen::FreeCallBisectLimit >= 0)) {
        buffer << "_FreeLimit";
    }
    if (LLVM_UNLIKELY(flags != 0)) {
        if (LLVM_UNLIKELY(((flags & KernelFlags::HasInOutStreamSet) != 0) && codegen::DebugOptionIsSet(codegen::DisableInOutAttributes))) {
            buffer << "_NoIOAttr";
        }
        if (LLVM_UNLIKELY(((flags & KernelFlags::HasInternallyManagedStreamSet) != 0) && codegen::StatisticsOptionIsSet(codegen::TraceDynamicBuffers))) {
            buffer << "_TDB";
        }
        if (flags & Kernel::KernelFlags::RequiresIllustratorObject) {
            buffer << "_Illustrated";
        }
    }
    #ifdef ENABLE_PAPI
    const auto & S = codegen::PapiCounterOptions;
    if (LLVM_UNLIKELY(S.compare(codegen::OmittedOption) != 0)) {
        buffer << "+PAPI";
    }
    #endif
    buffer.flush();
    return name;
}

static inline unsigned collectOutputFlags(const Bindings & streamSets) {
    unsigned flags = 0;
    for (const auto & output : streamSets) {
        if (LLVM_UNLIKELY(output.getRate().isUnknown())) {
            flags |= Kernel::KernelFlags::HasInternallyManagedStreamSet;
        }
        for (const auto & attr : output.getAttributes()) {
            switch (attr.getKind()) {
                case AttrId::ManagedBuffer:
                    flags |= Kernel::KernelFlags::HasInternallyManagedStreamSet;
                    break;
                case AttrId::InOut:
                    flags |= Kernel::KernelFlags::HasInOutStreamSet;
                    break;
                default: break;
            }
        }
    }
    return flags;
}

// CONSTRUCTOR
Kernel::Kernel(LLVMTypeSystemInterface & ts,
               const TypeId typeId,
               std::string && kernelName,
               Bindings && stream_inputs,
               Bindings && stream_outputs,
               Bindings && scalar_inputs,
               Bindings && scalar_outputs,
               InternalScalars && internal_scalars,
               CompilationStatus status, unsigned flags)
: mTypeId(typeId)
, mStride(ts.getBitBlockWidth())
, mFlags(flags | collectOutputFlags(stream_outputs))
, mInputStreamSets(std::move(stream_inputs))
, mOutputStreamSets(std::move(stream_outputs))
, mInputScalars(std::move(scalar_inputs))
, mOutputScalars(std::move(scalar_outputs))
, mInternalScalars( std::move(internal_scalars))
, mKernelName(annotateKernelNameWithDebugFlags(typeId, mFlags, std::move(kernelName))) {

}

const llvm::MDString * Kernel::readSignatureFromModule(const llvm::Module * const M) {
    NamedMDNode * const sig = M->getNamedMetadata(SIGNATURE);
    if (sig) {
        assert ("empty metadata node" && sig->getNumOperands() > 0);
        assert ("metadata should contain precisely one node" && sig->getNumOperands() == 1);
        assert ("no signature payload" && sig->getOperand(0)->getNumOperands() == 1);
        return cast<MDString>(sig->getOperand(0)->getOperand(0));
    }
    return nullptr;
}

void Kernel::writeSignatureToModule(const Kernel * const kernel, llvm::Module * const M) {
    if (LLVM_UNLIKELY(kernel->hasSignature())) {
        NamedMDNode * const md = M->getOrInsertNamedMetadata(SIGNATURE);
        assert (md->getNumOperands() == 0);
        MDString * const sig = MDString::get(M->getContext(), kernel->getSignature());
        md->addOperand(MDNode::get(M->getContext(), {sig}));
    }
}


Kernel::Kernel(LLVMTypeSystemInterface & ts,
               const TypeId typeId,
               AttributeSet && attributes,
               Bindings && stream_inputs,
               Bindings && stream_outputs,
               Bindings && scalar_inputs,
               Bindings && scalar_outputs,
               CompilationStatus status, unsigned flags)
: AttributeSet(std::move(attributes))
, mTypeId(typeId)
, mStride(ts.getBitBlockWidth())
, mFlags(flags | collectOutputFlags(stream_outputs))
, mInputStreamSets(std::move(stream_inputs))
, mOutputStreamSets(std::move(stream_outputs))
, mInputScalars(std::move(scalar_inputs))
, mOutputScalars(std::move(scalar_outputs))
, mInternalScalars()
, mKernelName() {

}

Kernel::~Kernel() { }

// CONSTRUCTOR
SegmentOrientedKernel::SegmentOrientedKernel(LLVMTypeSystemInterface & ts,
                                             std::string && kernelName,
                                             Bindings && stream_inputs,
                                             Bindings && stream_outputs,
                                             Bindings && scalar_parameters,
                                             Bindings && scalar_outputs,
                                             InternalScalars && internal_scalars,
                                             unsigned flags)
: Kernel(ts,
TypeId::SegmentOriented, std::move(kernelName),
std::move(stream_inputs), std::move(stream_outputs),
std::move(scalar_parameters), std::move(scalar_outputs),
std::move(internal_scalars),
CompilationStatus::FullyInitialized, flags) {

}

}

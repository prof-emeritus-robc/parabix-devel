/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include "binding_map.hpp"
#include "relationship.h"
#include "streamset.h"
#include <util/not_null.h>
#include <allocator/threadsafe_slaballocator.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Compiler.h>
#include <codegen/FunctionTypeBuilder.h>
#include <codegen/LLVMTypeSystemInterface.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <memory>
#include <string>
#include <vector>

namespace llvm { namespace orc { class LLJIT; } }
namespace llvm { class IndirectBrInst; }
namespace llvm { class PHINode; }
namespace llvm { class TargetMachine; }

class LLVMTypeSystemInterface;

namespace kernel {

class KernelBuilder;
class KernelCompiler;
class BlockKernelCompiler;
class StreamSetBuffer;
class StreamSet;
class ParabixIllustrator;

constexpr static auto KERNEL_ILLUSTRATOR_CALLBACK_OBJECT = "__illustrator";
constexpr static auto KERNEL_REGISTER_ILLUSTRATOR_CALLBACK = "__illustrator_register";
constexpr static auto KERNEL_ILLUSTRATOR_CAPTURE_CALLBACK = "__illustrator_capture";
constexpr static auto KERNEL_ILLUSTRATOR_STRIDE_NUM = "__illustrator_sn";

constexpr static auto KERNEL_ILLUSTRATOR_ENTER_KERNEL = "__illustrator_enter_kernel";
constexpr static auto KERNEL_ILLUSTRATOR_EXIT_KERNEL = "__illustrator_exit_kernel";

constexpr static auto KERNEL_ILLUSTRATOR_ENTER_LOOP = "__illustrator_enter_loop";
constexpr static auto KERNEL_ILLUSTRATOR_ITERATE_LOOP = "__illustrator_iterate_loop";
constexpr static auto KERNEL_ILLUSTRATOR_EXIT_LOOP = "__illustrator_exit_loop";

class Kernel : public SlabAllocatedObject, public AttributeSet {
    friend class KernelCompiler;
    friend class PipelineAnalysis;
    friend class PipelineCompiler;
    friend class PipelineBuilder;
    friend class PipelineKernel;
    friend class OptimizationBranchCompiler;
    friend class OptimizationBranch;
    friend class BaseDriver;
public:

    USE_SLAB_ALLOCATED_OBJECT_MEMORY_OPERATORS

    using Relationships = std::vector<const Relationship *>;

    enum class TypeId {
        SegmentOriented
        , MultiBlock
        , BlockOriented
        , Pipeline
        , OptimizationBranch
        , PopCountKernel
    };

    enum KernelFlags {
        HasInternallyManagedStreamSet = 1
        , RequiresIllustratorObject = 2
        , HasInOutStreamSet = 4
    };

    enum KernelCompilationPriority {
        Normal = 0
        , Medium = 1
        , High = 2
    };

    using InitArgs = llvm::SmallVector<llvm::Value *, 32>;

    using NestedStateObjs = llvm::SmallVector<llvm::Value *, 16>;

    using InitArgTypes = llvm::SmallVector<llvm::Type *, 32>;

    struct ParamMap {

        using PairEntry = std::pair<llvm::Value *, llvm::Value *>;

        inline llvm::Value * get(const Relationship * inputScalar) const {
            if (LLVM_UNLIKELY(llvm::isa<CommandLineScalar>(inputScalar))) {
                const auto k = (unsigned)llvm::cast<CommandLineScalar>(inputScalar)->getCLType();
                return mCommandLineMap[k];
            }
            const auto f = mRelationshipMap.find(inputScalar);
            if (LLVM_UNLIKELY(f == mRelationshipMap.end())) {
                return nullptr;
            }
            return f->second;
        }

        inline void set(const Relationship * inputScalar, llvm::Value * value) {
            if (LLVM_UNLIKELY(llvm::isa<CommandLineScalar>(inputScalar))) {
                const auto k = (unsigned)llvm::cast<CommandLineScalar>(inputScalar)->getCLType();
                assert ("relationship is already mapped to that value" && mCommandLineMap[k] == nullptr);
                mCommandLineMap[k] = value;
            } else {
                assert ("relationship is already mapped to that value" && mRelationshipMap.count(inputScalar) == 0);
                mRelationshipMap.insert(std::pair<const Relationship *, llvm::Value *>(inputScalar, value));
            }
        }

        inline bool get(const Relationship * inputScalar, PairEntry & pe) const {
            const auto f = mRelationshipPairMap.find(inputScalar);
            if (LLVM_UNLIKELY(f == mRelationshipPairMap.end())) {
                return false;
            }
            pe = f->second;
            return true;
        }

        inline void set(const Relationship * inputScalar, PairEntry value) {
            assert ("relationship is already mapped to that value" && mRelationshipPairMap.count(inputScalar) == 0);
            mRelationshipPairMap.insert(std::pair<const Relationship *, PairEntry>(inputScalar, value));
        }

    private:
        llvm::DenseMap<const Relationship *, llvm::Value *> mRelationshipMap;
        llvm::DenseMap<const Relationship *, PairEntry> mRelationshipPairMap;
        std::array<llvm::Value *, (unsigned)CommandLineScalarType::CommandLineScalarCount> mCommandLineMap{};
    };

    enum MainMethodGenerationType {
        AddInternal
        , DeclareExternal
        , AddExternal
    };

    using Rational = ProcessingRate::Rational;

    static bool classof(const Kernel *) { return true; }

    static bool classof(const void *) { return false; }

    LLVM_READNONE TypeId getTypeId() const {
        return mTypeId;
    }

    enum class ScalarType { Input, Output, Internal, NonPersistent, ThreadLocal };

    enum class ThreadLocalScalarAccumulationRule { DoNothing, Sum };

    struct InternalScalar {

        friend class Kernel;

        ScalarType getScalarType() const {
            return mScalarType;
        }

        llvm::Type * getValueType() const {
            return mValueType;
        }

        const std::string & getName() const {
            return mName;
        }

        unsigned getGroup() const {
            return mGroup;
        }

        ThreadLocalScalarAccumulationRule getAccumulationRule() const {
            return mAccumulationRule;
        }

        // The flat field index of this scalar within its owning kernel's constructed
        // state struct (mSharedStateType or mThreadLocalStateType, per getScalarType()).
        // Set exactly once, by Kernel::constructStateTypes, as it lays out that struct;
        // every other consumer (e.g. KernelCompiler's scalar-field-pointer lookup) must
        // read this value rather than independently recompute a struct position, so
        // construction and access can never silently diverge.
        unsigned getFieldIndex() const {
            assert (mFieldIndex != NO_FIELD_INDEX && "field index read before state type construction");
            return mFieldIndex;
        }

        explicit InternalScalar(llvm::Type * const valueType,
                                const llvm::StringRef name, const unsigned group = 0,
                                const ThreadLocalScalarAccumulationRule rule = ThreadLocalScalarAccumulationRule::DoNothing)
        : InternalScalar(ScalarType::Internal, valueType, name, group, rule) {

        }

        explicit InternalScalar(const ScalarType scalarType, llvm::Type * const valueType,
                                const llvm::StringRef name, const unsigned group = 0,
                                const ThreadLocalScalarAccumulationRule rule = ThreadLocalScalarAccumulationRule::DoNothing)
        : mScalarType(scalarType), mValueType(valueType), mName(name.str()), mGroup(group)
        , mAccumulationRule(rule) {
            assert (rule == ThreadLocalScalarAccumulationRule::DoNothing || scalarType == ScalarType::ThreadLocal);
        }

    protected:

        void setValueType(llvm::Type * type) {
            mValueType = type;
        }

        static constexpr unsigned NO_FIELD_INDEX = ~0U;

        void setFieldIndex(const unsigned index) {
            mFieldIndex = index;
        }

    private:
        const ScalarType                        mScalarType;
        llvm::Type *                            mValueType;
        const std::string                       mName;
        const unsigned                          mGroup;
        const ThreadLocalScalarAccumulationRule mAccumulationRule;
        unsigned                                mFieldIndex = NO_FIELD_INDEX;
    };

    using InternalScalars = std::vector<InternalScalar>;

    enum class PortType { Input, Output };

    struct StreamSetPort {
        PortType Type;
        unsigned Number;

        StreamSetPort() : Type(PortType::Input), Number(0) { }
        StreamSetPort(const PortType Type, const unsigned Number) : Type(Type), Number(Number) { }
        StreamSetPort(const StreamSetPort & other) = default;
        StreamSetPort & operator = (const StreamSetPort & other) {
            Type = other.Type;
            Number = other.Number;
            return *this;
        }
        bool operator < (const StreamSetPort other) const {
            if (Type == other.Type) {
                return Number < other.Number;
            } else {
                return Type == PortType::Input;
            }
        }

        bool operator == (const StreamSetPort other) const {
            return (Type == other.Type) && (Number == other.Number);
        }
    };

    // Kernel Signatures and Module IDs
    //
    // A kernel signature uniquely identifies a kernel and its full functionality.
    // In the event that a particular kernel instance is to be generated and compiled
    // to produce object code, and we have a cached kernel object code instance with
    // the same signature and targetting the same IDISA architecture, then the cached
    // object code may safely be used to avoid recompilation.
    //
    // A kernel signature is a byte string of arbitrary length.
    //
    // Kernel developers should take responsibility for designing appropriate signature
    // mechanisms that are short, inexpensive to compute and guarantee uniqueness
    // based on the semantics of the kernel.
    //
    // A kernel Module ID is short string that is used as a name for a particular kernel
    // instance.  Kernel Module IDs are used to look up and retrieve cached kernel
    // instances and so should be highly likely to uniquely identify a kernel instance.
    //
    // The ideal case is that a kernel Module ID serves as a full kernel signature thus
    // guaranteeing uniqueness.  In this case, hasSignature() should return false.
    //

    LLVM_READNONE const std::string & getName() const {
        return mKernelName;
    }

    LLVM_READNONE std::string getFamilyName() const;

    virtual bool isCachable() const { return true; }

    // Called by the PipelineBuilder when the kernel is added to a pipeline, after construction
    // is complete but before the kernel is registered with the driver.  A kernel type may add
    // attributes it can determine automatically; any that change the generated code must also
    // be recorded in the kernel name.
    virtual void addAutomaticAttributes() { }

    virtual bool hasSignature() const { return false; }

    virtual KernelCompilationPriority getCompilationPriority() const { return KernelCompilationPriority::Normal; }

    virtual llvm::StringRef getSignature() const {
        return getName();
    }

    LLVM_READNONE bool allocatesInternalStreamSets() const {
        return (mFlags & Kernel::KernelFlags::HasInternallyManagedStreamSet) != 0;
    }

    virtual bool requiresExplicitPartialFinalStride() const;

    unsigned getStride() const { return mStride; }

    void setStride(const unsigned stride) { mStride = stride; }

    const Bindings & getInputStreamSetBindings() const {
        return mInputStreamSets;
    }

    Bindings & getInputStreamSetBindings() {
        return mInputStreamSets;
    }

    const Binding & getInputStreamSetBinding(const unsigned i) const {
        assert (i < getNumOfStreamInputs());
        return mInputStreamSets[i];
    }

    LLVM_READNONE StreamSet * getInputStreamSet(const unsigned i) const {
        auto streamSet = getInputStreamSetBinding(i).getRelationship();
        assert (llvm::isa<TruncatedStreamSet>(streamSet) || llvm::isa<StreamSet>(streamSet) || llvm::isa<RepeatingStreamSet>(streamSet));
        return static_cast<StreamSet *>(streamSet);
    }

    LLVM_READNONE unsigned getNumOfStreamInputs() const {
        return mInputStreamSets.size();
    }

    virtual void setInputStreamSetAt(const unsigned i, StreamSet * value);

    LLVM_READNONE const Binding & getOutputStreamSetBinding(const unsigned i) const {
        assert (i < getNumOfStreamOutputs());
        return mOutputStreamSets[i];
    }

    LLVM_READNONE StreamSet * getOutputStreamSet(const unsigned i) const {
        auto streamSet = getOutputStreamSetBinding(i).getRelationship();
        assert (llvm::isa<TruncatedStreamSet>(streamSet) || llvm::isa<StreamSet>(streamSet));
        return static_cast<StreamSet *>(streamSet);
    }

    const Bindings & getOutputStreamSetBindings() const {
        return mOutputStreamSets;
    }

    Bindings & getOutputStreamSetBindings() {
        return mOutputStreamSets;
    }

    unsigned getNumOfStreamOutputs() const {
        return mOutputStreamSets.size();
    }

    Scalar * getInputScalarAt(const unsigned i) const {
        return llvm::cast<Scalar>(getInputScalarBinding(i).getRelationship());
    }

    virtual void setOutputStreamSetAt(const unsigned i, StreamSet * value);

    const Bindings & getInputScalarBindings() const {
        return mInputScalars;
    }

    Bindings & getInputScalarBindings() {
        return mInputScalars;
    }

    const Binding & getInputScalarBinding(const unsigned i) const {
        assert (i < mInputScalars.size());
        return mInputScalars[i];
    }

    LLVM_READNONE unsigned getNumOfScalarInputs() const {
        return mInputScalars.size();
    }

    // The flat field index of input scalar i within mSharedStateType, recorded once by
    // constructStateTypes as it lays out the struct. See InternalScalar::getFieldIndex
    // for why this must be read rather than independently recomputed.
    unsigned getInputScalarFieldIndex(const unsigned i) const {
        assert (i < mInputScalarFieldIndex.size());
        return mInputScalarFieldIndex[i];
    }

    virtual void setInputScalarAt(const unsigned i, Scalar * value);

    const Bindings & getOutputScalarBindings() const {
        return mOutputScalars;
    }

    Bindings & getOutputScalarBindings() {
        return mOutputScalars;
    }

    const Binding & getOutputScalarBinding(const unsigned i) const {
        assert (i < mOutputScalars.size());
        return mOutputScalars[i];
    }

    LLVM_READNONE unsigned getNumOfScalarOutputs() const {
        return mOutputScalars.size();
    }

    // See getInputScalarFieldIndex.
    unsigned getOutputScalarFieldIndex(const unsigned i) const {
        assert (i < mOutputScalarFieldIndex.size());
        return mOutputScalarFieldIndex[i];
    }

    Scalar * getOutputScalarAt(const unsigned i) const {
        return llvm::cast<Scalar>(getOutputScalarBinding(i).getRelationship());
    }

    virtual void setOutputScalarAt(const unsigned i, Scalar * value);

    void addInternalScalar(llvm::Type * type, const llvm::StringRef name, const unsigned group = 0) {
        mInternalScalars.emplace_back(ScalarType::Internal, type, name, group, ThreadLocalScalarAccumulationRule::DoNothing);
    }

    void addNonPersistentScalar(llvm::Type * type, const llvm::StringRef name) {
        mInternalScalars.emplace_back(ScalarType::NonPersistent, type, name, 0, ThreadLocalScalarAccumulationRule::DoNothing);
    }

    void addThreadLocalScalar(llvm::Type * type, const llvm::StringRef name, const unsigned group = 0,
                              const ThreadLocalScalarAccumulationRule rule = ThreadLocalScalarAccumulationRule::DoNothing) {
        mInternalScalars.emplace_back(ScalarType::ThreadLocal, type, name, group, rule);
    }

    llvm::StructType * getSharedStateType() const {
        return mSharedStateType;
    }

    void setSharedStateType(llvm::StructType * stateTy) {
        mSharedStateType = stateTy;
    }

    llvm::StructType * getSharedStateType(llvm::LLVMContext & C) const;

    llvm::StructType * getThreadLocalStateType() const {
        return mThreadLocalStateType;
    }

    void setThreadLocalStateType(llvm::StructType * stateTy) {
        mThreadLocalStateType = stateTy;
    }

    // For a Target that's about to fully reuse another, already-declared kernel's state
    // type (e.g. across LLVMContexts, via setSharedStateType/setThreadLocalStateType) but
    // still needs its own mInternalScalars list (and input/output scalar field indices)
    // populated to match: copies other's already-computed scalar list (including each
    // entry's recorded field index) wholesale, rather than independently recomputing it
    // via addInternalProperties(). A matching cache-name/signature already guarantees
    // Target and other were built identically (same kernel class, same construction
    // arguments), so recomputing would just reproduce the same result -- and for some
    // kernel classes (e.g. a PabloKernel's carry-structure analysis) recomputing it is
    // expensive enough to be a real cost, paid for nothing, every time a kernel with a
    // large enough body is deduplicated this way. mInputScalars/mOutputScalars (the
    // Bindings, fixed at construction time from the kernel's own declared I/O) are
    // asserted to already match, since those aren't copied here -- only the field
    // indices that locate them within the now-shared state type are.
    void copyInternalScalarsFrom(const Kernel & other) {
        assert (mInputScalars.size() == other.mInputScalars.size()
             && mOutputScalars.size() == other.mOutputScalars.size());
        mInternalScalars = InternalScalars(other.mInternalScalars.begin(), other.mInternalScalars.end());
        mInputScalarFieldIndex = other.mInputScalarFieldIndex;
        mOutputScalarFieldIndex = other.mOutputScalarFieldIndex;
    }

    llvm::StructType * getThreadLocalStateType(llvm::LLVMContext & C) const;

    std::string makeCacheName(KernelBuilder & b);

    llvm::Module * makeEmptyModule(KernelBuilder & b);

    void declareStateTypes(KernelBuilder & b);

    void declareKernel(KernelBuilder & b, llvm::TargetMachine * TM, llvm::GlobalValue::LinkageTypes linkageType);

    void generateKernel(KernelBuilder & b, llvm::TargetMachine * TM, llvm::GlobalValue::LinkageTypes linkageType);

    void loadCachedKernel(const llvm::Module * m);

    void recordScalarFieldIndices();

    struct LocalBufferFlagSet {
        enum LocalBufferFlagType : uint32_t {
            LBF_Shared = 1,
            LBF_Managed = 2,
            LBF_Returned = 4
        };

        bool isShared() const {
            return (Flags & LocalBufferFlagType::LBF_Shared) != 0;
        }

        bool isManaged() const {
            return (Flags & LocalBufferFlagType::LBF_Managed) != 0;
        }

        bool isReturned() const {
            return (Flags & LocalBufferFlagType::LBF_Returned) != 0;
        }

        bool any() const { return Flags != 0; }

        uint32_t Flags = 0;
    };

    LLVM_READNONE static LocalBufferFlagSet isLocalBuffer(const Binding & output);

    LLVM_READNONE static bool isManagedBuffer(const Binding & output);

    LLVM_READNONE bool canSetTerminateSignal() const;

    virtual std::unique_ptr<KernelCompiler> instantiateKernelCompiler(KernelBuilder & b);

    virtual ~Kernel();

    LLVM_READNONE virtual unsigned getNumOfNestedKernelFamilyCalls() const {
        return 0;
    }

    LLVM_READNONE unsigned getKernelFlags() const {
        return mFlags;
    }

    bool noMutableSharedScalars() const;

    enum class OptimizationPass {
        DCEPass,
        SimplifyCFGPass,
        EarlyCSEPass,
        MemCpyOptPass,
        AggressiveInstCombinePass,
        NewGVNPass,
        PHICanonicalizerPass,
        // Not a pass: a kernel that adds this marker in addOptimizationPasses drops
        // InstCombine from the base pass list the driver runs on every kernel. For
        // kernels whose IR is already simplified upstream (e.g. Pablo-generated).
        NoInstCombinePass
    };

    using SelectedOptimizationPasses = llvm::SmallVector<OptimizationPass, 6>;

    virtual void addOptimizationPasses(KernelBuilder & b, SelectedOptimizationPasses & passes) const;

protected:

    struct StateTypes {
        llvm::StructType * const Shared;
        llvm::StructType * const ThreadLocal;
        StateTypes(llvm::StructType * shared, llvm::StructType * threadLocal) : Shared(shared), ThreadLocal(threadLocal) {}
    };

    llvm::Function * getInitializeFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addInitializeDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * getExpectedOutputSizeFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addExpectedOutputSizeDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * getAllocateSharedInternalStreamSetsFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addAllocateSharedInternalStreamSetsDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * getInitializeThreadLocalFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addInitializeThreadLocalDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * getAllocateThreadLocalInternalStreamSetsFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addAllocateThreadLocalInternalStreamSetsDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    std::vector<llvm::Type *> getDoSegmentFields(KernelBuilder & b) const;

    llvm::Function * getDoSegmentFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addDoSegmentDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * getFinalizeThreadLocalFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addFinalizeThreadLocalDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * getFinalizeFunction(KernelBuilder & b, const bool alwayReturnDeclaration, const llvm::GlobalValue::LinkageTypes linkageType) const;

    llvm::Function * addFinalizeDeclaration(KernelBuilder & b, const llvm::GlobalValue::LinkageTypes linkageType) const;

protected:

    virtual bool hasInternallyGeneratedStreamSets() const { return false; }

    virtual const Relationships & getInternallyGeneratedStreamSets() const {
        llvm_unreachable("not supported");
    }

    using MetadataScaleVector = llvm::SmallVector<size_t, 8>;

    virtual void writeInternallyGeneratedStreamSetScaleVector(KernelBuilder & b, const Relationships & R, MetadataScaleVector & V, const size_t scale) const {
        llvm_unreachable("not supported");
    }

public:

    virtual llvm::Function * addOrDeclareMainFunction(KernelBuilder & b, const MainMethodGenerationType method) const;

protected:

    llvm::Value * constructFamilyKernels(KernelBuilder & b, InitArgs & hostArgs, ParamMap & params, NestedStateObjs & toFree) const;

    virtual void addAdditionalInitializationArgTypes(KernelBuilder & b, InitArgTypes & argTypes) const;

    virtual void recursivelyConstructFamilyKernels(KernelBuilder & b, InitArgs & args, ParamMap & params, NestedStateObjs & toFree) const;

    virtual void recursivelyListFamilyKernels(llvm::raw_ostream & familyName) const;

protected:

    llvm::Value * createInstance(KernelBuilder & b) const;

    llvm::Value * finalizeInstance(KernelBuilder & b, llvm::ArrayRef<llvm::Value *> args) const;

    llvm::Value * initializeThreadLocalInstance(KernelBuilder & b, llvm::ArrayRef<llvm::Value *> args) const;

    void finalizeThreadLocalInstance(KernelBuilder & b, llvm::ArrayRef<llvm::Value *> args) const;

public:

    static std::string getStringHash(const llvm::StringRef str);

protected:

    LLVM_READNONE bool hasFixedRateIO() const;

    virtual void addInternalProperties(KernelBuilder &) { }

    virtual void addAdditionalFunctions(KernelBuilder &) { }

    void constructStateTypes(KernelBuilder & b);

    virtual void generateInitializeMethod(KernelBuilder &) { }

    virtual llvm::Value * generateExpectedOutputSizeMethod(KernelBuilder &);

    virtual void generateInitializeThreadLocalMethod(KernelBuilder &) { }

    virtual void generateAllocateSharedInternalStreamSetsMethod(KernelBuilder & b, llvm::Value * expectedNumOfStrides);

    virtual void generateAllocateThreadLocalInternalStreamSetsMethod(KernelBuilder & b, llvm::Value * expectedNumOfStrides);

    virtual void generateKernelMethod(KernelBuilder &, llvm::TargetMachine *) = 0;

    virtual void generateFinalizeThreadLocalMethod(KernelBuilder &) { }

    virtual void generateFinalizeMethod(KernelBuilder &) { }

    void setKernelFlags(unsigned flags) { mFlags = flags; };

public:

    void addSymbols(llvm::orc::MangleAndInterner &mangler, llvm::orc::SymbolLookupSet & symbols) const;

    static const llvm::MDString * readSignatureFromModule(const llvm::Module * const M);

    static void writeSignatureToModule(const Kernel * const kernel, llvm::Module * const M);

public:

    virtual void linkExternalMethods(KernelBuilder & b);

protected:

    // Constructor
    Kernel(LLVMTypeSystemInterface & ts,
           const TypeId typeId, std::string && kernelName,
           Bindings &&stream_inputs, Bindings &&stream_outputs,
           Bindings &&scalar_inputs, Bindings &&scalar_outputs,
           InternalScalars && internal_scalars,
           unsigned flags = 0);

    // Constructor used by pipeline
    Kernel(LLVMTypeSystemInterface & ts,
           const TypeId typeId,
           AttributeSet && attributes,
           Bindings &&stream_inputs, Bindings &&stream_outputs,
           Bindings &&scalar_inputs, Bindings &&scalar_outputs,
           unsigned flags = 0);

    static std::string annotateKernelNameWithDebugFlags(const TypeId id, const unsigned flags, std::string && name);

    // Names of the two internal scalars every kernel unconditionally reserves at
    // construction time (see addBaseInternalScalars below): a per-output owned-buffer
    // handle and a termination-signal flag. Shared between kernel.cpp (which adds them)
    // and kernel_compiler.cpp (which looks them up), so both sides can never drift out of
    // sync on the name. Scoped as class members, not namespace-scope constants, so they
    // can't collide with an unrelated identifier of the same name declared unscoped
    // elsewhere in namespace kernel (as happened with an enumerator also named
    // TERMINATION_SIGNAL in multithreading_model_logic.cpp).
    static constexpr auto BUFFER_HANDLE_SUFFIX = "_buffer";
    static constexpr auto TERMINATION_SIGNAL = "__termination_signal";

    // Unconditionally reserves the buffer-handle (per output) and termination-signal
    // internal scalars at construction time, rather than at pipeline-compile time (the
    // old KernelCompiler::addBaseInternalProperties). This makes a kernel's scalar
    // composition fixed the moment it's constructed, before any pipeline-specific
    // buffer-layout analysis (which mutates output Binding attributes, e.g. promoting a
    // buffer to Shared/Managed) can change what used to be a conditional field count --
    // two structurally-identical kernel instances used in different, independently-
    // analyzed pipelines now always agree on scalar composition, which the driver's
    // kernel-declaration dedup (orc_jit_backend.cpp's materializeDecl) requires.
    //
    // The buffer handle field is only a pointer here, not the buffer's actual handle
    // struct: that struct's LLVM type depends on which concrete StreamSetBuffer subclass
    // the pipeline later picks (ExternalBuffer vs. ManagedDynamicBuffer, etc; see
    // KernelCompiler::constructStreamSetBuffers), which isn't known until then. The real
    // struct is heap-allocated once, lazily, the first time the kernel runs; see
    // KernelCompiler::allocateOwnedBufferHandleStorage.
    void addBaseInternalScalars(LLVMTypeSystemInterface & ts);

    struct FunctionLink {
        const std::string           UnmanagedName;
        llvm::FunctionType * const  FuncType;
        void * const                FuncPointer;

        FunctionLink(llvm::StringRef unmanagedName, llvm::FunctionType * funcType, void * funcPtr)
        : UnmanagedName(unmanagedName.str())
        , FuncType(funcType)
        , FuncPointer(funcPtr) {

        }
    };

    void addFunctionLink(llvm::StringRef unmangledName, llvm::FunctionType * functionType, void * functionPointer) {
        mPendingFunctionLinks.emplace_back(unmangledName, functionType, functionPointer);
    }

protected:

    const TypeId                        mTypeId;
    unsigned                            mStride;
    unsigned                            mFlags;
    Bindings                            mInputStreamSets;
    Bindings                            mOutputStreamSets;
    Bindings                            mInputScalars;
    Bindings                            mOutputScalars;
    InternalScalars                     mInternalScalars;
    // Parallel to mInputScalars/mOutputScalars (Binding carries no kernel-state-layout
    // bookkeeping of its own, unlike InternalScalar); recorded once by
    // constructStateTypes. See InternalScalar::getFieldIndex.
    std::vector<unsigned>               mInputScalarFieldIndex;
    std::vector<unsigned>               mOutputScalarFieldIndex;
    std::string                         mKernelName;
    llvm::StructType *                  mSharedStateType = nullptr;
    llvm::StructType *                  mThreadLocalStateType = nullptr;
    std::unique_ptr<KernelCompiler>     mCompiler;
    llvm::SmallVector<FunctionLink, 0>  mPendingFunctionLinks;
};

class SegmentOrientedKernel : public Kernel {
public:

    static bool classof(const Kernel * const k) {
        return k->getTypeId() == TypeId::SegmentOriented;
    }

    static bool classof(const void *) { return false; }

protected:

    SegmentOrientedKernel(LLVMTypeSystemInterface & ts,
                          std::string && kernelName,
                          Bindings &&stream_inputs,
                          Bindings &&stream_outputs,
                          Bindings &&scalar_parameters,
                          Bindings &&scalar_outputs,
                          InternalScalars && internal_scalars,
                          unsigned flags = 0);
public:

    virtual void generateDoSegmentMethod(KernelBuilder & b) = 0;

protected:

    void generateKernelMethod(KernelBuilder & b, llvm::TargetMachine * TM) final;

};

class MultiBlockKernel : public Kernel {
public:

    static bool classof(const Kernel * const k) {
        return k->getTypeId() == TypeId::MultiBlock;
    }

    static bool classof(const void *) { return false; }

protected:

    MultiBlockKernel(LLVMTypeSystemInterface & ts,
                     std::string && kernelName,
                     Bindings && stream_inputs,
                     Bindings && stream_outputs,
                     Bindings && scalar_parameters,
                     Bindings && scalar_outputs,
                     InternalScalars && internal_scalars,
                     unsigned flags = 0);

    MultiBlockKernel(LLVMTypeSystemInterface & ts,
                     const TypeId kernelTypId,
                     std::string && kernelName,
                     Bindings && stream_inputs,
                     Bindings && stream_outputs,
                     Bindings && scalar_parameters,
                     Bindings && scalar_outputs,
                     InternalScalars && internal_scalars,
                     unsigned flags = 0);

    virtual void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) = 0;

private:

    void generateKernelMethod(KernelBuilder & b, llvm::TargetMachine * TM) final;

};


class BlockOrientedKernel : public MultiBlockKernel {
    friend class BlockKernelCompiler;
public:

    static bool classof(const Kernel * const k) {
        return k->getTypeId() == TypeId::BlockOriented;
    }

    static bool classof(const void *) { return false; }

    std::unique_ptr<KernelCompiler> instantiateKernelCompiler(KernelBuilder & b) override;

    // Whether the stream I/O and attributes of this kernel permit the ProvisionalLookAheadStride
    // attribute; if not, reason (when given) describes the first requirement that is not met.
    bool meetsProvisionalLookAheadStrideRequirements(std::string * reason = nullptr) const;

protected:

    // Each BlockOrientedKernel must provide its own logic for generating
    // doBlock calls.
    virtual void generateDoBlockMethod(KernelBuilder & b) = 0;

    // Each BlockOrientedKernel must also specify the logic for processing the
    // final block of stream data, if there is any special processing required
    // beyond simply calling the doBlock function. In the case that the final block
    // processing may be trivially implemented by dispatching to the doBlock method
    // without additional preparation, the default generateFinalBlockMethod need
    // not be overridden.

    void RepeatDoBlockLogic(KernelBuilder & b);

    virtual void generateFinalBlockMethod(KernelBuilder & b, llvm::Value * remainingItems);

    // Adds the ProvisionalLookAheadStride attribute and records it in the kernel name,
    // since it changes the generated code.  Call from the subclass constructor.
    void setProvisionalLookAheadStride();

    BlockOrientedKernel(LLVMTypeSystemInterface & ts,
                        std::string && kernelName,
                        Bindings && stream_inputs,
                        Bindings && stream_outputs,
                        Bindings && scalar_parameters,
                        Bindings && scalar_outputs,
                        InternalScalars && internal_scalars,
                        const unsigned flags = 0);

private:

    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) final;

};

}


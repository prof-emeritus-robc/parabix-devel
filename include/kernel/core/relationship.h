#pragma once

#include <vector>
#include <memory>
#include <llvm/Support/Compiler.h>
#include <util/not_null.h>
#include <util/slab_allocator.h>

namespace llvm { class Constant; }
namespace llvm { class LLVMContext; }
namespace llvm { class Type; }

namespace kernel {

class Kernel;

// NOTE: Relationships themselves do not store producer/consumer information. When a PipelineKernel is compiled,
// it recalculates the data based on the existence of a relationship. The problem is that internally, a pipeline
// is considered to produce its inputs and consume its outputs whereas a kernel within a pipeline consumes its
// inputs and produces its outputs. However, a PipelineKernel would simply be another kernel if nested within
// another pipeline and it would become more difficult to reason about global buffer requirements if we
// considered them independently. Moreover, maintaining this information only adds additional bookkeeping work
// when the appropriate cached pipeline kernel already exists.

class Relationship {
    friend class Kernel;
    friend class PipelineKernel;
public:

    using Allocator = ProxyAllocator<Relationship>;

    static inline bool classof(const Relationship *) {
        return true;
    }
    static inline bool classof(const void *) {
        return false;
    }

    enum class ClassTypeId {
        // streamset types
        StreamSet
        , RepeatingStreamSet
        , TruncatedStreamSet
        // scalar types
        , Scalar
        , CommandLineScalar
        , ScalarConstant
        // -----------------
        , __Count
    };

    ClassTypeId getClassTypeId() const noexcept {
        return mClassTypeId;
    }

    llvm::Type * getType() const noexcept {
        return mType;
    }

    bool isScalar() const noexcept {
        return (mClassTypeId >= ClassTypeId::Scalar);
    }

    bool isStreamSet() const noexcept {
        return (mClassTypeId < ClassTypeId::Scalar);
    }

    void* operator new (std::size_t size, Allocator & A) noexcept {
        return A.allocate<uint8_t>(size);
    }

    bool isConstant() const {
        return mClassTypeId == ClassTypeId::ScalarConstant;
    }

protected:

    Relationship(const ClassTypeId typeId, llvm::Type * type) noexcept
    : mClassTypeId(typeId)
    , mType(type) {
    }

protected:
    const ClassTypeId   mClassTypeId;
    llvm::Type * const  mType;
};

class StreamSet : public Relationship {
public:

    static bool classof(const Relationship * e) {
        return e->getClassTypeId() == ClassTypeId::StreamSet;
    }
    static bool classof(const void *) {
        return false;
    }
    LLVM_READNONE unsigned getNumElements() const;

    LLVM_READNONE unsigned getFieldWidth() const;

    std::string shapeString();

    inline StreamSet(llvm::LLVMContext & C, const unsigned NumElements, const unsigned FieldWidth) noexcept
    : StreamSet(C, ClassTypeId::StreamSet, NumElements, FieldWidth) {

    }

protected:

    StreamSet(llvm::LLVMContext & C, const ClassTypeId typeId, const unsigned NumElements, const unsigned FieldWidth) noexcept;

};

class RepeatingStreamSet : public StreamSet {
public:
    static bool classof(const Relationship * e) {
        return e->getClassTypeId() == ClassTypeId::RepeatingStreamSet;
    }
    static bool classof(const void *) {
        return false;
    }

    const std::vector<uint64_t> & getPattern(const unsigned elementIndex = 0) const {
        return _StringSet[elementIndex];
    }

    bool isDynamic() const {
        return _isDynamic;
    }

    bool isUnaligned() const {
        return _isUnaligned;
    }

    RepeatingStreamSet(llvm::LLVMContext & C, const unsigned FieldWidth, std::vector<std::vector<uint64_t>> stringSet, bool isDynamic, bool isUnaligned) noexcept
    : StreamSet(C, ClassTypeId::RepeatingStreamSet, stringSet.size(), FieldWidth)
    , _isDynamic(isDynamic)
    , _isUnaligned(isUnaligned)
    , _StringSet(std::move(stringSet)) {

    }

private:

    const bool _isDynamic;
    const bool _isUnaligned;
    std::vector<std::vector<uint64_t>> _StringSet;

};

class TruncatedStreamSet : public StreamSet {
public:
    static bool classof(const Relationship * e) {
        return e->getClassTypeId() == ClassTypeId::TruncatedStreamSet;
    }
    static bool classof(const void *) {
        return false;
    }

    const StreamSet * getData() const {
        return _Data;
    }

    TruncatedStreamSet(llvm::LLVMContext & C, const StreamSet * const data) noexcept
    : StreamSet(C, ClassTypeId::TruncatedStreamSet, data->getNumElements(), data->getFieldWidth())
    , _Data(data) {

    }

private:

    const StreamSet * const _Data;

};

using StreamSets = std::vector<StreamSet *>;

class Scalar : public Relationship {
public:
    static bool classof(const Relationship * e) { assert (e);
        return e->getClassTypeId() == ClassTypeId::Scalar;
    }
    static bool classof(const void *) {
        return false;
    }
    unsigned getFieldWidth() const;
    Scalar(not_null<llvm::Type *> type) noexcept;
protected:
    Scalar(const ClassTypeId typeId, llvm::Type *type) noexcept;
};

enum class CommandLineScalarType {
    MinThreadCount
    , MaxThreadCount
    , DynamicMultithreadingPeriod
    , DynamicMultithreadingAddSynchronizationThreshold
    , DynamicMultithreadingRemoveSynchronizationThreshold
    , ParabixIllustratorObject
    , BufferSegmentLength
    #ifdef ENABLE_PAPI
    , PAPIEventCount
    , PAPIEventList
    #endif
    // --------------------
    , CommandLineScalarCount
};

// Command scalars are intended to be internal values that pipeline main function passes
// to the outermost pipeline kernel but is matched by its subtype rather than pointer/object
// equivalence
class CommandLineScalar : public Scalar {
public:

    static bool classof(const Relationship * e) {
        return e->getClassTypeId() == ClassTypeId::CommandLineScalar;
    }
    static bool classof(const void *) {
        return false;
    }

    CommandLineScalarType getCLType() const {
        return mCLType;
    }

    CommandLineScalar(const CommandLineScalarType clType, llvm::Type *type) noexcept;
private:
    const CommandLineScalarType mCLType;
};


class ScalarConstant : public Scalar {
public:
    static bool classof(const Relationship * e) {
        return e->getClassTypeId() == ClassTypeId::ScalarConstant;
    }
    static bool classof(const void *) {
        return false;
    }
    llvm::Constant * value(llvm::LLVMContext & C) const;

    ScalarConstant(not_null<llvm::Constant *> constant) noexcept;
private:
    llvm::Constant * const mConstant;
};

}


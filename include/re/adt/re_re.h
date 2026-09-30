/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <vector>
#include <allocator/threadsafe_slaballocator.h>

namespace re {

// REs are defined in a class hierarchy supporting llvm::isa, llvm::dyn_cast.
#define RE_SUBTYPE(kind) \
static inline bool classof(const RE * re) {return re->getClassTypeId() == ClassTypeId::kind;}\
static inline bool classof(const void *) {return false;}

class RE : public SlabAllocatedObject {
public:
    enum class ClassTypeId : unsigned {
        Alt
        , Any
        , Assertion
        , CC
        , Range
        , Diff
        , End
        , Intersect
        , Name
        , PropertyExpression
        , Capture
        , Reference
        , Group
        , Rep
        , Seq
        , Start
        , Permute
        , Interleavable
    };
    inline ClassTypeId getClassTypeId() const {
        return mClassTypeId;
    }
    typedef std::initializer_list<RE *> InitializerList;

protected:

    USE_SLAB_ALLOCATED_OBJECT_MEMORY_OPERATORS

    inline RE(const ClassTypeId id)
    : mClassTypeId(id) {

    }
    const ClassTypeId mClassTypeId;
    using length_t = std::string::size_type;
    inline const char * replicateString(const char * string, const length_t length) {
        if (string && (length > 0)) {
            char * allocated = ThreadSafeSlabAllocator::allocate_array_of<char>(length);
            std::memcpy(allocated, string, length);
            return allocated;
        }
        return nullptr;
    }
};

// Does the RE match the empty string, considering that ^ and $ each
// do match an empty string.
bool matchesEmptyString(const RE * re);

[[noreturn]] void UnsupportedRE(const std::string & errmsg);

// The name of an RE class, e.g., "Seq".
const char * getClassTypeName(RE::ClassTypeId t);

// Report a fatal error for an RE whose type the named routine does not handle.
[[noreturn]] void UnexpectedRE(const char * routine, const RE * re);

}


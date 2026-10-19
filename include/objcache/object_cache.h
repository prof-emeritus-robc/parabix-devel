/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <llvm/ADT/SmallString.h>
#include <llvm/ExecutionEngine/ObjectCache.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/ADT/StringRef.h>
#include <boost/container/flat_set.hpp>
#include <boost/container/flat_map.hpp>
#include <util/not_null.h>
#include <kernel/pipeline/driver/driver.h>
#include <mutex>
#include <string>
namespace kernel { class Kernel; }

namespace llvm { 
    class Module;  class MemoryBuffer;  class LLVMContext;
}

// The ParabixObjectCache is a two-level cache compatible with the requirements
// of the LLVM ExecutionEngine as well as the Parabix Kernel builder infrastructure.
//
// The ParabixObjectCache allows the CPUEngineInstance to look up cached modules based on a
// module stub that contains only the necessary Module ID and signature (loadCachedObjectFile).
// If found, the module object file is immediately loaded into the cachedObjectMap,
// and later made available to the ExecutionEngine as needed.  Otherwise, false is
// return to signal that a cached File is not found.  The CPUEngineInstance can then
// apply the necessary kernel builder to build the full module IR before passing
// it to the ExecutionEngine.
//

enum class CacheObjectResult {
    CACHED
    , COMPILED
    , UNCACHED
};

class ParabixObjectCache {
    template <typename K, typename V>
    using Map = boost::container::flat_map<K, V>;
    template <typename K>
    using Set = boost::container::flat_set<K>;
    using ObjectBufferCache = llvm::StringMap<llvm::MemoryBufferRef>;
    using Instance = std::unique_ptr<ParabixObjectCache>;
    using LoadResult = std::pair<std::unique_ptr<llvm::MemoryBuffer>, std::unique_ptr<llvm::Module>>;
public:

    friend class BaseDriver;

    using Path = llvm::SmallString<128>;

    LoadResult loadCachedObjectFile(kernel::KernelBuilder & builder, kernel::Kernel * kernel) noexcept;

    // -VerifyCache: on a cache hit the driver regenerates the kernel's IR anyway and
    // compares a hash of it with the one recorded in the .kernel file when the entry
    // was written.  The entry is read in a private LLVMContext, so that none of its
    // types (in particular the kernel's state types, which declareStateTypes would
    // otherwise find by name and reuse) leak into the regenerated IR.
    struct VerificationEntry {
        std::unique_ptr<llvm::MemoryBuffer> Object; // null if there is no usable entry
        std::string IRHash;                         // empty if the entry records none
        std::string WrittenBy;                      // PARABIX_VERSION of the writer
        std::string FileName;
    };

    VerificationEntry loadCachedObjectFileForVerification(kernel::KernelBuilder & builder, kernel::Kernel * kernel) noexcept;

    // Records the hash of the IR just generated for a kernel in M (before any
    // optimization), so that saveCachedObjectFile stores it in the .kernel file.
    static void recordIRHash(llvm::Module & M);

    // Compares the hash of M, the IR just regenerated for KERNEL, with ENTRY's.
    // Returns true if they match, false if ENTRY records no hash (the kernel must
    // then be compiled anew); a mismatch is a fatal error explaining how to find
    // the dependency missing from the kernel's cache key.
    bool verifyIRHash(const kernel::Kernel * kernel, const llvm::Module & M, const VerificationEntry & entry);

    void saveCachedObjectFile(const llvm::Module & M, llvm::MemoryBufferRef Obj) noexcept;

protected:

    ParabixObjectCache();
    void loadCacheSettings() noexcept;
    void saveCacheSettings() noexcept;

private:
    LoadResult lookupCachedObjectFile(kernel::KernelBuilder & builder, kernel::Kernel * kernel, llvm::LLVMContext & C) noexcept;
    void initiateCacheCleanUp() noexcept;
    bool requiresCacheCleanUp() noexcept;
    bool cachejanitordAppearsAlive() noexcept;
private:
    static bool         mStartedCacheCleanupDaemon;
    ObjectBufferCache   mCachedObject;
    Path                mCachePath;
    // CPUDriverCompiler's worker threads call loadCachedObjectFile/saveCachedObjectFile
    // concurrently for different kernels; mCachedObject (an llvm::StringMap) and the
    // on-disk .o/.kernel files it mirrors are not otherwise safe against that.
    std::mutex          mCacheMutex;
};


/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <llvm/ADT/SmallString.h>
#include <llvm/ExecutionEngine/ObjectCache.h>
#include <llvm/ADT/StringRef.h>
#include <boost/container/flat_set.hpp>
#include <boost/container/flat_map.hpp>
#include <util/not_null.h>
#include <kernel/core/kernel.h>
#include <string>

namespace llvm { 
    class Module;  class MemoryBuffer;  class MemoryBufferRef;  class LLVMContext;
    namespace orc {class TMOwningSimpleCompiler;}
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

class ParabixObjectCache final : public llvm::ObjectCache {
    template <typename K, typename V>
    using Map = boost::container::flat_map<K, V>;
    template <typename K>
    using Set = boost::container::flat_set<K>;
    using KnownSignatures = Map<std::string, llvm::Module *>;
    using ModuleCache = Map<std::string, std::unique_ptr<llvm::MemoryBuffer>>;
    using Instance = std::unique_ptr<ParabixObjectCache>;
public:

    friend class BaseDriver;
    friend class llvm::orc::TMOwningSimpleCompiler;

    using Path = llvm::SmallString<128>;

    CacheObjectResult loadCachedObjectFile(kernel::KernelBuilder & b, kernel::Kernel * const kernel) noexcept;

    // Overridden methods used internally by orc::TMOwningSimpleCompiler during JIT pipeline passes
    void notifyObjectCompiled(const llvm::Module * M, llvm::MemoryBufferRef Obj) override;

    std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module * M) override;

    virtual ~ParabixObjectCache();

protected:

    ParabixObjectCache();
    void loadCacheSettings() noexcept;
    void saveCacheSettings() noexcept;

private:
    void initiateCacheCleanUp() noexcept;
    bool requiresCacheCleanUp() noexcept;
private:
    static bool         mStartedCacheCleanupDaemon;
    KnownSignatures     mKnownSignatures;
    ModuleCache         mCachedObject;
    Path                mCachePath;
};


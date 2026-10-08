#include <objcache/object_cache.h>

#include <objcache/object_cache_util.hpp>
#include <objcache/build_identity.h>
#include <kernel/core/kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/driver/driver.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/Module.h>
#include <toolchain/toolchain.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <system_error>
#include <csignal>

using namespace llvm;
using namespace boost;



using Path = ParabixObjectCache::Path;

bool ParabixObjectCache::mStartedCacheCleanupDaemon = false;

//===----------------------------------------------------------------------===//
// Object cache (based on tools/lli/lli.cpp, LLVM 3.6.1)
//
// This object cache implementation writes cached objects to disk to the
// directory specified by CacheDir, using a filename provided in the module
// descriptor. The cache tries to load a saved object using that path if the
// file exists.
//

// Cached kernels are keyed by the identity of the running code (the build IDs of
// the executable and the Parabix libraries), so that rebuilding any of them
// invalidates the cache automatically.  An --object-cache-salt adds a further
// component, giving experiments their own entries.
static const std::string & cachePrefix() {
    static const std::string prefix = [] {
        std::string p = "parabix-" + parabix::getBuildIdentity() + "_";
        if (!codegen::ObjectCacheSalt.empty()) {
            p += "salt-" + codegen::ObjectCacheSalt + "_";
        }
        if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
            errs() << "Object cache prefix: " << p << "\n";
        }
        return p;
    }();
    return prefix;
}

const static auto SIGNATURE = "signature";

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getSignature
 ** ------------------------------------------------------------------------------------------------------------- */
const MDString * getSignature(const llvm::Module * const M) {
    NamedMDNode * const sig = M->getNamedMetadata(SIGNATURE);
    if (sig) {
        assert ("empty metadata node" && sig->getNumOperands() > 0);
        assert ("metadata should contain precisely one node" && sig->getNumOperands() == 1);
        assert ("no signature payload" && sig->getOperand(0)->getNumOperands() == 1);
        return cast<MDString>(sig->getOperand(0)->getOperand(0));
    }
    return nullptr;
}

inline bool isNonMatchingSignature(const MDString * const received, const StringRef expected) {
    return expected.compare(received->getString()) != 0;
}

// Each .kernel file records the --optimization-level and --backend-optimization-level
// its object was compiled at, as a single node holding the two levels as integers.
// The levels are not part of the cache key: one entry per kernel is kept, and it is
// replaced only when a run asks for a higher level than the entry was compiled at.
const static auto OPT_LEVELS = "parabix.opt-levels";

using OptLevels = std::pair<unsigned, unsigned>;

static OptLevels requestedOptLevels() {
    return OptLevels{static_cast<unsigned>(codegen::OptLevel), static_cast<unsigned>(codegen::BackEndOptLevel)};
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief readOptLevels
 *
 * Entries written before the levels were recorded have no node. They are read as none/none,
 * so any run that requests a higher level recompiles and replaces them.
 ** ------------------------------------------------------------------------------------------------------------- */
static OptLevels readOptLevels(const Module * const M) {
    OptLevels levels{static_cast<unsigned>(CodeGenOptLevel::None), static_cast<unsigned>(CodeGenOptLevel::None)};
    const NamedMDNode * const md = M->getNamedMetadata(OPT_LEVELS);
    if (md && md->getNumOperands() == 1 && md->getOperand(0)->getNumOperands() == 2) {
        const MDNode * const node = md->getOperand(0);
        const auto front = mdconst::dyn_extract<ConstantInt>(node->getOperand(0));
        const auto back = mdconst::dyn_extract<ConstantInt>(node->getOperand(1));
        if (front && back) {
            levels = OptLevels{static_cast<unsigned>(front->getZExtValue()), static_cast<unsigned>(back->getZExtValue())};
        }
    }
    return levels;
}

static void writeOptLevels(Module * const M, const OptLevels levels) {
    LLVMContext & C = M->getContext();
    IntegerType * const int32Ty = Type::getInt32Ty(C);
    Metadata * const ops[2] = {ConstantAsMetadata::get(ConstantInt::get(int32Ty, levels.first)),
                               ConstantAsMetadata::get(ConstantInt::get(int32Ty, levels.second))};
    NamedMDNode * const md = M->getOrInsertNamedMetadata(OPT_LEVELS);
    md->clearOperands();
    md->addOperand(MDNode::get(C, ops));
}

static const char * optLevelName(const unsigned level) {
    switch (static_cast<CodeGenOptLevel>(level)) {
        case CodeGenOptLevel::None: return "none";
        case CodeGenOptLevel::Less: return "less";
        case CodeGenOptLevel::Default: return "standard";
        case CodeGenOptLevel::Aggressive: return "aggressive";
    }
    return "unknown";
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief loadCachedObjectFile
 ** ------------------------------------------------------------------------------------------------------------- */
ParabixObjectCache::LoadResult ParabixObjectCache::loadCachedObjectFile(kernel::KernelBuilder & builder, kernel::Kernel * kernel) noexcept {

    // Have we already seen this signature before? if so, we can safely assume that the ExecutionEngine
    // will have a compiled module for this kernel when we execute the pipeline.

    std::lock_guard<std::mutex> L(mCacheMutex);

    if (LLVM_UNLIKELY(codegen::UpdateObjectCache)) {
        if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
            errs() << "Forcing recompilation (--update-object-cache): " << kernel->makeCacheName(builder)
                   << KERNEL_FILE_EXTENSION << "\n";
        }
        return LoadResult{nullptr, nullptr};
    }

    Path fileName(mCachePath);
    sys::path::append(fileName, cachePrefix());
    const auto moduleId = kernel->makeCacheName(builder);
    fileName.append(moduleId);
    fileName.append(KERNEL_FILE_EXTENSION);
    auto kernelBuffer = MemoryBuffer::getFile(fileName, false, false, false);
    if (kernelBuffer) {
        auto loadedFile = parseBitcodeFile((*kernelBuffer)->getMemBufferRef(), builder.getContext());
        if (LLVM_LIKELY(loadedFile)) {

            std::unique_ptr<Module> H{std::move(*loadedFile)};

            if (LLVM_UNLIKELY(kernel->hasSignature())) {
                const MDString * const sig = kernel::Kernel::readSignatureFromModule(H.get());
                assert ("signature is missing from kernel file: possible module naming conflict or change in the LLVM metadata storage policy?" && sig);
                if (LLVM_UNLIKELY(isNonMatchingSignature(sig, kernel->getSignature()))) {
                    if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
                        errs() << "Mismatched signature in cache file: " << moduleId << KERNEL_FILE_EXTENSION << "\n"
                                  "Expected: " << kernel->getSignature() << "\n"
                                  "Loaded:   " << sig->getString() << "\n";
                    }
                    return LoadResult{nullptr, nullptr};
                }
            }
            // An entry compiled at the requested levels or higher (in both the front end
            // and the back end) is used as is. Otherwise the kernel is recompiled at the
            // requested levels and saveCachedObjectFile replaces the entry.
            const auto cachedLevels = readOptLevels(H.get());
            const auto requested = requestedOptLevels();
            if (LLVM_UNLIKELY(cachedLevels.first < requested.first || cachedLevels.second < requested.second)) {
                if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
                    errs() << "Recompiling at a higher optimization level: " << moduleId << KERNEL_FILE_EXTENSION
                           << " (cached " << optLevelName(cachedLevels.first) << "/" << optLevelName(cachedLevels.second)
                           << ", requested " << optLevelName(requested.first) << "/" << optLevelName(requested.second) << ")\n";
                }
                return LoadResult{nullptr, nullptr};
            }
            sys::path::replace_extension(fileName, OBJECT_FILE_EXTENSION);
            auto objectBuffer = MemoryBuffer::getFile(fileName.c_str(), false, false, false);
            if (LLVM_LIKELY(objectBuffer)) {

                // defaults to <path>/<moduleId>.kernel
                auto obj = std::move(*objectBuffer);

                // update the modified time of the .o and .kernel files
                const auto access_time = currentTime();
                fs::last_write_time(fileName.c_str(), access_time);
                sys::path::replace_extension(fileName, KERNEL_FILE_EXTENSION);
                fs::last_write_time(fileName.c_str(), access_time);

                if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
                    errs() << "Read cache file: " << moduleId << KERNEL_FILE_EXTENSION
                           << " (" << optLevelName(cachedLevels.first) << "/" << optLevelName(cachedLevels.second) << ")\n";
                }

                return std::make_pair(std::move(obj), std::move(H));
            }
        } else if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
            errs() << "Failed to load cache file: " << moduleId << KERNEL_FILE_EXTENSION << "\n";
        }
    }
    return LoadResult{nullptr, nullptr};
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeCacheFileAtomically
 *
 * Writes to a uniquely-named temporary file in the same directory as finalPath, then renames it into
 * place. saveCachedObjectFile can run concurrently in unrelated processes that share this cache
 * directory (e.g. parallel test targets), and rename() on the same filesystem is atomic: a concurrent
 * loadCachedObjectFile() in another process only ever sees the old (or absent) file or the fully-written
 * new one, never a partially-written one. Writing straight to finalPath let a reader observe a torn
 * object file mid-write (e.g. "section header table goes past the end of the file", or missing symbols).
 ** ------------------------------------------------------------------------------------------------------------- */
static void writeCacheFileAtomically(const ParabixObjectCache::Path & finalPath, llvm::function_ref<void(raw_fd_ostream &)> write) {
    SmallString<256> tempPath;
    int tempFD;
    std::error_code EC = sys::fs::createUniqueFile(Twine(finalPath) + ".tmp-%%%%%%", tempFD, tempPath);
    if (LLVM_UNLIKELY(EC)) {
        SmallVector<char, 512> tmp;
        llvm::raw_svector_ostream msg(tmp);
        msg << "Could not create a temporary file for \""
            << finalPath.str()
            << "\" in object cache directory.\n\n"
            "Reason: " << EC.message() << "\n\n"
            "Rerun " << codegen::ProgramName << " with --enable-object-cache=0";
        report_fatal_error(Twine(msg.str()));
    }
    {
        raw_fd_ostream out(tempFD, true);
        write(out);
    }
    EC = sys::fs::rename(tempPath, finalPath);
    if (LLVM_UNLIKELY(EC)) {
        sys::fs::remove(tempPath);
        SmallVector<char, 512> tmp;
        llvm::raw_svector_ostream msg(tmp);
        msg << "Could not finalize \""
            << finalPath.str()
            << "\" in object cache directory.\n\n"
            "Reason: " << EC.message() << "\n\n"
            "Rerun " << codegen::ProgramName << " with --enable-object-cache=0";
        report_fatal_error(Twine(msg.str()));
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief saveCachedObjectFile
 *
 * A new module has been compiled. If it is cacheable and no conflicting module exists, write it out.
 ** ------------------------------------------------------------------------------------------------------------- */
void ParabixObjectCache::saveCachedObjectFile(const Module & M, llvm::MemoryBufferRef Obj) noexcept {

    std::lock_guard<std::mutex> L(mCacheMutex);

    auto moduleId = M.getModuleIdentifier();

    // Store back into the memory buffer cache system
    mCachedObject[moduleId] = Obj;

    Path objectName(mCachePath);
    sys::path::append(objectName, cachePrefix());
    objectName.append(moduleId);
    objectName.append(OBJECT_FILE_EXTENSION);

    writeCacheFileAtomically(objectName, [&](raw_fd_ostream & out) {
        out.write(Obj.getBufferStart(), Obj.getBufferSize());
    });

    sys::path::replace_extension(objectName, KERNEL_FILE_EXTENSION);

    // Clone the function prototypes and metadata to minimize the size of the stored .kernel file.
    std::unique_ptr<Module> H(new Module(moduleId, M.getContext()));
    H->setTargetTriple(M.getTargetTriple());
    H->setDataLayout(M.getDataLayout());
//    for (const Function & f : M.getFunctionList()) {
//        if (f.hasExternalLinkage() && !f.empty()) {
//            Function::Create(f.getFunctionType(), Function::ExternalLinkage, f.getName(), H.get());
//        }
//    }
    for (const auto & og : M.named_metadata()) {
        NamedMDNode * const md = H->getOrInsertNamedMetadata(og.getName());
        const auto n = og.getNumOperands();
        for (unsigned i = 0; i < n; ++i) {
            md->addOperand(og.getOperand(i));
        }
    }
    writeOptLevels(H.get(), requestedOptLevels());

    writeCacheFileAtomically(objectName, [&](raw_fd_ostream & out) {
        WriteBitcodeToFile(*H, out);
    });

    if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
        const auto levels = requestedOptLevels();
        errs() << "Wrote cache file: " << moduleId << KERNEL_FILE_EXTENSION
               << " (" << optLevelName(levels.first) << "/" << optLevelName(levels.second) << ")\n";
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief cachejanitordAppearsAlive
 *
 * A cheap, best-effort check for whether a cachejanitord is already running, to avoid
 * needlessly forking+exec'ing+daemonizing a redundant one. This is NOT airtight: two
 * processes launched close enough together can still both pass it before either's
 * daemon has actually relocked the pid file (see requiresCacheCleanUp's own fcntl-based
 * check, which is race-prone the same way -- the daemon's own internal lock is the only
 * thing that actually guarantees at most one janitor ever runs its cleanup loop). But
 * since ParabixObjectCache's constructor runs once per process, and this repo's test
 * suite launches many short-lived tool invocations in quick succession, this closes the
 * overwhelmingly common case: a prior janitor from an earlier invocation is still alive,
 * so nothing needs to be spawned at all.
 ** ------------------------------------------------------------------------------------------------------------- */
inline bool ParabixObjectCache::cachejanitordAppearsAlive() noexcept {
    std::ifstream in((fs::path{mCachePath.c_str()} / DAEMON_FILE).string());
    if (!in) return false;
    pid_t pid = 0;
    in >> pid;
    if (pid <= 0) return false;
    return kill(pid, 0) == 0;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief requiresCacheCleanUp
 ** ------------------------------------------------------------------------------------------------------------- */
inline bool ParabixObjectCache::requiresCacheCleanUp() noexcept {
    if (LLVM_UNLIKELY(mStartedCacheCleanupDaemon)) {
        return false;
    }
    if (cachejanitordAppearsAlive()) {
        return false;
    }
    // if we cannot lock the pid file then an earlier process
    // must have acquired it.
    return FileLock{fs::path{mCachePath.c_str()}}.locked();
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief initiateCacheCleanUp
 ** ------------------------------------------------------------------------------------------------------------- */
void ParabixObjectCache::initiateCacheCleanUp() noexcept {
    if (LLVM_UNLIKELY(requiresCacheCleanUp())) {
        mStartedCacheCleanupDaemon = true;
        const auto pid = fork();
        if (pid == 0) {
            char * const cachePath = const_cast<char *>(mCachePath.c_str());
            char * args[3] = {const_cast<char *>(CACHE_JANITOR_FILE_NAME), cachePath, nullptr};
            Path janitorFileName(codegen::ProgramName);
            sys::path::remove_filename(janitorFileName);
            sys::path::append(janitorFileName, CACHE_JANITOR_FILE_NAME);
            char * const janitorPath = const_cast<char *>(janitorFileName.c_str());
            if (execvp(janitorPath, args) < 0) {
                #ifndef NDEBUG
                SmallVector<char, 1024> tmp;
                raw_svector_ostream out(tmp);
                out << "failed to exec cache cleanup deamon \"" << janitorPath << "\"";
                perror(reinterpret_cast<const char *>(out.str().bytes().begin()));
                #endif
                exit(errno);
            }
        } else if (pid < 0) {
            #ifndef NDEBUG
            perror("failed to fork cache cleanup deamon process");
            #endif
        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getDefaultCachePath
 ** ------------------------------------------------------------------------------------------------------------- */
inline void getDefaultCachePath(Path & configPath) {
#ifdef PARABIX_OBJECT_CACHE
    configPath = PARABIX_OBJECT_CACHE;
#else
    // default: $HOME/.parabix/cache
    sys::path::home_directory(configPath);
    sys::path::append(configPath, ".parabix", "cache");
#endif
}

#if 0
/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getConfigPath
 ** ------------------------------------------------------------------------------------------------------------- */
inline Path getConfigPath() {
    // $HOME/.config/parabix/cache.cfg
    Path configPath;
    sys::path::home_directory(configPath);
    sys::path::append(configPath, ".config", "parabix");
    sys::fs::create_directories(configPath);
    sys::path::append(configPath, "cache.cfg");
    return configPath;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief loadCacheSettings
 ** ------------------------------------------------------------------------------------------------------------- */
inline size_t parseInt(const StringRef & str, const StringRef & label) {
    try {
        return lexical_cast<size_t>(str.data(), str.size());
    } catch(const bad_lexical_cast &) {
        errs() << "configuration for " << label << " must be an integer";
        exit(-1);
    }
}

#endif

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief loadCacheSettings
 ** ------------------------------------------------------------------------------------------------------------- */
inline void ParabixObjectCache::loadCacheSettings() noexcept {
    if (codegen::ObjectCacheDir) {
        mCachePath.assign(codegen::ObjectCacheDir);
        // the cache cleanup daemon is handed this path; resolve it now
        sys::fs::make_absolute(mCachePath);
    } else {
        getDefaultCachePath(mCachePath);
    }
    #if 0

    const auto configPath = getConfigPath();
    auto configFile = MemoryBuffer::getFile(configPath);

    // default: $HOME/.cache/parabix/
    sys::path::home_directory(mCachePath);
    sys::path::append(mCachePath, ".cache", "parabix");
    // default: 1 week
    mCacheExpirationDelay = CACHE_ENTRY_EXPIRY_PERIOD;

    if (LLVM_UNLIKELY(!!configFile)) {
        const StringRef config = (*configFile)->getBuffer();
        #define ASCII_WHITESPACE " \f\n\r\t\v"
        #define ASCII_WHITESPACE_OR_EQUALS (ASCII_WHITESPACE "+")
        size_t nameStart = 0;
        for (;;) {

            const auto nameEnd = config.find_first_of(ASCII_WHITESPACE_OR_EQUALS, nameStart);
            if (nameEnd == StringRef::npos) break;
            const auto afterEquals = config.find_first_of('=', nameEnd) + 1;
            if (LLVM_UNLIKELY(afterEquals == StringRef::npos)) break;
            const auto valueStart = config.find_first_not_of(ASCII_WHITESPACE, afterEquals);
            if (LLVM_UNLIKELY(valueStart == StringRef::npos)) break;
            const auto valueEnd = config.find_first_of(ASCII_WHITESPACE, valueStart);
            if (LLVM_UNLIKELY(valueEnd == StringRef::npos)) break;
            const auto name = config.slice(nameStart, nameEnd);
            const auto value = config.slice(valueStart, valueEnd);

            if (name.equals_lower("cachepath")) {
                mCachePath.assign(value);
            } else if (name.equals_lower("cachedayslimit")) {
                mCacheExpirationDelay = parseInt(value, "cachedayslimit");
            }
            // get the next name start
            nameStart = config.find_first_not_of(ASCII_WHITESPACE, valueEnd + 1);
        }
    }
    #endif

    const auto err = sys::fs::create_directories(mCachePath, true);

    if (LLVM_UNLIKELY(err)) {
        std::string tmp;
        llvm::raw_string_ostream msg(tmp);
        msg << "Could not create object cache directory \""
            << mCachePath.str() << "\" with read/write permissions.\n\n"
            "Reason: " << err.message() << "\n\n"
            "Rerun " << codegen::ProgramName << " with --enable-object-cache=0";
        report_fatal_error(Twine(msg.str()));
    }

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief saveCachePath
 ** ------------------------------------------------------------------------------------------------------------- */
inline void ParabixObjectCache::saveCacheSettings() noexcept {


}

ParabixObjectCache::ParabixObjectCache() {
    loadCacheSettings();
    initiateCacheCleanUp();
}

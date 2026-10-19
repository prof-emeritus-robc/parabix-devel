#include <objcache/object_cache.h>

#include <objcache/object_cache_util.hpp>
#include <srchash/source_hash.h>
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
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/SHA1.h>
#include <system_error>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <llvm/Support/Signals.h>

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

// Each cached kernel is keyed by its cache name, which ends with the kernel's
// source tag (Kernel::getSourceTag): changing the source of a module that can
// shape a kernel's code (or the build configuration) invalidates that kernel's
// entry, and no other.  An --object-cache-salt adds a further component,
// giving experiments their own entries.
static const std::string & cachePrefix() {
    static const std::string prefix = [] {
        std::string p = "parabix-";
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
 * @brief traceSourceTag
 *
 * Reports where a kernel's source tag comes from: the image defining the kernel's class, plus, for
 * kernels that call nested kernels, the tags of those kernels.
 ** ------------------------------------------------------------------------------------------------------------- */
static void traceSourceTag(const kernel::Kernel * const kernel) {
    using TypeId = kernel::Kernel::TypeId;
    const void * const vtable = *reinterpret_cast<const void * const *>(kernel);
    const bool hasSourceHash = !parabix::getImageSourceHash(vtable).empty();
    errs() << "Source tag of " << kernel->getName() << ": " << kernel->getSourceTag()
           << " (" << (hasSourceHash ? "source hash" : "no source hash, build ID") << " of "
           << parabix::getImageName(vtable);
    const auto id = kernel->getTypeId();
    if (id == TypeId::Pipeline || id == TypeId::OptimizationBranch) {
        errs() << ", with the tags of its nested kernels";
    }
    errs() << ")\n";
}

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

// Each .kernel file records a hash of the IR its object was compiled from, taken right after
// generation (before any optimization), and the PARABIX_VERSION of the build that wrote it,
// as a single node holding the two strings.  Read only by -VerifyCache.
const static auto IR_HASH = "parabix.ir-hash";

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
    return lookupCachedObjectFile(builder, kernel, builder.getContext());
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief lookupCachedObjectFile
 *
 * Finds the entry for the kernel, parsing its .kernel file into C.
 ** ------------------------------------------------------------------------------------------------------------- */
ParabixObjectCache::LoadResult ParabixObjectCache::lookupCachedObjectFile(kernel::KernelBuilder & builder, kernel::Kernel * kernel, LLVMContext & C) noexcept {

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
    if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
        traceSourceTag(kernel);
    }
    auto kernelBuffer = MemoryBuffer::getFile(fileName, false, false, false);
    if (kernelBuffer) {
        auto loadedFile = parseBitcodeFile((*kernelBuffer)->getMemBufferRef(), C);
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
                    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::VerifyCache) && !kernel->hasDeterministicCodeGeneration())) {
                        errs() << "Not verified (nondeterministic code generation): " << cachePrefix() << moduleId
                               << KERNEL_FILE_EXTENSION << "\n";
                    }
                }

                return std::make_pair(std::move(obj), std::move(H));
            }
        } else if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
            errs() << "Failed to load cache file: " << moduleId << KERNEL_FILE_EXTENSION << "\n";
        }
    } else if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
        errs() << "No cache file: " << moduleId << KERNEL_FILE_EXTENSION << "\n";
    }
    return LoadResult{nullptr, nullptr};
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief computeIRHash
 *
 * A SHA-1 of the printed module.  The suffixes LLVM appends to make a name unique within its
 * LLVMContext (e.g. %Foo.3 for a struct type) depend on whatever else that context holds, which
 * varies from run to run, so they are dropped first.
 ** ------------------------------------------------------------------------------------------------------------- */
static std::string computeIRHash(const Module & M) {
    std::string ir;
    raw_string_ostream out(ir);
    M.print(out, nullptr);
    out.flush();
    auto isIdentChar = [](const char c) {
        return isAlnum(c) || c == '_' || c == '.' || c == '$' || c == '-';
    };
    std::string normal;
    normal.reserve(ir.size());
    const auto n = ir.size();
    for (size_t i = 0; i < n; ) {
        const char c = ir[i++];
        normal.push_back(c);
        if (c != '%' && c != '@') continue;
        const size_t start = i;
        while (i < n && isIdentChar(ir[i])) ++i;
        size_t end = i;
        // drop a trailing ".<digits>", but leave a name that is only digits alone
        size_t d = end;
        while (d > start && isDigit(ir[d - 1])) --d;
        if (d < end && d > start + 1 && ir[d - 1] == '.') {
            end = d - 1;
        }
        normal.append(ir, start, end - start);
    }
    SHA1 hash;
    hash.update(normal);
    const auto digest = hash.final();
    return toHex(ArrayRef<uint8_t>(digest.data(), digest.size()), /*LowerCase=*/true);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief recordIRHash
 ** ------------------------------------------------------------------------------------------------------------- */
void ParabixObjectCache::recordIRHash(Module & M) {
    LLVMContext & C = M.getContext();
    const auto hash = computeIRHash(M);
    Metadata * const ops[2] = {MDString::get(C, hash), MDString::get(C, PARABIX_VERSION)};
    NamedMDNode * const md = M.getOrInsertNamedMetadata(IR_HASH);
    md->clearOperands();
    md->addOperand(MDNode::get(C, ops));
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief loadCachedObjectFileForVerification
 ** ------------------------------------------------------------------------------------------------------------- */
ParabixObjectCache::VerificationEntry ParabixObjectCache::loadCachedObjectFileForVerification(kernel::KernelBuilder & builder, kernel::Kernel * kernel) noexcept {
    VerificationEntry entry;
    LLVMContext C;
    auto loaded = lookupCachedObjectFile(builder, kernel, C);
    if (loaded.second) {
        entry.Object = std::move(loaded.first);
        const NamedMDNode * const md = loaded.second->getNamedMetadata(IR_HASH);
        if (md && md->getNumOperands() == 1 && md->getOperand(0)->getNumOperands() == 2) {
            const MDNode * const node = md->getOperand(0);
            if (const auto hash = dyn_cast<MDString>(node->getOperand(0))) {
                entry.IRHash = hash->getString().str();
            }
            if (const auto writer = dyn_cast<MDString>(node->getOperand(1))) {
                entry.WrittenBy = writer->getString().str();
            }
        }
        // a module parsed from a file is named after it
        entry.FileName = loaded.second->getModuleIdentifier();
    }
    // the module must go before the context it lives in
    loaded.second.reset();
    return entry;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief verifyIRHash
 ** ------------------------------------------------------------------------------------------------------------- */
bool ParabixObjectCache::verifyIRHash(const kernel::Kernel * kernel, const Module & M, const VerificationEntry & entry) {
    // Called on compilation worker threads: hold the cache lock while tracing, as the other
    // traces do, so that lines from different threads cannot interleave.
    auto trace = [&](StringRef what) {
        if (LLVM_UNLIKELY(codegen::TraceObjectCache)) {
            std::lock_guard<std::mutex> L(mCacheMutex);
            errs() << what << sys::path::filename(entry.FileName) << "\n";
        }
    };
    if (LLVM_UNLIKELY(entry.IRHash.empty())) {
        trace("Cannot verify (no IR hash recorded), recompiling: ");
        return false;
    }
    if (LLVM_LIKELY(computeIRHash(M) == entry.IRHash)) {
        trace("Verified cache file: ");
        return true;
    }

    // PARABIX_VERSION is "parabixgit-<commit>" as of configure time.
    auto commitOf = [](StringRef version) {
        const auto dash = version.rfind('-');
        return dash == StringRef::npos ? version : version.substr(dash + 1);
    };
    const StringRef writtenBy = entry.WrittenBy.empty() ? StringRef("unknown") : StringRef(entry.WrittenBy);
    const StringRef thisCommit = commitOf(PARABIX_VERSION);
    const StringRef thatCommit = commitOf(writtenBy);

    const void * const vtable = *reinterpret_cast<const void * const *>(kernel);
    const auto image = parabix::getImageName(vtable);
    const auto modules = parabix::getImageModules(vtable);
    const auto id = kernel->getTypeId();
    const bool callsKernels = (id == kernel::Kernel::TypeId::Pipeline || id == kernel::Kernel::TypeId::OptimizationBranch);

    std::string msg;
    raw_string_ostream out(msg);
    out << sys::path::filename(codegen::ProgramName) << " ERROR: object cache verification (-VerifyCache) failed for kernel " << kernel->getName() << "\n"
           "\n"
           "  cache entry: " << entry.FileName << "\n"
           "  written by:  " << writtenBy << " (revision at configure time); this is " << PARABIX_VERSION << "\n"
           "  source tag:  " << kernel->getSourceTag() << ", from the source hash of " << image << "\n"
           "\n"
           "The IR generated for this kernel now differs from the IR its cached object was compiled\n"
           "from, although both have the same cache name: something that shapes this kernel's code\n"
           "is missing from its cache key.  Either\n"
           "\n"
           "  - source outside the modules in the key changed, and code from it runs while this kernel\n"
           "    is generated.  The key covers these modules (all that " << image << " links, transitively):\n"
           "      " << (modules.empty() ? std::string("(none registered)") : modules) << "\n"
           "    Add the module holding that code to the DEPS of the target that builds " << image << ";\n";
    if (callsKernels) {
        out << "\n"
               "  - or, since this kernel calls nested kernels, a property of one of them that its code\n"
               "    depends on is missing from its source tag (see getSourceTag);\n";
    }
    out << "\n"
           "  - or a parameter or command-line option changes the generated IR without changing the\n"
           "    kernel's name or signature: record it in the name (the kernel's constructor, or\n"
           "    Kernel::annotateKernelNameWithDebugFlags) or in its signature.\n"
           "\n"
           "To narrow it down:\n"
           "\n";
    if (thatCommit != thisCommit) {
        out << "  1. List what changed since the entry was written, and look for files outside the\n"
               "     modules above:\n"
               "       git diff --stat " << thatCommit << " -- lib include tools\n";
    } else {
        out << "  1. The entry was written by a build configured at this same revision, so the change is\n"
               "     in uncommitted work or in commits made since configuring: look through git status\n"
               "     and git diff for files outside the modules above.\n";
    }
    out << "  2. If no source outside those modules changed, compare the command-line options of the\n"
           "     run that wrote the entry with this one's.\n"
           "  3. To see the difference itself, run with -enable-object-cache=0 -ShowUnoptimizedIR=new.ll,\n"
           "     build the writer's revision and run it the same way with -ShowUnoptimizedIR=old.ll,\n"
           "     then diff the definitions for " << kernel->getName() << ".\n"
           "\n"
           "Without -VerifyCache the entry is used as is; --update-object-cache recompiles it.\n";
    out.flush();
    // This runs on a compilation worker thread: exit() (as report_fatal_error's handlers call)
    // would run static destructors under the other threads' feet, so leave without them.
    const auto written = ::write(STDERR_FILENO, msg.data(), msg.size());
    (void)written;
    sys::RunInterruptHandlers();
    std::_Exit(EXIT_FAILURE);
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

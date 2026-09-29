/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <toolchain/toolchain.h>
#include <ucd/core/UCD_Config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/StringRef.h>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <thread>
#include <cstdlib>
#include <map>
#include <set>

#if defined(PARABIX_ARM_TARGET)
#include <llvm/TargetParser/AArch64TargetParser.h>
#include <arm_sve.h>
#endif

using namespace llvm;

#ifndef NDEBUG
#define IN_DEBUG_MODE true
#else
#define IN_DEBUG_MODE false
#endif

// #define FORCE_ASSERTIONS

// #define DISABLE_OBJECT_CACHE

namespace codegen {

llvm::StringMap<bool> GetFeatureNames() {
    StringMap<bool> features;
#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(19, 0, 0)
    if (!sys::getHostCPUFeatures(features)) {
#ifdef PARABIX_ARM_TARGET
        std::vector<StringRef> extNames;
        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(17, 0, 0)
            auto info = llvm::AArch64::parseCpu(sys::getHostCPUName());
            if (info) {
                llvm::AArch64::getExtensionFeatures(info->Arch.DefaultExts | info->DefaultExtensions, extNames);
            }
        #else
            const llvm::AArch64::CpuInfo & info = llvm::AArch64::parseCpu(sys::getHostCPUName());
            llvm::AArch64::getExtensionFeatures(info.Arch.DefaultExts | info.DefaultExtensions, extNames);
        #endif
        for (const auto eName : extNames) {
            //llvm::errs() << "Extension: " << eName << "\n";
            if (eName.size() > 1) {
                char op = eName[0];
                if (op == '+' || op == '-') {
                    features[eName.drop_front(1)] = (op == '+');
                }
            }
        }
#else
        llvm::report_fatal_error(
            "codegen::GetFeatureNames() failed to get host CPU features");
#endif
    }
#else
    features = sys::getHostCPUFeatures();
#endif

    // Parse a list of feature options basically like "mattrs", comma-separated +X or -X strings, adding them to the map
    // with true/false depending on whether they are +/-. That is, "+sse,-bmi" would map to "sse"=true, "bmi"=false.
    if (!CPUFeatureOptions.empty()) {
        llvm::StringRef ref(CPUFeatureOptions);
        while (!ref.empty()) {
            llvm::StringRef raw;
            std::tie(raw, ref) = ref.split(',');
            std::string feature = raw.trim().lower();
            if (feature.size() > 1) {
                char op = feature[0];
                if (op == '+' || op == '-') {
                    features[feature.substr(1)] = (op == '+');
                }
            }
        }
    }

    return features;
}

FeatureSet MapFeatureNames(llvm::StringMap<bool> const &namedFeatures) {
    FeatureSet featureSet;

    StringMap<Feature> namesToFeatures = {
#ifdef PARABIX_X86_TARGET
        {"ssse3", Feature::SSSE3},
        {"avx", Feature::AVX},
        {"avx2", Feature::AVX2},
        // if (HasAVX || HasAVX2)...
        {"bmi", Feature::AVX_BMI},
        {"bmi2", Feature::AVX_BMI2},
        // if (HasAVX512F)...
        {"avx512f", Feature::AVX512F},
        {"avx512cd", Feature::AVX512_CD},
        {"avx512bw", Feature::AVX512_BW},
        {"avx512dq", Feature::AVX512_DQ},
        {"avx512vl", Feature::AVX512_VL},
        // AVX512_VBMI, AVX512_VBMI2 and AVX512_VPOPCNTDQ  have not been tested as we
        //did not have hardware support. It should work in theory (tm)
        {"avx512vbmi", Feature::AVX512_VBMI},
        {"avx512vbmi2", Feature::AVX512_VBMI2},
        {"avx512vpopcntdq", Feature::AVX512_VPOPCNTDQ},
#elif defined(PARABIX_ARM_TARGET)
        {"sve", Feature::SVE},
        {"sve2", Feature::SVE2},
#endif
    };

    // Translate feature list to bit flags
    for (auto const &f : namedFeatures) {
        auto found = namesToFeatures.find(f.first());
        if (f.second && found != namesToFeatures.end()) {
            featureSet.set((size_t)found->second);
        }
    }

    return featureSet;
}

unsigned DefaultBlockSizeForFeatures(const FeatureSet &featureSet) {
#if defined(PARABIX_X86_TARGET)
    if (featureSet.test((size_t)Feature::AVX512F)) {
        return 512;
    } else if (featureSet.test((size_t)Feature::AVX2)) {
        return 256;
    } else {
        return 128;
    }
#elif defined(PARABIX_ARM_TARGET)
    if (featureSet.test((size_t)Feature::SVE)) {
        return HostSVEBitWidth();
    }
    return 128;
#else
    return 64;
#endif
}

cl::OptionCategory JIT_InfoOptions("J.  JIT Information Options", 
    "These options control production of information reports during JIT compilation.");

cl::OptionCategory CodeGenOptions("K.  Kernel and Pipeline Compilation Options", 
    "These options control how kernel and pipeline code is generated.");

cl::OptionCategory InstrumentationOptions("X.  Execution Time Instrumentation Options", 
    "These options instrument executed code for tracing or statistics gathering.");

static cl::bits<InfoFlags>
JIT_InfoFlags(cl::desc("JIT Info Flags"),
              cl::values(clEnumVal(PrintKernelSizes, "Write kernel state object size in bytes to stderr."),
                         clEnumVal(PrintPipelineGraph, "Write PipelineKernel graph in dot file format to stderr.")),
              cl::cat(JIT_InfoOptions));

static cl::bits<DebugFlags>
KernelFlags(cl::desc("Flags controlling kernel/pipeline compilation"), 
            cl::values(clEnumVal(SerializeThreads, "Force segment threads to run sequentially."),
                        clEnumVal(DisableIndirectBranch, "Disable use of indirect branches in kernel code."),
                        clEnumVal(DisableThreadLocalStreamSets, "Disable use of thread-local memory for streamsets within the same partition."),
                        clEnumVal(DisableCacheAlignedKernelStructs, "Disable cache alignment of kernel state memory."),
                        clEnumVal(DisableInOutAttributes, "Disable In/Out attributes for streamset data buffers."),
                        clEnumVal(EnableAsserts, "Enable built-in Parabix framework asserts in all generated IR."),

                        clEnumVal(EnableStreamSetAsserts, "Enable built-in Parabix framework asserts for streamset I/O IR."),

                        clEnumVal(EnablePipelineAsserts, "Enable built-in Parabix framework asserts in generated pipeline IR."),
                        clEnumVal(EnableMProtect, "Use mprotect to cause a write fault when erroneously "
                                                  "overwriting kernel state / stream space."),
                        clEnumVal(ForcePipelineRecompilation, "Disable object cache lookup for any PipelineKernel."),
                        clEnumVal(VerifyIR, "Run the IR verification pass.")),
            cl::cat(CodeGenOptions));

static cl::bits<StatisticsFlags>
StatisticsOptions(cl::desc("Statistics gathering options"), 
             cl::values(clEnumVal(EnableCycleCounter, "Count and report CPU cycles per kernel."),
                        clEnumVal(GenerateTransferredItemCountHistogram, "Generate a histogram CSV of each non-Fixed port detailing "
                                                                         "the transfered item count per executed stride."),
                        clEnumVal(GenerateDeferredItemCountHistogram, "Generate a histogram CSV of each deferred port detailing "
                                                                      "the difference between the deferred and total item count "
                                                                      "per executed stride."),
                        #ifdef ENABLE_PAPI
                        clEnumVal(DisplayPAPICounterThreadTotalsOnly, "Disable per-kernel PAPI counters when given a valid PapiCounters list."),
                        #endif
                        clEnumVal(EnableBlockingIOCounter, "Count and report the number of blocked kernel "
                                                           "executions due to insufficient data/space of a "
                                                           "particular stream."),
                        clEnumVal(TraceCounts, "Trace kernel processed, consumed and produced item counts."),
                        clEnumVal(TraceDynamicBuffers, "Trace dynamic buffer allocations and deallocations."),
                        clEnumVal(TraceDynamicMultithreading, "Trace dynamic multithreading thread count state."),
                        clEnumVal(TraceBlockedIO, "Trace kernels prevented from processing any strides "
                                                  "due to insufficient input items / output space."),
                        clEnumVal(TraceStridesPerSegment, "Trace number of strides executed over segments."),
                        clEnumVal(TraceProducedItemCounts, "Trace produced item count deltas over segments."),
                        clEnumVal(TraceUnconsumedItemCounts, "Trace unconsumed item counts over segments.")),
             cl::cat(InstrumentationOptions));

std::string ShowIROption = OmittedOption;
static cl::opt<std::string, true> IROutputOption("ShowIR", cl::location(ShowIROption), cl::ValueOptional,
  cl::desc("Print optimized LLVM IR to stderr (by omitting =<filename>) or a file"),
  cl::value_desc("filename"), cl::cat(JIT_InfoOptions));


std::string ShowUnoptimizedIROption = OmittedOption;
static cl::opt<std::string, true> UnoptimizedIROutputOption("ShowUnoptimizedIR", cl::location(ShowUnoptimizedIROption), cl::ValueOptional,
  cl::desc("Print generated LLVM IR to stderr (by omitting =<filename> or a file"),
  cl::value_desc("filename"), cl::cat(JIT_InfoOptions));


std::string ShowIRFilter = "";
static cl::opt<std::string, true> ToShowIRFilerOption("ToShow", cl::location(ShowIRFilter), cl::ValueOptional,
  cl::desc("Regex filter to choose which kernels to display when showing LLVM IR"),
  cl::value_desc("regex"), cl::cat(JIT_InfoOptions));


std::string ThreadLocalPermittedOptions = "";
static cl::opt<std::string, true> optThreadLocalPermittedOption("permitted-thread-local-streamsets", cl::location(ThreadLocalPermittedOptions), cl::ValueOptional,
  cl::desc("Comma delimited list of which streamsets to permit to be thread local (default=all)"),
  cl::value_desc("streamsets"), cl::cat(CodeGenOptions));

std::string PreserveAllStreamSetDataOptions = "";
static cl::opt<std::string, true> optPreserveAllStreamSetDataOption("preserve-all-streamset-data", cl::location(PreserveAllStreamSetDataOptions), cl::ValueOptional,
  cl::desc("Comma delimited list of which streamsets to permit to be thread local (default=all)"),
  cl::value_desc("streamsets"), cl::cat(CodeGenOptions));

std::string DoubleStreamSetSizeOptions = "";
static cl::opt<std::string, true> optDoubleStreamSetSizeOptions("double-streamset-size", cl::location(DoubleStreamSetSizeOptions), cl::ValueOptional,
  cl::desc("Comma delimited list of which streamsets to permit to be thread local (default=all)"),
  cl::value_desc("streamsets"), cl::cat(CodeGenOptions));

std::string CPUFeatureOptions = "";
static cl::opt<std::string, true> optCPUFeatureOptions("cpu-features", cl::location(CPUFeatureOptions), cl::ValueOptional,
  cl::desc("Comma delimited list of CPU features to enable or disable"),
  cl::value_desc("attrs"), cl::cat(CodeGenOptions));

bool UseI64Builder = false;
static cl::opt<bool, true> optUseI64Builder("i64-builder", cl::location(UseI64Builder),
  cl::desc("Force fallback (scalar) path even at larger bit block size"), cl::cat(CodeGenOptions));

#ifdef ENABLE_PAPI
std::string PapiCounterOptions = OmittedOption;
static cl::opt<std::string, true> clPapiCounterOptions("PapiCounters", cl::location(PapiCounterOptions), cl::ValueOptional,
                                                       cl::desc("comma delimited list of PAPI event names (run papi_avail for options)"),
                                                       cl::value_desc("comma delimited list"), cl::cat(InstrumentationOptions));
#endif

std::string ShowASMOption = OmittedOption;
static cl::opt<std::string, true> ASMOutputFilenameOption("ShowASM", cl::location(ShowASMOption), cl::ValueOptional,
  cl::desc("Print generated assembly code to stderr (by omitting =<filename> or a file"),
  cl::value_desc("filename"), cl::cat(JIT_InfoOptions));

// Enable Debug Options to be specified on the command line

static cl::opt<CodeGenOptLevel, true>
OptimizationLevel("optimization-level", cl::location(OptLevel), cl::init(CodeGenOptLevel::None), cl::desc("Set the front-end optimization level:"),
                  cl::values(clEnumValN(CodeGenOptLevel::None, "none", "no optimizations (default)"),
                             clEnumValN(CodeGenOptLevel::Less, "less", "trivial optimizations"),
                             clEnumValN(CodeGenOptLevel::Default, "standard", "standard optimizations"),
                             clEnumValN(CodeGenOptLevel::Aggressive, "aggressive", "aggressive optimizations")), cl::cat(CodeGenOptions));
static cl::opt<CodeGenOptLevel, true>
BackEndOptOption("backend-optimization-level", cl::location(BackEndOptLevel), cl::init(CodeGenOptLevel::None), cl::desc("Set the back-end optimization level:"),
                  cl::values(clEnumValN(CodeGenOptLevel::None, "none", "no optimizations (default)"),
                             clEnumValN(CodeGenOptLevel::Less, "less", "trivial optimizations"),
                             clEnumValN(CodeGenOptLevel::Default, "standard", "standard optimizations"),
                             clEnumValN(CodeGenOptLevel::Aggressive, "aggressive", "aggressive optimizations")), cl::cat(CodeGenOptions));

PipelineCompilationModeOptions PipelineCompilationMode = PipelineCompilationModeOptions::DefaultFast;

static cl::opt<PipelineCompilationModeOptions, true>
PipelineCompilationModeOption("pipeline-optimization-level", cl::location(PipelineCompilationMode),
                  cl::init(PipelineCompilationModeOptions::DefaultFast),
                  cl::desc("Set the pipeline optimization level:"),
                  cl::values(clEnumValN(PipelineCompilationModeOptions::DefaultFast, "fast", "minimal analysis(default)"),
                             clEnumValN(PipelineCompilationModeOptions::Expensive, "aggressive", "full analysis")), cl::cat(CodeGenOptions));

static cl::opt<bool, true> EnableObjectCacheOption("enable-object-cache", cl::location(EnableObjectCache), cl::init(true),
                                                   cl::desc("Enable object caching"), cl::cat(CodeGenOptions));

bool UpdateObjectCache = false;
static cl::opt<bool, true> UpdateObjectCacheOption("update-object-cache", cl::location(UpdateObjectCache), cl::init(false),
                                                   cl::desc("Force existing object cache entries to be recompiled and replaced, "
                                                            "rather than reused. Implied if --optimization-level or "
                                                            "--backend-optimization-level is explicitly set."), cl::cat(CodeGenOptions));

static cl::opt<bool, true> EnableModuleInlinerOption("enable-kernel-module-inliner", cl::location(EnableModuleInliner), cl::init(false),
                                                   cl::desc("Run a whole-module inliner pass over each kernel's IR before object generation."), cl::cat(CodeGenOptions));

static cl::opt<bool, true> TraceObjectCacheOption("trace-object-cache", cl::location(TraceObjectCache), cl::init(false),
                                                   cl::desc("Trace object cache retrieval."), cl::cat(JIT_InfoOptions));

static cl::opt<std::string> ObjectCacheDirOption("object-cache-dir", cl::init(""),
                                                 cl::desc("Path to the object cache diretory"), cl::cat(CodeGenOptions));

// The custom allocator keeps persistent, long-lived exec/data slab pools rather than
// allocating a small dedicated region per compiled object as LLVM's default in-process
// memory manager does. That's a deliberate linking-speed optimization, but under LLVM 21
// it can place exec and data content too far apart for Mach-O compact-unwind info's
// 32-bit deltas, so it defaults to off there; LLVM < 21 is unaffected and defaults to on.
static cl::opt<bool, true> UseCustomJITMemoryManagerOption("use-custom-jit-memory-manager", cl::location(UseCustomJITMemoryManager),
    #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(21, 0, 0)
    cl::init(false),
    #else
    cl::init(true),
    #endif
    cl::desc("Use the custom slab-based JIT memory manager instead of LLVM's default in-process memory manager."),
    cl::cat(CodeGenOptions));

unsigned CompileThreads;
static cl::opt<unsigned, true>
CompileThreadsOption("compile-threads", cl::location(CompileThreads), cl::init(4),
                     cl::desc("Number of threads used for JIT compilation."),
                     cl::value_desc("positive integer"), cl::cat(CodeGenOptions));

bool UseMCJIT = false;
static cl::opt<bool, true> UseMCJITOption("use-mcjit", cl::location(UseMCJIT), cl::init(false),
    cl::desc("Use classic single-threaded MCJIT instead of the default multi-threaded ORC JIT backend."),
    cl::cat(CodeGenOptions));

bool EnableDynamicMultithreading;
static cl::opt<bool, true> EnableDynamicMultithreadingOption("dynamic-multithreading", cl::location(EnableDynamicMultithreading), cl::init(false),
                                                   cl::desc("Dynamic multithreading."), cl::cat(CodeGenOptions));

float DynamicMultithreadingAddThreshold;
static cl::opt<float, true> DynamicMultithreadingAddThresholdOption("dynamic-multithreading-add-threshold", cl::location(DynamicMultithreadingAddThreshold), cl::init(10.0),
                                                   cl::desc("Dynamic multithreading."), cl::cat(CodeGenOptions));

float DynamicMultithreadingRemoveThreshold;
static cl::opt<float, true> DynamicMultithreadingRemoveThresholdOption("dynamic-multithreading-remove-threshold", cl::location(DynamicMultithreadingRemoveThreshold), cl::init(15.0),
                                                   cl::desc("Dynamic multithreading."), cl::cat(CodeGenOptions));

size_t DynamicMultithreadingPeriod;
static cl::opt<size_t, true> DynamicMultithreadingPeriodOption("dynamic-multithreading-period", cl::location(DynamicMultithreadingPeriod), cl::init(100),
                                                   cl::desc("Dynamic multithreading."), cl::cat(CodeGenOptions));

static cl::opt<int, true> FreeCallBisectOption("free-bisect-value", cl::location(FreeCallBisectLimit), cl::init(-1),
                                                    cl::desc("The number of free calls to allow in bisecting"), cl::cat(CodeGenOptions));

static cl::opt<unsigned, true> BlockSizeOption("BlockSize", cl::location(BlockSize), cl::init(0),
                                          cl::desc("specify a block size (defaults to widest SIMD register width in bits)."), cl::cat(CodeGenOptions));


const unsigned DefaultSegmentSize = 16384;
static cl::opt<unsigned, true> SegmentSizeOption("segment-size", cl::location(SegmentSize),
                                               cl::init(DefaultSegmentSize),
                                               cl::desc("Expected amount of input data to process per segment"), cl::value_desc("positive integer"), cl::cat(CodeGenOptions));

static cl::opt<unsigned, true> BufferSegmentsOption("buffer-segments", cl::location(BufferSegments), cl::init(1),
                                               cl::desc("Buffer Segments"), cl::value_desc("positive integer"));


unsigned Z3_Timeout;
static cl::opt<unsigned, true> Z3_TimeoutOption("Z3-timeout", cl::location(Z3_Timeout), cl::init(3000),
                                               cl::desc("Z3 timeout"), cl::value_desc("positive integer"));

static cl::opt<unsigned, true>
MaxTaskThreadsOption("max-task-threads", cl::location(TaskThreads),
                     cl::init(std::thread::hardware_concurrency()),
                     cl::desc("Maximum number of threads to assign for separate pipeline tasks."),
                     cl::value_desc("positive integer"));

static cl::opt<unsigned, true>
ThreadNumOption("thread-num", cl::location(SegmentThreads), cl::init(3),
                cl::desc("Number of threads used for segment pipeline parallel"),
                cl::value_desc("positive integer"));

static cl::opt<unsigned, true> ScanBlocksOption("scan-blocks", cl::location(ScanBlocks), cl::init(4),
                                          cl::desc("Number of blocks per stride for scanning kernels"), cl::value_desc("positive initeger"));

std::string TraceOption = "";
static cl::opt<std::string, true> TraceValueOption("trace", cl::location(TraceOption),
                                            cl::desc("Trace the values of variables beginning with the given prefix."), cl::value_desc("prefix"), cl::cat(CodeGenOptions));

bool EnableIllustrator;
static cl::opt<bool, true> OptEnableIllustrator("enable-illustrator", cl::location(EnableIllustrator),
                                                 cl::desc("Enable bitstream illustrator with the default display width."), cl::init(0), cl::cat(CodeGenOptions));

int IllustratorDisplay;
static cl::opt<int, true> OptIllustratorWidth("illustrator-width", cl::location(IllustratorDisplay),
                                                 cl::desc("Enable bitstream illustrator with the given display width."), cl::init(0), cl::cat(CodeGenOptions));

std::string CCCOption = "";
static cl::opt<std::string, true> CCTypeOption("ccc-type", cl::location(CCCOption), cl::init("binary"),
                                            cl::desc("The character class compiler"), cl::value_desc("[binary, ternary]"));

bool TimeKernelsIsEnabled;
static cl::opt<bool, true> OptCompileTime("time-kernels", cl::location(TimeKernelsIsEnabled),
                                        cl::desc("Times each kernel, printing elapsed time for each on exit"), cl::init(false));


bool UseProcessThreadForIO;
static cl::opt<bool, true> OptUseProcessThreadForIO("io-thread", cl::location(UseProcessThreadForIO),
                                        cl::desc("Only permit the process thread to perform IO"), cl::init(false));

CodeGenOptLevel OptLevel;
CodeGenOptLevel BackEndOptLevel;

const char * ObjectCacheDir;

unsigned BlockSize;

unsigned SegmentSize;

unsigned BufferSegments;
unsigned TaskThreads;
unsigned SegmentThreads;

unsigned ScanBlocks;

bool EnableObjectCache = true;
bool EnableModuleInliner = false;
bool EnablePipelineObjectCache = true;
bool TraceObjectCache;
bool UseCustomJITMemoryManager = true;
bool ObjectCacheForceUpdate = false;

unsigned CacheDaysLimit;

int FreeCallBisectLimit;

unsigned GroupNum;

TargetOptions target_Options;

const cl::OptionCategory * LLVM_READONLY codegen_flags() {
    return &CodeGenOptions;
}

bool LLVM_READONLY DebugOptionIsSet(const DebugFlags flag) {
    #ifdef FORCE_ASSERTIONS
    if (flag == DebugFlags::EnableAsserts) return true;
    #endif
    return KernelFlags.isSet(flag);
}

bool LLVM_READONLY DebugOptionIsSet(const DebugFlags flag1, const DebugFlags flag2) {
    #ifdef FORCE_ASSERTIONS
    if (flag1 == DebugFlags::EnableAsserts) return true;
    if (flag2 == DebugFlags::EnableAsserts) return true;
    #endif
    return KernelFlags.isSet(flag1) || KernelFlags.isSet(flag2);
}

bool LLVM_READONLY StatisticsOptionIsSet(const StatisticsFlags flag) {
    return StatisticsOptions.isSet(flag);
}

bool LLVM_READONLY InfoOptionIsSet(const InfoFlags flag) {
    return JIT_InfoFlags.isSet(flag);
}

bool LLVM_READONLY AnyDebugOptionIsSet() {
    #ifdef FORCE_ASSERTIONS
    return true;
    #endif
    return (KernelFlags.getBits() != 0) || 
           (JIT_InfoFlags.getBits() != 0) || 
           (StatisticsOptions.getBits() != 0);
}

bool LLVM_READONLY AnyAssertionOptionIsSet() {
    #ifdef FORCE_ASSERTIONS
    return true;
    #endif
    return KernelFlags.isSet(DebugFlags::EnableAsserts) || KernelFlags.isSet(DebugFlags::EnableStreamSetAsserts) || KernelFlags.isSet(DebugFlags::EnablePipelineAsserts);
}

const char * ProgramName;

static inline bool disableObjectCacheDueToCommandLineOptions() {
    if (!TraceOption.empty()) return true;
    if (JIT_InfoFlags.isSet(PrintKernelSizes)) return true;
    if (JIT_InfoFlags.isSet(PrintPipelineGraph)) return true;
    if (ShowIROption != OmittedOption) return true;
    if (ShowUnoptimizedIROption != OmittedOption) return true;
    if (ShowASMOption != OmittedOption) return true;
//    if (pablo::ShowPabloOption != OmittedOption) return true;
//    if (pablo::ShowOptimizedPabloOption != OmittedOption) return true;
    return false;
}

static inline bool disablePipelineObjectCacheDueToCommandLineOptions() {
    if (JIT_InfoFlags.isSet(PrintPipelineGraph)) return true;
    if (KernelFlags.isSet(EnablePipelineAsserts)) return true;
    if (KernelFlags.isSet(DisableThreadLocalStreamSets)) return true;
    if (KernelFlags.isSet(ForcePipelineRecompilation)) return true;
    return false;
}

// Modified version of cl::HideUnrelatedOptions: it's too aggressive, this leaves things visible with --help-hidden
static inline void gentlyHideUnrelatedOptions(ArrayRef<const cl::OptionCategory *> Categories,
                                              cl::SubCommand &Sub = cl::SubCommand::getTopLevel()) {
    for (auto &I : cl::getRegisteredOptions(Sub)) {
        bool Unrelated = true;
        for (auto &Cat : I.second->Categories) {
            if (is_contained(Categories, Cat) || (Cat->getName() == "Generic Options"))
                Unrelated = false;
        }
        // Only increase hidden-ness, don't take things from ReallyHidden down to Hidden
        if (Unrelated && (I.second->getOptionHiddenFlag() == cl::NotHidden))
            I.second->setHiddenFlag(cl::Hidden);
    }
}

// Since LLVM 15, cl::opt no longer rejects a repeated single-value option: cl::Optional
// and cl::Required only enforce a minimum, and the last occurrence silently wins. Restore
// the error for every option declared to occur at most once (cl::Optional or cl::Required,
// the cl::opt default), after parsing. cl::list and options explicitly declared
// cl::ZeroOrMore/cl::OneOrMore may still repeat. Options named in TEST_FLAGS
// (testFlagArgs) are exempt, since TEST_FLAGS exists to override a test's own flags.
static void reportRepeatedOptions(const std::vector<std::string> & testFlagArgs) {
    std::set<std::string> testFlagNames;
    for (const auto & arg : testFlagArgs) {
        StringRef name(arg);
        if (!name.consume_front("-")) continue;
        name.consume_front("-");
        testFlagNames.insert(name.take_until([](char c) { return c == '='; }).str());
    }
    std::map<std::string, cl::Option *> repeated;   // sorted by name for stable output
    for (auto & entry : cl::getRegisteredOptions()) {
        cl::Option * const O = entry.second;
        // getRegisteredOptions() returns a DenseMap from LLVM 22 (key in .first), and a
        // StringMap before that (key via first()).
        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(22, 0, 0)
        const std::string name = entry.first.str();
        #else
        const std::string name = entry.first().str();
        #endif
        const auto flag = O->getNumOccurrencesFlag();
        if ((flag == cl::Optional || flag == cl::Required) && O->getNumOccurrences() > 1
                && testFlagNames.count(name) == 0) {
            repeated.emplace(name, O);
        }
    }
    if (LLVM_LIKELY(repeated.empty())) return;
    for (auto & [name, O] : repeated) {
        O->error("may only occur zero or one times!", name);
    }
    exit(1);
}

void ParseCommandLineOptions(int argc, const char * const *argv, std::initializer_list<const cl::OptionCategory *> hiding, StringRef overview) {
    AddParabixVersionPrinter();

    codegen::ProgramName = argv[0];
    if (hiding.size() != 0) {
        gentlyHideUnrelatedOptions(ArrayRef<const cl::OptionCategory *>(hiding));
    }

    // Every Parabix tool and test binary funnels its argv through here, so this is a
    // single choke point to uniformly inject extra flags via the TEST_FLAGS environment
    // variable, e.g. `TEST_FLAGS="--use-mcjit" make check` to run the whole test suite
    // under a different backend/setting without editing every test script or
    // CMakeLists.txt COMMAND. Whitespace-separated; appended after argv's own flags, so
    // for a single-value option the TEST_FLAGS value overrides the command line's (the
    // last occurrence wins; see reportRepeatedOptions). A no-op when unset, which is the overwhelming
    // majority of invocations, including every normal (non-test) run of any of these
    // programs -- quoted values containing spaces are not supported, matching the same
    // naive whitespace-splitting convention as CFLAGS/CXXFLAGS-style environment variables
    // elsewhere in the C/C++ build ecosystem.
    std::vector<std::string> extraArgStorage;
    std::vector<const char *> expandedArgv(argv, argv + argc);
    if (const char * const testFlags = std::getenv("TEST_FLAGS")) {
        boost::split(extraArgStorage, std::string(testFlags), boost::is_any_of(" \t"), boost::token_compress_on);
        for (const auto & arg : extraArgStorage) {
            if (!arg.empty()) {
                expandedArgv.push_back(arg.c_str());
            }
        }
    }

    cl::ParseCommandLineOptions((int)expandedArgv.size(), expandedArgv.data(), overview);
    reportRepeatedOptions(extraArgStorage);
    if(BlockSize == 0) {
        BlockSize = DefaultBlockSizeForFeatures(MapFeatureNames(GetFeatureNames()));
    }
//    if (LLVM_UNLIKELY(!PabloIllustrateBitstreamRegEx.empty() || IllustratorDisplay != 0)) {
//        EnableIllustrator = true;
//    }
    if (disableObjectCacheDueToCommandLineOptions()) {
        EnableObjectCache = false;
    } else if (disablePipelineObjectCacheDueToCommandLineOptions()) {
        EnablePipelineObjectCache = false;
    }
    // A cache entry compiled under one --optimization-level/--backend-optimization-level
    // setting is not distinguished from one compiled under another (they share the same
    // cache key), so silently reusing it would serve code built at the wrong opt level.
    // Rather than growing the cache with a separate entry per opt level, explicitly
    // setting either flag (or passing --update-object-cache directly) forces existing
    // entries to be recompiled and replaced in place.
    ObjectCacheForceUpdate = UpdateObjectCache
        || (OptimizationLevel.getNumOccurrences() > 0)
        || (BackEndOptOption.getNumOccurrences() > 0);
    ObjectCacheDir = ObjectCacheDirOption.empty() ? nullptr : ObjectCacheDirOption.data();
    target_Options.MCOptions.AsmVerbose = true;

    if (UseMCJIT) {
        // --compile-threads and --use-custom-jit-memory-manager only affect the default
        // ORC JIT backend's worker-thread pool and its custom JITLink memory manager;
        // MCJIT compiles single-threaded and has no equivalent knobs, so both are ignored.
        if (CompileThreadsOption.getNumOccurrences() > 0) {
            errs() << "warning: --compile-threads is ignored under --use-mcjit (MCJIT compiles single-threaded)\n";
        }
        if (UseCustomJITMemoryManagerOption.getNumOccurrences() > 0) {
            errs() << "warning: --use-custom-jit-memory-manager is ignored under --use-mcjit\n";
        }
    }

}

void printParabixVersion (raw_ostream & outs) {
    outs << "Parabix revision " << PARABIX_VERSION << "\n";
    outs << "Unicode version " << UCD::UnicodeVersion << "\n";
    llvm::sys::printDefaultTargetAndDetectedCPU(outs);
}

void AddParabixVersionPrinter() {
    cl::AddExtraVersionPrinter(&printParabixVersion);
}

void setTaskThreads(unsigned taskThreads) {
    TaskThreads = std::max(taskThreads, 1u);
    unsigned coresPerTask = std::thread::hardware_concurrency()/TaskThreads;
    SegmentThreads = std::min(coresPerTask, SegmentThreads);
}

}

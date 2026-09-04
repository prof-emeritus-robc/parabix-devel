/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#if defined(PARABIX_ARM_TARGET)
#include <arm_sve.h>
#endif

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Target/TargetMachine.h>

#include <bitset>

#ifndef LLVM_VERSION_CODE
// #defines for comparison with LLVM_VERSION_INTEGER
#define LLVM_VERSION_CODE(major, minor, point) ((10000 * major) + (100 * minor) + point)
#endif

namespace llvm { namespace cl { class OptionCategory; } }

#if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(18, 0, 0)
namespace llvm {
using CodeGenOptLevel = CodeGenOpt::Level;
}
#endif

namespace codegen {

extern llvm::cl::OptionCategory JIT_InfoOptions;
extern llvm::cl::OptionCategory CodeGenOptions;
extern llvm::cl::OptionCategory InstrumentationOptions;

const llvm::cl::OptionCategory * LLVM_READONLY codegen_flags();

// Command Parameters
enum InfoFlags {
    PrintPipelineGraph,
    PrintKernelSizes,
    InfoFlagSentinel
};

enum DebugFlags {
    VerifyIR,
    SerializeThreads,
    EnableAsserts,
    EnableStreamSetAsserts,
    EnablePipelineAsserts,
    EnableMProtect,
    DisableIndirectBranch,
    DisableThreadLocalStreamSets,
    DisableCacheAlignedKernelStructs,
    DisableInOutAttributes,
    ForcePipelineRecompilation,
    DebugFlagSentinel
};

enum StatisticsFlags {
    EnableCycleCounter,
    GenerateTransferredItemCountHistogram,
    GenerateDeferredItemCountHistogram,
    #ifdef ENABLE_PAPI
    DisplayPAPICounterThreadTotalsOnly,
    #endif
    EnableBlockingIOCounter,
    TraceCounts,
    TraceDynamicBuffers,
    TraceDynamicMultithreading,
    TraceStridesPerSegment,
    TraceProducedItemCounts,
    TraceUnconsumedItemCounts,
    TraceBlockedIO,
    StatisticsFlagSentinel
};

enum PipelineCompilationModeOptions {
    DefaultFast
    , Expensive
};

// not an exhaustive list; can be extended but keep __Count as the last entry
enum class Feature : size_t {
    SSSE3,
    // ---------------
    AVX,
    AVX_BMI,
    AVX_BMI2,
    // ---------------
    AVX2,
    // ---------------
    AVX512F,
    AVX512_CD,
    AVX512_BW,
    AVX512_DQ,
    AVX512_VL,
    AVX512_VBMI,
    AVX512_VBMI2,
    AVX512_VPOPCNTDQ,
    // ---------------
    SVE,
    // ---------------
    SVE2,
    __Count
};

using FeatureSet = std::bitset<(size_t)Feature::__Count>;

#if defined(PARABIX_ARM_TARGET)
__attribute__((target ("+sve")))
static inline unsigned HostSVEBitWidth() {
    return svcntb() * 8;
}
#endif

llvm::StringMap<bool> GetFeatureNames();
FeatureSet MapFeatureNames(llvm::StringMap<bool> const &namedFeatures);
unsigned DefaultBlockSizeForFeatures(const FeatureSet &featureSet);

bool LLVM_READONLY DebugOptionIsSet(const DebugFlags flag);

bool LLVM_READONLY DebugOptionIsSet(const DebugFlags flag1, const DebugFlags flag2);

bool LLVM_READONLY InfoOptionIsSet(const InfoFlags flag);

bool LLVM_READONLY StatisticsOptionIsSet(const StatisticsFlags flag);

bool LLVM_READONLY AnyDebugOptionIsSet();

bool LLVM_READONLY AnyAssertionOptionIsSet();

// Options for generating IR or ASM to files
const std::string OmittedOption = ".";
extern std::string ShowUnoptimizedIROption;
extern std::string ShowIROption;
extern std::string ShowIRFilter;
extern std::string TraceOption;
extern std::string CCCOption;
extern std::string ThreadLocalPermittedOptions;
extern std::string PreserveAllStreamSetDataOptions;
extern std::string DoubleStreamSetSizeOptions;
extern std::string CPUFeatureOptions;
extern bool UseI64Builder;
extern PipelineCompilationModeOptions PipelineCompilationMode;
#ifdef ENABLE_PAPI
extern std::string PapiCounterOptions;
#endif
extern std::string ShowASMOption;
extern const char * ObjectCacheDir;
extern unsigned CacheDaysLimit;  // set from command line
extern int FreeCallBisectLimit;  // set from command line
extern llvm::CodeGenOptLevel OptLevel;  // set from command line
extern llvm::CodeGenOptLevel BackEndOptLevel;  // set from command line
const unsigned LaneWidth = 64;
extern unsigned BlockSize;  // set from command line
extern unsigned SegmentSize; // set from command line
extern unsigned BufferSegments;
extern unsigned TaskThreads;
extern unsigned SegmentThreads;
extern unsigned ScanBlocks;
extern bool EnableObjectCache;
extern bool EnablePipelineObjectCache;
extern bool EnableDynamicMultithreading;
extern bool TraceObjectCache;
extern unsigned GroupNum;
extern const char * ProgramName;
extern llvm::TargetOptions target_Options;
extern bool TimeKernelsIsEnabled;
extern unsigned Z3_Timeout;
extern bool EnableIllustrator;
extern int IllustratorDisplay;
extern float DynamicMultithreadingAddThreshold;
extern float DynamicMultithreadingRemoveThreshold;
extern size_t DynamicMultithreadingPeriod;
extern bool UseProcessThreadForIO;

void ParseCommandLineOptions(int argc, const char *const *argv, std::initializer_list<const llvm::cl::OptionCategory *> hiding = {}, llvm::StringRef overview = "");

void AddParabixVersionPrinter();

void setTaskThreads(unsigned taskThreads);
}


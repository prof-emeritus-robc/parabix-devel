#pragma once

#include <limits.h>
#include <stdint.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>

#include <kernel/core/kernel_builder.h>
#include <kernel/core/streamset.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/pipeline/program_builder.h>

#include "entropy.h"
#include "xorshiro.h"

// using std::unique_ptr;
// using std::make_unique;
// using std::string;
// using std::to_string;

extern Entropy ent;
extern XorShift256pp rng;

extern llvm::cl::OptionCategory ExerciserFlags;

extern llvm::cl::opt<std::string> OperationName;
extern llvm::cl::opt<unsigned> OperationFieldWidth;
extern llvm::cl::list<std::string> OperationArgs;
extern llvm::cl::opt<std::string> OperationOutputFile;
extern llvm::cl::opt<bool> OperationOutputHex;
extern llvm::cl::opt<bool> QuietMode;
extern llvm::cl::opt<bool> DisableChecks;
extern llvm::cl::opt<unsigned> WarmupCount;
extern llvm::cl::opt<unsigned> RepeatCount;
extern llvm::cl::opt<unsigned> DropBestCount;
extern llvm::cl::opt<unsigned> DropWorstCount;
extern llvm::cl::opt<bool> ReportTiming;

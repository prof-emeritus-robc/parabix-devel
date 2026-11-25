/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <llvm/Support/FileSystem.h>

namespace pablo {

extern std::string ShowOptimizedPabloOption;
extern std::string ShowPabloOption;

enum PabloCompilationFlags {
    Flatten,
    DisableSimplification,
    DisableCodeMotion,
    EnableDistribution,
    EnableSchedulingPrePass,
    EnableProfiling,
    EnableTernaryOpt,
    PabloUseLLVMOptimizationPasses,
    VerifyPablo
};

enum class PabloCarryMode {
    BitBlock,
    Compressed
};
extern PabloCarryMode CarryMode;

enum class BitMovementMode {
    Advance,
    LookAhead
};

std::string BitMovementMode_string(BitMovementMode m);

extern llvm::sys::fs::OpenFlags PabloOutputFileFlag;
extern llvm::sys::fs::OpenFlags PabloOptimizedOutputFileFlag;

extern std::string PabloIllustrateKernelRegEx;

// Count the iterations of the Whiles of the Pablo kernels whose names match this regex
// (-pablo-count-while-iterations): per block, the popcount of the OR of the lowest bits
// of the conditions that start the iterations.
extern std::string PabloCountWhileRegEx;
// (Matched against a Pablo kernel's name before its annotations.)
bool countsWhileIterations(const std::string & kernelName);

// Give every eligible Pablo kernel with lookahead inputs the ProvisionalLookAheadStride attribute.
extern bool EnableProvisionalLookAheadStride;

// Let Pablo kernels with the LocalCarries attribute rebuild their carries at each segment start.
extern bool EnableLocalCarries;

// Check that each iteration of a While advances the lowest bit of its condition.
extern bool CheckWhileProgress;

// Report the CarryExtent of each compiled Pablo kernel, or the statement that prevents it.
extern bool ShowCarryExtent;
extern std::string PabloIllustrateBitstreamRegEx;

bool CompileOptionIsSet(const PabloCompilationFlags flag);

}

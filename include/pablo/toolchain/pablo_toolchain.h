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

// Give every eligible Pablo kernel with lookahead inputs the ProvisionalLookAheadStride attribute.
extern bool EnableProvisionalLookAheadStride;

// Let Pablo kernels with the RebuildCarries attribute rebuild their carries at each segment start.
extern bool EnableRebuildCarries;

// Report the CarryExtent of each compiled Pablo kernel, or the statement that prevents it.
extern bool ShowCarryExtent;
extern std::string PabloIllustrateBitstreamRegEx;

bool CompileOptionIsSet(const PabloCompilationFlags flag);

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <llvm/Support/Compiler.h>

namespace llvm { namespace cl { class OptionCategory; } }

namespace re {

extern llvm::cl::OptionCategory RE_Options;

extern bool IgnoreCaseFlag; // -i

enum RE_PrintFlags {
    ShowREs, ShowAllREs
};
    
enum RE_AlgorithmFlags {
    DisableLog2BoundedRepetition, DisableIfHierarchy, DisableMatchStar
};
    
bool LLVM_READONLY PrintOptionIsSet(RE_PrintFlags flag);

// While an object of this class exists, PrintOptionIsSet is false on the
// thread that made it: RE passes run to check an RE, not to compile it,
// print nothing.
class QuietREPrinting {
public:
    QuietREPrinting();
    ~QuietREPrinting();
};
bool LLVM_READONLY AlgorithmOptionIsSet(RE_AlgorithmFlags flag);
bool LLVM_READONLY UnicodeLevel2IsSet();

extern int IfInsertionGap;

// The maximum length of a region (between barriers) reversed by the regular
// expression engine (see kernel::RegionReversal): --reverse-region-limit.
extern unsigned ReverseRegionLimit;

// Whether the regular expression engine matches over the codes of a
// multiplexed alphabet of the CCs of an RE, rather than the 21-bit Unicode
// basis, with full Unicode indexing (see RE_PipelineBuilder::ensurePrepared):
// --re-multiplex.  Off: never; Auto (the default): where the RE allows it (it does not need
// the codepoints, as references do) and its codes need at most
// MultiplexedStreamLimit streams; Force: wherever the RE allows it.
enum class MultiplexMode {Off, Auto, Force};
extern MultiplexMode Multiplexing;
// --multiplexed-stream-limit (default 8).
extern unsigned MultiplexedStreamLimit;

std::string AnnotateWithREflags(std::string name);

}

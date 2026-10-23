/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/toolchain/toolchain.h>

#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;

namespace re {

cl::OptionCategory RE_Options("A. Regular Expression Interpretation", 
    "These options control regular expression parsing and interpretation");

bool IgnoreCaseFlag;
static cl::opt<bool, true> IgnoreCaseOption("i", cl::location(IgnoreCaseFlag), cl::desc("Ignore case distinctions in the pattern and the file."), cl::cat(re::RE_Options), cl::Grouping);
static cl::alias IgnoreCaseAlias("ignore-case", cl::desc("Alias for -i"), cl::aliasopt(IgnoreCaseOption), cl::cat(re::RE_Options));

static cl::bits<RE_PrintFlags>
    PrintOptions(cl::values(clEnumVal(ShowREs, "Show parsed regular expressions and transformations that change them"),
                            clEnumVal(ShowAllREs, "Print all regular expression passes")), cl::cat(codegen::JIT_InfoOptions));

static cl::bits<RE_AlgorithmFlags>
    AlgorithmOptions(cl::values(clEnumVal(DisableLog2BoundedRepetition, "disable log2 optimizations for bounded repetition of bytes"),
                              clEnumVal(DisableMatchStar, "disable MatchStar optimization")), cl::cat(codegen::CodeGenOptions));

static cl::opt<bool> UnicodeLevel2("U2", cl::desc("Enable Unicode Level matching under canonical and compatible (?K) equivalence."), 
    cl::Hidden, cl::cat(RE_Options));

static thread_local unsigned QuietPrintingDepth = 0;

QuietREPrinting::QuietREPrinting() {
    QuietPrintingDepth++;
}

QuietREPrinting::~QuietREPrinting() {
    QuietPrintingDepth--;
}

bool LLVM_READONLY PrintOptionIsSet(RE_PrintFlags flag) {
    return (QuietPrintingDepth == 0) && PrintOptions.isSet(flag);
}

bool LLVM_READONLY AlgorithmOptionIsSet(RE_AlgorithmFlags flag) {
    return AlgorithmOptions.isSet(flag);
}

bool LLVM_READONLY UnicodeLevel2IsSet() {
    return UnicodeLevel2;
}

const int DefaultIfInsertionGap = 3;
int IfInsertionGap;
static cl::opt<int, true>
    IfInsertionGapOption("if-insertion-gap",  cl::location(IfInsertionGap), cl::init(DefaultIfInsertionGap),
                         cl::desc("minimum number of nonempty elements between inserted if short-circuit tests"),
                         cl::cat(codegen::CodeGenOptions));

unsigned ReverseRegionLimit;
static cl::opt<unsigned, true>
    ReverseRegionLimitOption("reverse-region-limit", cl::location(ReverseRegionLimit), cl::init(1000),
                             cl::desc("maximum length of the regions (between characters the regular expression "
                                      "cannot match, and line breaks) that it reverses to match backward"),
                             cl::cat(codegen::CodeGenOptions));

MultiplexMode Multiplexing;
static cl::opt<MultiplexMode, true>
    MultiplexingOption("re-multiplex", cl::location(Multiplexing), cl::init(MultiplexMode::Auto),
                       cl::desc("match over the codes of a multiplexed alphabet of the character classes of a "
                                "regular expression, rather than the 21-bit Unicode basis"),
                       cl::values(clEnumValN(MultiplexMode::Off, "off", "never"),
                                  clEnumValN(MultiplexMode::Auto, "auto", "where the regular expression allows it "
                                             "and its codes need at most --multiplexed-stream-limit streams (default)"),
                                  clEnumValN(MultiplexMode::Force, "force", "wherever the regular expression allows it")),
                       cl::cat(codegen::CodeGenOptions));

unsigned MultiplexedStreamLimit;
static cl::opt<unsigned, true>
    MultiplexedStreamLimitOption("multiplexed-stream-limit", cl::location(MultiplexedStreamLimit), cl::init(8),
                                 cl::desc("with --re-multiplex=auto, the most streams of multiplexed codes used in "
                                          "place of the 21-bit Unicode basis"),
                                 cl::cat(codegen::CodeGenOptions));

std::string AnnotateWithREflags(std::string name) {
    if (re::AlgorithmOptionIsSet(re::DisableMatchStar)) {
        name += "-MatchStar";
    }
    if (re::AlgorithmOptionIsSet(re::DisableLog2BoundedRepetition)) {
        name += "-log2rep";
    }
    if (IfInsertionGap != DefaultIfInsertionGap) {
        name += "+ifGap="+std::to_string(IfInsertionGap);
    }
    return name;
}

} // namespace re

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#include "decomposition_modes.h"
#include <string>
#include <llvm/Support/CommandLine.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/bitwise/bixnum_kernel.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/utf8gen.h>
#include <re/adt/re_cc.h>
#include <re/cc/cc_compiler.h>

using namespace kernel;
using namespace llvm;

cl::OptionCategory & DecompositionModeOptions() {
    static cl::OptionCategory category("Decomposition Mode Options", "Masked and 21-bit decomposition.");
    return category;
}

static cl::opt<std::string> MaskSet("nfd-mask", cl::desc("Decompose only within the characters of a set (hexadecimal "
                                    "code points and ranges, as 300-36F,1E00) (NFD_Masked, focused or with -NoFocus in "
                                    "place; for testing)"), cl::init(""), cl::cat(DecompositionModeOptions()), cl::Hidden);
static cl::opt<bool> U21Mode("nfd-u21", cl::desc("Decompose on the 21-bit basis (NFD_U21): the input decoded, "
                             "decomposed (focused, or with -NoFocus in place) and encoded"),
                             cl::init(false), cl::cat(DecompositionModeOptions()));
enum class MaskSelect {All, Inside, Outside};
static cl::opt<MaskSelect> MaskSelection("nfd-mask-select", cl::desc("With -nfd-mask, print only the output inside or outside "
                                  "the output mask"),
    cl::values(clEnumValN(MaskSelect::All, "all", "all the output (default)"),
               clEnumValN(MaskSelect::Inside, "inside", "the output within the output mask"),
               clEnumValN(MaskSelect::Outside, "outside", "the output outside it")),
    cl::init(MaskSelect::All), cl::cat(DecompositionModeOptions()), cl::Hidden);

static StreamSet * selection(PipelineBuilder & P, StreamSet * OutputMask) {
    if (MaskSelection != MaskSelect::Outside) return OutputMask;
    StreamSet * const selected = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<bixnum::EQ_immediate>(OutputMask, 0, selected);
    return selected;
}

StreamSet * DecompositionModes(PipelineBuilder & P, NFD_PipelineBuilder & builder, UCD::DecompositionOptions form,
                               const FocusOptions & options, StreamSet * Bytes, StreamSet * Basis) {
    if (MaskSet.empty() && !U21Mode) return nullptr;
    StreamSet * const u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(Basis, u8index);
    StreamSet * Mask = nullptr;
    if (!MaskSet.empty()) {
        //  (The bytes of the characters of the set.)
        UCD::UnicodeSet set;
        const std::string spec = MaskSet;
        for (size_t pos = 0; pos < spec.size(); ) {
            size_t comma = spec.find(',', pos);
            if (comma == std::string::npos) comma = spec.size();
            const std::string r = spec.substr(pos, comma - pos);
            const size_t dash = r.find('-');
            const unsigned lo = std::stoul(r.substr(0, dash), nullptr, 16);
            const unsigned hi = dash == std::string::npos ? lo : std::stoul(r.substr(dash + 1), nullptr, 16);
            set = set + UCD::UnicodeSet(lo, hi);
            pos = comma + 1;
        }
        StreamSet * const firsts = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{re::makeCC(set, &cc::Unicode)}, Basis, firsts,
                                              pablo::BitMovementMode::LookAhead);
        Mask = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<U8Spans>(firsts, u8index, Mask, pablo::BitMovementMode::Advance);
    }
    StreamSet * const OutputMask = Mask && MaskSelection != MaskSelect::All ? P.CreateStreamSet(1, 1) : nullptr;
    if (U21Mode) {
        StreamSet * const U21_u8indexed = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<UTF8_Decoder>(Basis, U21_u8indexed);
        StreamSet * const U21 = P.CreateStreamSet(21, 1);
        FilterByMask(P, u8index, U21_u8indexed, U21);
        StreamSet * Mask21 = nullptr;
        if (Mask) {
            Mask21 = P.CreateStreamSet(1, 1);
            FilterByMask(P, u8index, Mask, Mask21);
        }
        StreamSet * Result = builder.NFD_U21(form, U21, Mask21, OutputMask, options.Focus);
        if (OutputMask) {
            StreamSet * const Selected = P.CreateStreamSet(21, 1);
            FilterByMask(P, selection(P, OutputMask), Result, Selected);
            Result = Selected;
        }
        StreamSet * const OutputBasis = P.CreateStreamSet(8, 1);
        U21_to_UTF8(P, Result, OutputBasis);
        StreamSet * const OutputBytes = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
        return OutputBytes;
    }
    StreamSet * OutputBytes = builder.NFD_Masked(form, options, Bytes, Basis, Mask, OutputMask);
    if (OutputMask) {
        StreamSet * const OutputBasis = P.CreateStreamSet(8, 1);
        Selected_S2P(P, OutputBytes, OutputBasis);
        StreamSet * const SelectedBasis = P.CreateStreamSet(8, 1);
        FilterByMask(P, selection(P, OutputMask), OutputBasis, SelectedBasis);
        OutputBytes = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(SelectedBasis, OutputBytes);
    }
    return OutputBytes;
}

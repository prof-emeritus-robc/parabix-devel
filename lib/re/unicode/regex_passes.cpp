/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/unicode/regex_passes.h>

#include <llvm/Support/raw_ostream.h>
#include <re/adt/adt.h>
#include <re/analysis/validation.h>
#include <re/transforms/name_lookaheads.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_minimizer.h>
#include <re/transforms/re_simplifier.h>
#include <re/transforms/re_star_normal.h>
#include <re/transforms/resolve_diffs.h>
#include <re/transforms/resolve_ranges.h>
#include <re/transforms/resolve_possessive.h>
#include <re/transforms/capture_removal.h>
#include <re/transforms/assertion_transformations.h>
#include <re/unicode/boundaries.h>
#include <re/unicode/casing.h>
#include <re/unicode/decomposition.h>
#include <re/unicode/equivalence.h>
#include <re/unicode/re_name_resolve.h>
#include <re/unicode/resolve_properties.h>
#include <re/toolchain/toolchain.h>
#include <toolchain/toolchain.h>
#include <re/printer/re_printer.h>
#include <functional>
#include <vector>

using namespace llvm;
using namespace re;

namespace re {

const char * goalName(RE_Goal goal) {
    switch (goal) {
        case RE_Goal::MatchEnds: return "MatchEnds";
        case RE_Goal::MatchSpans: return "MatchSpans";
        case RE_Goal::MatchStarts: return "MatchStarts";
        case RE_Goal::SearchReplace: return "SearchReplace";
    }
    return "?";
}

namespace {

// A pass, applied for the goals of a mask of goals (see goalBit).
struct RE_Pass {
    const char * name;
    unsigned goals;
    std::function<RE * (RE *)> apply;
};

constexpr unsigned goalBit(RE_Goal goal) {
    return 1u << static_cast<unsigned>(goal);
}

constexpr unsigned AllGoals = goalBit(RE_Goal::MatchEnds) | goalBit(RE_Goal::MatchSpans) |
                              goalBit(RE_Goal::MatchStarts) | goalBit(RE_Goal::SearchReplace);

// The captures of a search pattern referenced by its replacement must be kept,
// and passes that may restructure captures are not yet applied to it.
constexpr unsigned AllButSearchReplace = AllGoals & ~goalBit(RE_Goal::SearchReplace);

RE * applyPasses(RE * r, RE_Goal goal, const std::vector<RE_Pass> & passes) {
    for (const RE_Pass & p : passes) {
        if (p.goals & goalBit(goal)) {
            r = p.apply(r);
        } else if ((goal != RE_Goal::MatchEnds) && (PrintOptionIsSet(ShowAllREs) || PrintOptionIsSet(ShowREs))) {
            // (The output for the default goal is that of the passes applied.)
            errs() << p.name << ": skipped for goal " << goalName(goal) << '\n';
        }
    }
    return r;
}

}

RE * resolveModesAndExternalSymbols(RE * r, GrepLinesFunctionType grep, RE_Goal goal, RE * replacement) {
    if (PrintOptionIsSet(ShowAllREs) || PrintOptionIsSet(ShowREs)) {
        errs() << "Parser:\n" << Printer_RE::PrintRE(r) << '\n';
        if (goal != RE_Goal::MatchEnds) {
            errs() << "Goal: " << goalName(goal) << '\n';
        }
    }
    const std::vector<RE_Pass> passes = {
        {"resolveEscapeNames", AllGoals, [](RE * r) {return resolveEscapeNames(r);}},
        {"resolveGraphemeMode", AllGoals, [](RE * r) {
            return resolveGraphemeMode(r, false /* not in grapheme mode at top level*/);}},
        {"linkAndResolve", AllGoals, [grep](RE * r) {return UCD::linkAndResolve(r, grep);}},
        {"removeUnneededCaptures", AllButSearchReplace, [](RE * r) {return removeUnneededCaptures(r);}},
        //  The captures referenced by the search pattern itself or by the replacement.
        {"removeUnreplacedCaptures", goalBit(RE_Goal::SearchReplace), [replacement](RE * r) {
            return replacement ? removeUnneededCaptures(r, {r, replacement}) : r;}},
        {"inlineSimpleProperties", AllGoals, [](RE * r) {return UCD::inlineSimpleProperties(r);}},
        {"validateNamesDefined", AllGoals, [](RE * r) {validateNamesDefined(r); return r;}},
        {"resolveRanges", AllGoals, [](RE * r) {return resolveRanges(r);}},
        {"UnicodeLevel2", AllGoals, [](RE * r) {
            if (UnicodeLevel2IsSet() && validateAlphabet(&cc::Unicode, r)) {
                r = UCD::toNFD(r);
                r = UCD::addClusterMatches(r);
                r = UCD::addEquivalentCodepoints(r);
            }
            return r;}},
        {"resolveCaseInsensitiveMode", AllGoals, [](RE * r) {return resolveCaseInsensitiveMode(r);}},
        //  Possessive repetitions are resolved once the character classes are final
        //  (ranges and case-insensitive classes resolved), so that classes following
        //  them can be compared with those repeated.
        {"resolvePossessiveQuantifiers", AllGoals, [](RE * r) {return resolvePossessiveQuantifiers(r);}},
    };
    return applyPasses(r, goal, passes);
}

RE * regular_expression_passes(RE * re, const cc::Alphabet * lengthAlpha, RE_Goal goal) {
    //Optimization passes to simplify the AST.
    const std::vector<RE_Pass> passes = {
        {"standardizeAssertions", AllGoals, [lengthAlpha](RE * r) {return standardizeAssertions(r, lengthAlpha);}},
        {"convertToStarNormalForm", AllButSearchReplace, [](RE * r) {return convertToStarNormalForm(r);}},
        {"minimizeRE", AllButSearchReplace, [](RE * r) {
            if (codegen::OptLevel > CodeGenOptLevel::Less) {
                return minimizeRE(r);
            }
            return simplifyRE(r);}},
        {"resolveDiffs", AllGoals, [](RE * r) {return resolveDiffs(r);}},
    };
    return applyPasses(re, goal, passes);
}

} // namespace re

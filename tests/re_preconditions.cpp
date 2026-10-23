/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  The preconditions of the regular expression engine (checkPreconditions)
//  for REs that it supports and does not support, and the capture removal of
//  the goals.  Only the RE passes are run: no pipeline is compiled.

#include <kernel/re/regexp_engine.h>
#include <re/adt/adt.h>
#include <re/analysis/re_inspector.h>
#include <re/parse/parser.h>
#include <re/parse/replacement_parser.h>
#include <re/printer/re_printer.h>
#include <re/unicode/regex_passes.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>

using namespace re;

namespace {

struct PreconditionCase {
    const char * pattern;
    RE_Goal goal;
    const cc::Alphabet * sourceEncoding;
    bool supported;
};

const PreconditionCase Cases[] = {
    {"abc", RE_Goal::MatchEnds, &cc::UTF8, true},
    {"a(?=bc)", RE_Goal::MatchEnds, &cc::UTF8, true},              // fixed length
    {"x(?=ab*c)", RE_Goal::MatchEnds, &cc::UTF8, true},            // unique prefix
    {"x(?=b*c)", RE_Goal::MatchEnds, &cc::UTF8, true},             // chain (Unicode mode)
    {"x(?=b*c)", RE_Goal::MatchEnds, &cc::Unicode, true},
    {"x(?=(?:a*b)*c)", RE_Goal::MatchEnds, &cc::UTF8, true},         // no forward strategy: by reversal
    {"x(?=(?:a*b)*c)", RE_Goal::MatchSpans, &cc::Unicode, true},
    {"x(?=(?:a*b)*c$)", RE_Goal::MatchEnds, &cc::UTF8, true},        // reversal, anchored
    {"x(?=(?:a*b)*(?<!a)c)", RE_Goal::MatchEnds, &cc::UTF8, false},  // reversal: would extend the text
    {"x(?=[a-c]*(?<=c(?:a*b)*)d)", RE_Goal::MatchEnds, &cc::UTF8, true}, // reversal, a lookbehind matched forward
    {"(a)\\1", RE_Goal::MatchEnds, &cc::UTF8, true},               // fixed back reference
    {"(a+)\\1", RE_Goal::MatchEnds, &cc::UTF8, false},             // variable capture
    {"abc", RE_Goal::MatchSpans, &cc::UTF8, true},
    {"abc", RE_Goal::MatchStarts, &cc::UTF8, true},                // fixed length (byte mode)
    {"(?<=a)q[a-z]*u", RE_Goal::MatchStarts, &cc::UTF8, true},     // unique prefix
    {"[a-z]+ing", RE_Goal::MatchStarts, &cc::UTF8, true},          // chain (Unicode mode)
    {"ab(?=c+d)", RE_Goal::MatchStarts, &cc::UTF8, true},          // trailing lookahead folded
    {"[ab]{1,3}c", RE_Goal::MatchStarts, &cc::UTF8, true},         // length classes (byte mode)
    {"(?:ab|b)c*", RE_Goal::MatchStarts, &cc::UTF8, true},         // nullable suffix removed
    {"(?:ab|b){1,9}c", RE_Goal::MatchStarts, &cc::Unicode, true},  // too many length classes: by reversal
    {"(?:a*b)*c", RE_Goal::MatchStarts, &cc::UTF8, true},          // no forward strategy: by reversal
    {"(?:a*b)*c", RE_Goal::MatchStarts, &cc::Unicode, true},
    {"x(?=(?:a*b)*c)", RE_Goal::MatchStarts, &cc::UTF8, true},     // (a lookbehind of the reversed RE)
    {"(?:a*b)*c$", RE_Goal::MatchStarts, &cc::UTF8, true},         // reversal, anchored
    {"^(?:a*b)*c", RE_Goal::MatchStarts, &cc::UTF8, true},
    {"(?:a*b)*c?", RE_Goal::MatchStarts, &cc::UTF8, false},        // reversal: nullable
    {"(?<=c(?:a*b)*)(?:a*b)*d", RE_Goal::MatchStarts, &cc::UTF8, true}, // reversal, its lookbehind matched forward
    {"abc", RE_Goal::SearchReplace, &cc::UTF8, true},              // (the search pattern alone)
};

struct CaptureCounter final : public RE_Inspector {
    CaptureCounter() : RE_Inspector(NameProcessingMode::None, InspectionMode::IgnoreNonUnique) {}
    void inspectCapture(Capture * c) override {
        mCount++;
        RE_Inspector::inspectCapture(c);
    }
    unsigned mCount = 0;
};

unsigned captures(RE * r) {
    CaptureCounter counter;
    counter.inspectRE(r);
    return counter.mCount;
}

}

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv);
    unsigned errors = 0;
    for (const PreconditionCase & c : Cases) {
        RE_ModeOptions opts;
        opts.goal = c.goal;
        const PreparedRE prepared = prepareRE(RE_Parser::parse(c.pattern), opts);
        const auto failures = checkPreconditions(prepared, c.sourceEncoding);
        if (failures.empty() != c.supported) {
            errors++;
            llvm::errs() << "FAIL " << c.pattern << " (" << goalName(c.goal) << ", "
                         << c.sourceEncoding->getName() << "): expected "
                         << (c.supported ? "supported" : "unsupported") << "\n" << preconditionReport(failures);
        }
    }
    // Search and replace: the search pattern with its replacement.
    {
        struct SearchReplaceCase {
            const char * search;
            const char * replacement;
            bool supported;
            bool reversal = true;                   // see RE_ModeOptions::reversal
        };
        const SearchReplaceCase cases[] = {
            {"cat", "dog", true},
            {"[a-z]+@", "<at>", true},              // overlapping matches end together
            {"[0-9]+(?![0-9])", "#", true},
            {"cr[e\xC3\xA8]me", "cream", true},       // bounded: its strings compared
            {"(b)c", "$1$1", true},                  // a capture of one string
            {"[0-9]+", "#", false},                  // greedy or lazy
            {"ab|abc", "X", false},                  // longest or first
            {"x{2}", "X", false},                    // overlapping matches with different ends
            {"x*", "X", false},                      // empty matches
            {"([a-z]+)@", "$1", true},               // a capture of a maximal run, from the start
            {"([a-z]+)@([a-z]+)\\.", "$1-$2", true},
            {"#([a-z0-9]*)([0-9]);", "$1.$2", true},  // boundaries from the end of the match
            {"([a-z]+)@([a-z]+)\\.", "$2$1", false}, // captures out of order (moved)
            {"([a-z]+)@", "$1$1", false},            // a capture copied
            {"(([a-z])[a-z]*)@", "$1$2", false},     // a capture within another
            {"(?:(a)|b)c", "$1", false},             // a capture on one path only (not folded)
            {"([a-z])+@", "$1", false},              // a capture within a repetition
            {"#([a-z0-9]*)([0-9]+);", "$1", false},  // the end of the capture is not found
            {"([a-z0-9]*[a-z])\\d+;", "<$1>", true},    // the end of the capture by a walk back
            {"([a-z0-9]*[a-z])\\d+;", "<$1>", false, false},  // (by reversal)
        };
        for (const SearchReplaceCase & c : cases) {
            RE * const search = RE_Parser::parse(c.search);
            std::string error;
            RE * const replacement = parseReplacement(c.replacement, search, error);
            RE_ModeOptions opts;
            opts.reversal = c.reversal;
            const auto failures = checkPreconditions(prepareSearchReplace(search, replacement, opts));
            if (failures.empty() != c.supported) {
                errors++;
                llvm::errs() << "FAIL s/" << c.search << "/" << c.replacement << "/: expected "
                             << (c.supported ? "supported" : "unsupported") << "\n" << preconditionReport(failures);
            }
        }
    }
    // The captures referenced by a replacement are kept for SearchReplace only.
    RE * const search = RE_Parser::parse("([a-z]+)@([a-z]+)");
    std::string error;
    RE * const replacement = parseReplacement("<$2>", search, error);
    const unsigned forEnds = captures(resolveModesAndExternalSymbols(search, nullptr, RE_Goal::MatchEnds));
    const unsigned forReplace = captures(resolveModesAndExternalSymbols(search, nullptr, RE_Goal::SearchReplace,
                                                                        replacement));
    if ((forEnds != 0) || (forReplace != 1)) {
        errors++;
        llvm::errs() << "FAIL capture removal: " << forEnds << " captures for MatchEnds (expected 0), "
                     << forReplace << " for SearchReplace (expected 1)\n";
    }
    if (errors == 0) {
        llvm::outs() << "re_preconditions: all tests passed\n";
    }
    return errors == 0 ? 0 : 1;
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  The replacement patterns of search and replace (re::parseReplacement) and
//  their analyses (folding, validation, lengths).  No pipeline is compiled.

#include <re/adt/adt.h>
#include <re/analysis/replacement_analysis.h>
#include <re/parse/parser.h>
#include <re/parse/replacement_parser.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/raw_ostream.h>
#include <climits>
#include <string>
#include <vector>

using namespace re;

namespace {

unsigned errors = 0;

void fail(const std::string & what) {
    errors++;
    llvm::errs() << "FAIL " << what << "\n";
}

// The folded items of a replacement, printed: literals in quotes (UTF-8),
// references as <capture name>.
std::string folded(RE * replacement, RE * search) {
    std::vector<ReplacementItem> items;
    if (!foldReplacement(replacement, search, items)) return "(not a replacement)";
    std::string s;
    for (const ReplacementItem & item : items) {
        if (item.reference) {
            s += "<" + item.reference->getName() + ">";
        } else {
            s += "\"" + toUTF8String(item.literal) + "\"";
        }
    }
    return s;
}

struct ParseCase {
    const char * search;
    const char * replacement;
    const char * expected;      // the folded items, or nullptr: an error
};

const ParseCase ParseCases[] = {
    {"([a-z]+)@([a-z]+)", "x$1y", "\"x\"<\\1>\"y\""},
    {"([a-z]+)@([a-z]+)", "$2-$1", "<\\2>\"-\"<\\1>"},
    {"([a-z]+)@([a-z]+)", "${1}0", "<\\1>\"0\""},
    {"([a-z]+)@([a-z]+)", "\\2\\1", "<\\2><\\1>"},
    {"([a-z]+)@([a-z]+)", "$01", "<\\1>"},
    {"([a-z]+)", "$$\\$\\\\", "\"$$\\\""},
    {"([a-z]+)", "\\t\\x41\\x{1F600}\xC3\xA9", "\"\tA\xF0\x9F\x98\x80\xC3\xA9\""},
    {"([a-z]+)", "", ""},
    {"(a)(bc)", "[$2]", "\"[bc]\""},                 // a capture of one string, folded
    {"(x{3})y", "$1$1", "\"xxxxxx\""},
    {"(a|b)c", "$1", "<\\1>"},
    {"([a-z]+)@([a-z]+)", "$3", nullptr},
    {"([a-z]+)", "$", nullptr},
    {"([a-z]+)", "a\\", nullptr},
    {"([a-z]+)", "${1", nullptr},
    {"([a-z]+)", "${a-b}", nullptr},
    {"([a-z]+)", "${}", nullptr},
    {"([a-z]+)", "${name}", nullptr},
    {"([a-z]+)", "$x", nullptr},
    {"([a-z]+)", "\\q", nullptr},
    {"([a-z]+)", "\\x4", nullptr},
    {"([a-z]+)", "\\x{110000}", nullptr},
    {"([a-z]+)", "\\x{D800}", nullptr},
    {"([a-z]+)", "\xFF", nullptr},
    {"([a-z]+)", "\xC3", nullptr},
    {"([a-z]+)", "\xC0\x80", nullptr},           // overlong
};

}

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv);
    for (const ParseCase & c : ParseCases) {
        RE * const search = RE_Parser::parse(c.search);
        std::string error;
        RE * const replacement = parseReplacement(c.replacement, search, error);
        const std::string what = std::string("parse ") + c.search + " / " + c.replacement;
        if (c.expected == nullptr) {
            if (replacement) fail(what + ": expected an error, got " + folded(replacement, search));
        } else if (replacement == nullptr) {
            fail(what + ": " + error);
        } else if (folded(replacement, search) != c.expected) {
            fail(what + ": got " + folded(replacement, search) + ", expected " + c.expected);
        }
    }

    // Captures named as in LDML rules ("1", "2"), and named captures.
    {
        RE * const search = makeSeq({makeCapture("1", makeRep(makeCC('a', 'z'), 1, Rep::UNBOUNDED_REP)),
                                     makeCapture("tail", makeRep(makeCC('0', '9'), 1, Rep::UNBOUNDED_REP))});
        std::string error;
        RE * const replacement = parseReplacement("$1:${tail}:${1}", search, error);
        if ((replacement == nullptr) || (folded(replacement, search) != "<1>\":\"<tail>\":\"<1>")) {
            fail("LDML and named captures: " + (replacement ? folded(replacement, search) : error));
        }
    }

    // The references of a replacement follow those of the search pattern.
    {
        RE * const search = RE_Parser::parse("([a-z])\\1");
        std::string error;
        RE * const replacement = parseReplacement("$1", search, error);
        //  (A sequence of one item is the item itself.)
        Reference * const r = replacement ? llvm::dyn_cast<Reference>(replacement) : nullptr;
        if ((r == nullptr) || (r->getInstance() != 1)) fail("reference instance after a back reference");
    }

    // Unresolved references: to captures that another search pattern lacks.
    {
        RE * const search = RE_Parser::parse("([a-z]+)@([a-z]+)");
        std::string error;
        RE * const replacement = parseReplacement("$2$1", search, error);
        if (!unresolvedReferences(replacement, search).empty()) fail("unresolved references: expected none");
        if (unresolvedReferences(replacement, RE_Parser::parse("([a-z]+)")).size() != 1) {
            fail("unresolved references: expected one");
        }
    }

    // Lengths: in characters and in UTF-8 code units.
    {
        struct LengthCase {
            const char * search;
            const char * replacement;
            const cc::Alphabet * alphabet;
            int lo, hi;
        };
        const LengthCase cases[] = {
            {"([a-z]+)@(b)", "<$1:$2>", &cc::Unicode, 5, INT_MAX},
            {"([a-z]+)@(b)", "\xC3\xA9$2", &cc::Unicode, 2, 2},
            {"([a-z]+)@(b)", "\xC3\xA9$2", &cc::UTF8, 3, 3},
            {"([a-z]{2,3})", "$1$1", &cc::Unicode, 4, 6},
            {"(\xC3\xA9|e)", "$1", &cc::UTF8, 1, 2},
            {"(x)", "", &cc::Unicode, 0, 0},
        };
        for (const LengthCase & c : cases) {
            RE * const search = RE_Parser::parse(c.search);
            std::string error;
            RE * const replacement = parseReplacement(c.replacement, search, error);
            const auto range = replacement ? replacementLengthRange(replacement, search, c.alphabet)
                                           : std::make_pair(-1, -1);
            if ((range.first != c.lo) || (range.second != c.hi)) {
                fail(std::string("length ") + c.search + " / " + c.replacement + ": got " +
                     std::to_string(range.first) + ".." + std::to_string(range.second));
            }
        }
    }

    if (errors == 0) {
        llvm::outs() << "re_replacement: all tests passed\n";
    }
    return errors == 0 ? 0 : 1;
}

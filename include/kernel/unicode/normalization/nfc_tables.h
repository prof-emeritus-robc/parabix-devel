/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <map>
#include <string>
#include <vector>
#include <ucd/core/unicode_set.h>

namespace llvm { class raw_ostream; }

//  The analysis of canonical composition behind the NFC kernels, computed from
//  the Unicode Character Database (dm, dt, ccc, CE): a port of the analysis of
//  scripts/UCD/nfc_generator.py (see doc/masked-nfc-plan.md, step 1).  Each
//  member corresponds to the generator's attribute of the same name; combining
//  classes are their numeric values, as in the generator.
struct NFC_Tables {
    using codepoint_t = UCD::codepoint_t;

    //  Canonical singletons: cp -> its canonical decomposition (one character).
    std::map<codepoint_t, codepoint_t> singleton_map;
    //  Excluded composites (and singletons to them): cp -> its decomposition, the
    //  first character decomposed further while it is an excluded composite.
    std::map<codepoint_t, std::u32string> excluded_composite_map;
    //  Primary composites of two starters: cp1 -> cp2 -> precomposed.
    std::map<codepoint_t, std::map<codepoint_t, codepoint_t>> short_composable_map;
    //  The first characters of short_composable_map in the generator's (insertion) order.
    std::vector<codepoint_t> short_composable_order;
    //  Composites of a starter and a mark: mark -> starter -> replacement (one
    //  character, or with the overridable primaries, possibly two).
    std::map<codepoint_t, std::map<codepoint_t, std::u32string>> long_composable_map;
    //  The marks of long_composable_map in the generator's (insertion) order.
    std::vector<codepoint_t> long_composable_order;
    //  A -> AA, where A A composes to AA.
    std::map<codepoint_t, codepoint_t> self_composables;
    //  ccc of the second -> second -> first -> precomposed (non-excluded pairs).
    std::map<unsigned, std::map<codepoint_t, std::map<codepoint_t, codepoint_t>>> by_ccc_and_second;
    UCD::UnicodeSet composable_seconds;
    UCD::UnicodeSet non_starter_uset;
    //  A BC => AB C (or ABC, or AB CD): A -> BC (or BCD) -> replacement.
    std::map<codepoint_t, std::map<codepoint_t, std::u32string>> short_overridable_map;
    //  Overridable primaries: p -> mark -> replacement (see the generator).
    std::map<codepoint_t, std::map<codepoint_t, std::u32string>> overridable_primaries;
    //  The UTF-8 bytes to insert after a character for its NFC working space.
    std::map<codepoint_t, unsigned> max_insert_map;
    //  The long-composition passes: ccc -> pass, and the cccs of each pass.
    std::map<unsigned, unsigned> ccc_pass_allocation;
    std::vector<std::vector<unsigned>> pass_cccs;
    unsigned pass_count = 0;
    //  The short-composition passes: pass -> X -> Y -> replacement.
    std::map<unsigned, std::map<codepoint_t, std::map<codepoint_t, std::u32string>>> short_pass_map;
    int self_composable_pass_no = -1;
    unsigned short_pass_count = 1;
    //  The codes of the marks of a pass for the starters of a prefix range, where
    //  several marks share it: pass -> prefix code -> mark -> code.
    std::map<unsigned, std::map<unsigned, std::map<codepoint_t, unsigned>>> conditional_mark_codes;
    std::map<unsigned, unsigned> max_conditional_code_bits;
    //  The characters that may take part in composition, and those that need expansion.
    UCD::UnicodeSet candidate_class;
    UCD::UnicodeSet expansion_required_class;
    //  The number of applications of each long-composition pass: the longest
    //  chain of compositions with marks of the pass (p0 + m1 -> p1, p1 + m2 -> p2, ...).
    std::vector<unsigned> pass_applications;

    //  The tables of the current UCD, computed once.
    static const NFC_Tables & get();

    //  The tables in the text form of nfc_generator.py --dump-tables.
    void dump(llvm::raw_ostream & out) const;

    //  (The generator's prefix-based ranges: the code of a code point's UTF-8 lead byte.)
    static unsigned prefixCode(codepoint_t cp);
};

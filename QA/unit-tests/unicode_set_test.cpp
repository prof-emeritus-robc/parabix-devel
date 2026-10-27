/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  unicode_set_test: tests of UCD::UnicodeSet comparison.
//
//  The representation of a UnicodeSet (runs of Empty, Mixed and Full quads)
//  is not canonical: the same set may be constructed with different
//  representations.  Equality is set equality, operator< is proper subset,
//  and compare is a total order consistent with equality, independently of
//  representation.

#include <ucd/core/unicode_set.h>
#include <iostream>

using UCD::UnicodeSet;

static unsigned tests = 0;
static unsigned failures = 0;

static void check(const char * what, bool actual, bool expected) {
    tests++;
    if (actual != expected) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

int main() {
    // A range within two adjacent quads, constructed directly and as a union
    // (the direct construction has a different representation).
    const UnicodeSet range(0x0A15, 0x0A39);
    const UnicodeSet joined = UnicodeSet(0x0A15, 0x0A1F) + UnicodeSet(0x0A20, 0x0A39);

    // operator== is set equality.
    check("range == union", range == joined, true);
    check("union == range", joined == range, true);
    check("range != smaller range", range == UnicodeSet(0x0A15, 0x0A38), false);
    check("range != larger range", UnicodeSet(0x0A14, 0x0A39) == range, false);
    const UnicodeSet quads(0x40, 0x7F);
    const UnicodeSet parts = UnicodeSet(0x40, 0x50) + UnicodeSet(0x51, 0x7F);
    check("full quads == union", quads == parts, true);
    check("all == complement of empty", UnicodeSet(0, UCD::UNICODE_MAX) == ~UnicodeSet(), true);
    check("empty == difference", UnicodeSet() == (range - joined), true);
    check("empty != single", UnicodeSet() == UnicodeSet(0x41), false);
    check("single == single", UnicodeSet(0x10FFFF) == UnicodeSet(0x10FFFF, 0x10FFFF), true);

    // operator< is proper subset (a partial order).
    const UnicodeSet a(0x61);
    const UnicodeSet b(0x62);
    const UnicodeSet ab(0x61, 0x62);
    check("a < ab", a < ab, true);
    check("ab < a", ab < a, false);
    check("a < a", a < a, false);
    check("a < b (incomparable)", a < b, false);
    check("b < a (incomparable)", b < a, false);
    check("range < union (equal)", range < joined, false);
    check("smaller range < range", UnicodeSet(0x0A16, 0x0A39) < range, true);
    check("range < union with more", range < (joined + UnicodeSet(0x41)), true);
    check("empty < a", UnicodeSet() < a, true);
    check("a < all", a < UnicodeSet(0, UCD::UNICODE_MAX), true);

    // subset is independent of representation.
    check("range subset of union", range.subset(joined), true);
    check("union subset of range", joined.subset(range), true);

    // compare is a total order consistent with equality.
    check("compare equal", range.compare(joined) == 0 && joined.compare(range) == 0, true);
    check("compare a b antisymmetric", (a.compare(b) < 0) != (b.compare(a) < 0) && a.compare(b) != 0, true);
    check("compare a ab antisymmetric", (a.compare(ab) < 0) != (ab.compare(a) < 0) && a.compare(ab) != 0, true);

    std::cout << (tests - failures) << "/" << tests << " tests passed\n";
    return failures == 0 ? 0 : 1;
}

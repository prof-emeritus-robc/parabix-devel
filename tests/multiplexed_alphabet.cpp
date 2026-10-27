/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Multiplexed alphabets (cc::MultiplexedAlphabet): the partition of the
//  codepoints into the exclusive sets of a set of CCs, their codes and basis
//  CCs, and the codes of any union of exclusive sets (including complements,
//  and code 0), checked against a direct computation.  No pipeline is compiled.

#include <re/adt/re_cc.h>
#include <re/alphabet/multiplex_CCs.h>
#include <ucd/core/unicode_set.h>
#include <llvm/Support/raw_ostream.h>
#include <random>
#include <string>
#include <vector>

using namespace re;
using cc::MultiplexedAlphabet;

namespace {

unsigned errors = 0;

void fail(const std::string & what) {
    errors++;
    llvm::errs() << "FAIL " << what << "\n";
}

std::string show(const std::vector<unsigned> & codes) {
    std::string s = "{";
    for (unsigned i = 0; i < codes.size(); i++) s += (i ? "," : "") + std::to_string(codes[i]);
    return s + "}";
}

// The codes of s, or "none" if s is not expressible.
std::string codesOf(const MultiplexedAlphabet * mpx, const UCD::UnicodeSet & s) {
    std::vector<unsigned> codes;
    if (!mpx->codes(s, codes)) return "none";
    return show(codes);
}

void expect(const std::string & what, const std::string & got, const std::string & expected) {
    if (got != expected) fail(what + ": got " + got + ", expected " + expected);
}

UCD::UnicodeSet set(std::initializer_list<std::pair<UCD::codepoint_t, UCD::codepoint_t>> ranges) {
    UCD::UnicodeSet s;
    for (const auto & r : ranges) s.insert_range(r.first, r.second);
    return s;
}

// The basis CCs give the code of every codepoint, and the codes of s are
// those whose codepoints (all of them) are in s, if s is a union of
// exclusive sets.  The CCs and s are within [0, Domain) or contain all of
// [Domain - 1, UNICODE_MAX], so that Domain - 1 stands for the rest.
const UCD::codepoint_t Domain = 0x201;

void checkAgainstDirect(const std::string & what, const MultiplexedAlphabet * mpx, const UCD::UnicodeSet & s) {
    const auto & basis = mpx->getMultiplexedCCs();
    const unsigned n = mpx->getSize();
    std::vector<int> in(n, -1);
    bool expressible = true;
    for (UCD::codepoint_t cp = 0; cp < Domain; cp++) {
        const unsigned code = mpx->codeOf(cp);
        if (code >= n) {
            fail(what + ": code " + std::to_string(code) + " out of range");
            return;
        }
        unsigned fromBasis = 0;
        for (unsigned b = 0; b < basis.size(); b++) {
            if (basis[b]->contains(cp)) fromBasis |= (1u << b);
        }
        if (fromBasis != code) {
            fail(what + ": codepoint " + std::to_string(cp) + " has code " + std::to_string(code) +
                 " but basis " + std::to_string(fromBasis));
        }
        const int status = s.contains(cp) ? 1 : 0;
        if ((in[code] != -1) && (in[code] != status)) expressible = false;
        in[code] = status;
    }
    std::string expected = "none";
    if (expressible) {
        std::vector<unsigned> codes;
        for (unsigned c = 0; c < n; c++) if (in[c] == 1) codes.push_back(c);
        expected = show(codes);
    }
    expect(what, codesOf(mpx, s), expected);
    if (mpx->expressible(s) != expressible) fail(what + ": expressible() disagrees");
}

UCD::UnicodeSet randomSet(std::mt19937 & rng) {
    UCD::UnicodeSet s;
    std::uniform_int_distribution<UCD::codepoint_t> point(0, Domain - 2);
    std::uniform_int_distribution<unsigned> width(0, 12);
    const unsigned ranges = std::uniform_int_distribution<unsigned>(1, 4)(rng);
    for (unsigned i = 0; i < ranges; i++) {
        const UCD::codepoint_t lo = point(rng);
        s.insert_range(lo, std::min<UCD::codepoint_t>(lo + width(rng), Domain - 2));
    }
    return s;
}

} // namespace

int main(int, char **) {
    // a-f and 0-3 are in two CCs; the codes are numbered in codepoint order:
    // 0-3 → 1, 4-9 → 2, a-f → 3, g-z → 4.
    CC * const lower = makeCC('a', 'z');
    CC * const digit = makeCC('0', '9');
    CC * const hex = makeCC(makeCC('a', 'f'), makeCC('0', '3'));
    MultiplexedAlphabet * const mpx = cc::makeMultiplexedAlphabet("mpx", {lower, digit, hex});
    expect("size", std::to_string(mpx->getSize()), "5");
    expect("basis", std::to_string(mpx->getMultiplexedCCs().size()), "3");
    expect("code of '5'", std::to_string(mpx->codeOf('5')), "2");
    expect("code of '!'", std::to_string(mpx->codeOf('!')), "0");
    expect("code of max", std::to_string(mpx->codeOf(UCD::UNICODE_MAX)), "0");
    expect("[a-z]", codesOf(mpx, *lower), "{3,4}");
    expect("[a-f0-3]", codesOf(mpx, *hex), "{1,3}");
    expect("[a-z0-9]", codesOf(mpx, *lower + *digit), "{1,2,3,4}");
    expect("[a-f]", codesOf(mpx, *lower & *hex), "{3}");
    expect("[^a-z0-9]", codesOf(mpx, ~(*lower + *digit)), "{0}");
    expect("[^a-z]", codesOf(mpx, ~*lower), "{0,1,2}");
    expect("all", codesOf(mpx, ~UCD::UnicodeSet()), "{0,1,2,3,4}");
    expect("empty", codesOf(mpx, UCD::UnicodeSet()), "{}");
    expect("[a-c]", codesOf(mpx, set({{'a', 'c'}})), "none");
    expect("[a-z0]", codesOf(mpx, *lower + set({{'0', '0'}})), "none");
    expect("[^a]", codesOf(mpx, ~set({{'a', 'a'}})), "none");
    CC * const t = mpx->transformCC(hex);
    if (t->getAlphabet() != mpx) fail("transformCC: alphabet");
    expect("transformCC", t->canonicalName(), "mpx_1,3");
    expect("transformCC complement", mpx->transformCC(makeCC(~*lower, &cc::Unicode))->canonicalName(), "mpx_0_2");

    // An exclusive set of several ranges: a and z (code 1), b-c and x-y (code 2).
    MultiplexedAlphabet * const split = cc::makeMultiplexedAlphabet("split",
        {makeCC(makeCC('a', 'c'), makeCC('x', 'z')), makeCC('b', 'y')});
    expect("split size", std::to_string(split->getSize()), "4");
    expect("split {a}", codesOf(split, set({{'a', 'a'}})), "none");
    expect("split {a,z}", codesOf(split, set({{'a', 'a'}, {'z', 'z'}})), "{1}");
    expect("split [b-y]", codesOf(split, set({{'b', 'y'}})), "{2,3}");
    expect("split [x-y]", codesOf(split, set({{'x', 'y'}})), "none");

    // No CCs: one code, for all codepoints.
    MultiplexedAlphabet * const none = cc::makeMultiplexedAlphabet("none", {});
    expect("none size", std::to_string(none->getSize()), "1");
    expect("none basis", std::to_string(none->getMultiplexedCCs().size()), "0");
    expect("none all", codesOf(none, ~UCD::UnicodeSet()), "{0}");
    expect("none empty", codesOf(none, UCD::UnicodeSet()), "{}");
    expect("none {a}", codesOf(none, set({{'a', 'a'}})), "none");

    // CCs at the ends of the codepoints.
    MultiplexedAlphabet * const ends = cc::makeMultiplexedAlphabet("ends",
        {makeCC(0, 0x10), makeCC(0x10FFF0, UCD::UNICODE_MAX)});
    expect("ends code of 0", std::to_string(ends->codeOf(0)), "1");
    expect("ends code of max", std::to_string(ends->codeOf(UCD::UNICODE_MAX)), "2");
    expect("ends middle", codesOf(ends, set({{0x11, 0x10FFEF}})), "{0}");

    // The signature identifies the partition and its codes.
    MultiplexedAlphabet * const same = cc::makeMultiplexedAlphabet("same", {lower, digit, hex});
    MultiplexedAlphabet * const reordered = cc::makeMultiplexedAlphabet("reordered", {hex, lower, digit});
    if (mpx->getPartitionSignature() != same->getPartitionSignature()) fail("signature: same CCs differ");
    if (mpx->getPartitionSignature() != reordered->getPartitionSignature()) fail("signature: reordered CCs differ");
    if (mpx->getPartitionSignature() == split->getPartitionSignature()) fail("signature: different partitions agree");

    // Random CCs, and random unions of their exclusive sets, complements and
    // other sets, checked against the direct computation.
    std::mt19937 rng(20261022);
    for (unsigned trial = 0; trial < 40; trial++) {
        cc::CC_Set ccs;
        const unsigned m = std::uniform_int_distribution<unsigned>(1, 6)(rng);
        for (unsigned i = 0; i < m; i++) ccs.push_back(makeCC(randomSet(rng), &cc::Unicode));
        MultiplexedAlphabet * const a = cc::makeMultiplexedAlphabet("rand", ccs);
        const std::string what = "trial " + std::to_string(trial);
        const unsigned n = a->getSize();
        const unsigned k = a->getMultiplexedCCs().size();
        if ((n > 1) && (((1u << k) < n) || ((1u << (k - 1)) >= n))) {
            fail(what + ": " + std::to_string(k) + " basis CCs for " + std::to_string(n) + " codes");
        }
        UCD::UnicodeSet all;
        for (CC * cc : ccs) {
            checkAgainstDirect(what + " cc", a, *cc);
            checkAgainstDirect(what + " complement", a, ~*cc);
            all = all + *cc;
        }
        checkAgainstDirect(what + " union", a, all);
        checkAgainstDirect(what + " code 0", a, ~all);
        checkAgainstDirect(what + " intersection", a, *ccs[0] & ~*ccs[m - 1]);
        for (unsigned i = 0; i < 4; i++) {
            UCD::UnicodeSet u;
            for (UCD::codepoint_t cp = 0; cp < Domain; cp++) {
                if ((a->codeOf(cp) % 3) == i % 3) u.insert(cp);
            }
            if (u.contains(Domain - 1)) u.insert_range(Domain - 1, UCD::UNICODE_MAX);
            checkAgainstDirect(what + " codes mod 3", a, u);
            checkAgainstDirect(what + " random", a, randomSet(rng));
        }
    }

    if (errors == 0) {
        llvm::outs() << "multiplexed_alphabet: all tests passed\n";
    }
    return errors == 0 ? 0 : 1;
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <algorithm>
#include <iterator>
#include <map>
#include <boost/dynamic_bitset.hpp>
#include <llvm/ADT/Twine.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <re/adt/re_cc.h>
#include <re/alphabet/multiplex_CCs.h>
#include <ucd/core/unicode_set.h>

namespace cc {

//
// Breakpoints of a set of character classes (CCs): each codepoint c such that
// there is some CC in CCs such that either (a) c is in the CC and c-1 is not, or
// (b) c-1 is in the CC and c is not.  Boundary cases: if codepoint 0 is in
// some CC, then 0 is a breakpoint (codepoint -1 is not in any CC).  If codepoint
// 0x10FFFF is in some CC then 0x110000 is a breakpoint.
//
// The breakpoints may be determined by iterating through the interval
// representation of each CC.   For each interval (lo, hi), lo and hi+1 
// are breakpoints. 
//
// For each breakpoint, a bitset is computed identifying the source CCs whose
// status changes at the breakpoint.
//

using bitset = boost::dynamic_bitset<>;

std::map<UCD::codepoint_t, bitset> computeBreakpoints(const CC_Set & CCs) {
    std::map<UCD::codepoint_t, bitset> breakpoints;
    for (unsigned i = 0; i < CCs.size(); i++) {
        for (const auto range : *CCs[i]) {
            const auto lo = re::lo_codepoint(range);
            const auto hi = re::hi_codepoint(range);
            auto f = breakpoints.find(lo);
            if (f == breakpoints.end()) {
                breakpoints.emplace(lo, bitset(CCs.size()));
            }
            breakpoints[lo].set(i);
            f = breakpoints.find(hi + 1);
            if (f == breakpoints.end()) {
                breakpoints.emplace(hi+1, bitset(CCs.size()));
            }
            breakpoints[hi+1].set(i);
        }
    }
    return breakpoints;
}

//
// The partition of the codepoints into the exclusive sets of the CCs, as
// ranges with their codes (see MultiplexedAlphabet::getPartition), and the
// basis CCs of the codes.  Code 0 is the set of codepoints in no CC; the
// others are numbered in codepoint order of their first ranges.  Returns the
// number of codes, including code 0.
//
unsigned doMultiplexCCs(const CC_Set & CCs,
                        std::vector<std::pair<UCD::codepoint_t, unsigned>> & partition,
                        CC_Set & multiplexedCCs) {
    
    const auto breakpoints = computeBreakpoints(CCs);
    partition.clear();
    //
    // Exclusive set determination.   
    //
    // Set up a map from the set of source CCs for each exclusive set to the exclusive set index.

    std::map<bitset, unsigned> CC_set_to_exclusive_set_map;

    // Entry 0 is for the characters not in any of the CCs.
    CC_set_to_exclusive_set_map.emplace(bitset(CCs.size()), 0);
    if (breakpoints.empty() || (breakpoints.begin()->first > 0)) {
        partition.emplace_back(0, 0);
    }

    unsigned current_exclusive_set_idx = 0;
    unsigned multiplexed_bit_count = 0;
    bitset current_set(CCs.size());
    
    unsigned range_lo = 0;
    unsigned next_set_index = 1;
    for (auto & bkpt_entry : breakpoints) {
        if (current_exclusive_set_idx > 0) {  // We have a range entry to close for a pending exclusive set.
            unsigned range_hi = bkpt_entry.first - 1;
            for (unsigned bit = 0; bit < multiplexed_bit_count; bit++) {
                if (((current_exclusive_set_idx >> bit) & 1) == 1) {
                    multiplexedCCs[bit]->insert_range(range_lo, range_hi);
                }
            }
        }
        // Start a new range.
        range_lo = bkpt_entry.first;
        if (range_lo > UCD::UNICODE_MAX) continue; // Nothing to do for bkpt 0x110000
        current_set ^= bkpt_entry.second;
        auto idx_iter = CC_set_to_exclusive_set_map.find(current_set);
        if (idx_iter == CC_set_to_exclusive_set_map.end()) {
            // New exclusive class; assign the next sequential integer.
            current_exclusive_set_idx = next_set_index;
            next_set_index++;
            CC_set_to_exclusive_set_map.emplace(current_set, current_exclusive_set_idx);
            if ((current_exclusive_set_idx & (current_exclusive_set_idx - 1)) == 0) {
                multiplexed_bit_count++;
                multiplexedCCs.push_back(re::makeCC());
            }
        }
        else {
            current_exclusive_set_idx = idx_iter->second;
        }
        partition.emplace_back(range_lo, current_exclusive_set_idx);
    }
    return next_set_index;
}



MultiplexedAlphabet::MultiplexedAlphabet(const std::string name, const CC_Set CCs)
: Alphabet(name, name, ClassTypeId::MultiplexedAlphabet)
, mSourceAlphabet(&cc::Unicode)
, mUnicodeSets(std::move(CCs)) {
    if (!mUnicodeSets.empty()) {
        mSourceAlphabet = mUnicodeSets[0]->getAlphabet();
        for (unsigned i = 1; i < mUnicodeSets.size(); i++) {
            if (mUnicodeSets[i]->getAlphabet() != mSourceAlphabet) {
                llvm::report_fatal_error("Mismatched source alphabets for Multiplexed Alphabet");
            }
        }
    }
    mCodeCount = doMultiplexCCs(mUnicodeSets, mPartition, mMultiplexedCCs);
    llvm::raw_string_ostream sig(mPartitionSignature);
    sig.write_hex(mCodeCount);
    for (const auto & range : mPartition) {
        sig << ',';
        sig.write_hex(range.first);
        sig << ':';
        sig.write_hex(range.second);
    }
    sig.flush();
}

unsigned MultiplexedAlphabet::codeOf(UCD::codepoint_t cp) const {
    // The last range that begins at or before cp.
    auto f = std::upper_bound(mPartition.begin(), mPartition.end(), cp,
                              [](UCD::codepoint_t c, const std::pair<UCD::codepoint_t, unsigned> & range) {
                                  return c < range.first;
                              });
    return std::prev(f)->second;
}

bool MultiplexedAlphabet::codes(const UCD::UnicodeSet & s, std::vector<unsigned> & codes) const {
    // Whether each code is in s (1), not in s (0), or not yet seen (-1).  An
    // exclusive set may have several ranges, which must all agree.
    std::vector<int> in(mCodeCount, -1);
    auto interval = s.begin();
    const auto end = s.end();
    for (unsigned i = 0; i < mPartition.size(); i++) {
        const UCD::codepoint_t lo = mPartition[i].first;
        const UCD::codepoint_t hi = (i + 1 < mPartition.size()) ? mPartition[i + 1].first - 1 : UCD::UNICODE_MAX;
        while ((interval != end) && (re::hi_codepoint(*interval) < lo)) ++interval;
        int status;
        if ((interval == end) || (re::lo_codepoint(*interval) > hi)) {
            status = 0;
        } else if ((re::lo_codepoint(*interval) <= lo) && (re::hi_codepoint(*interval) >= hi)) {
            status = 1;
        } else {
            return false;   // the range is partly in s
        }
        int & code = in[mPartition[i].second];
        if ((code != -1) && (code != status)) return false;
        code = status;
    }
    codes.clear();
    for (unsigned c = 0; c < mCodeCount; c++) {
        if (in[c] == 1) codes.push_back(c);
    }
    return true;
}

bool MultiplexedAlphabet::expressible(const UCD::UnicodeSet & s) const {
    std::vector<unsigned> c;
    return codes(s, c);
}

re::CC * MultiplexedAlphabet::transformCC(const re::CC * sourceCC) const {
    if (sourceCC->getAlphabet() != mSourceAlphabet) {
        llvm::report_fatal_error("Mismatched source alphabets for transformCC");
    }
    std::vector<unsigned> c;
    if (!codes(*sourceCC, c)) {
        llvm::report_fatal_error(llvm::Twine("transformCC: ") + sourceCC->canonicalName() +
                                 " is not a union of the exclusive sets of " + getName());
    }
    UCD::UnicodeSet codeSet;
    for (const unsigned i : c) codeSet.insert(i);
    re::CC * CC_union = re::makeCC(std::move(codeSet), this);
    CC_union->sourceCC = sourceCC;
    return CC_union;
}

}

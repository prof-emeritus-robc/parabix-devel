/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Internal to the ldml library: an index of sets of characters, to find the
//  pairs of rules that may meet the necessary conditions of the analyses of
//  pairs (shape groups, findRuleOverlaps) without testing every pair.

#pragma once

#include <algorithm>
#include <vector>
#include <ucd/core/unicode_set.h>

namespace ldml {

// An index of sets of characters (the leading characters of rules, or the
// characters within their texts, see RuleOverlapAnalysis::Leading): the items
// whose sets meet a set, without testing each item.  The code points are
// divided into the intervals between the bounds of the ranges of the sets,
// each of which a set includes entirely or not at all; an item is listed in
// the intervals of its set, unless they are many (a wide set, tested
// directly).  An item may meet any set (e.g. a rule whose text and after
// context may be empty).
class SetIndex {
public:
    void add(const UCD::UnicodeSet & set, bool any = false) {
        if (any) mAnyItems.push_back(mSets.size());
        mSets.push_back(set);
        mAny.push_back(any);
    }
    void build() {
        std::vector<UCD::codepoint_t> bounds;
        for (const UCD::UnicodeSet & set : mSets) {
            for (const auto & range : set) {
                bounds.push_back(range.first);
                bounds.push_back(range.second + 1);
            }
        }
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
        mBounds = std::move(bounds);
        mMembers.assign(mBounds.size(), {});
        for (size_t i = 0; i < mSets.size(); i++) {
            if (mAny[i]) continue;
            std::vector<size_t> intervals;
            for (const auto & range : mSets[i]) {
                for (size_t k = interval(range.first); (k < mBounds.size()) && (mBounds[k] <= range.second); k++) {
                    intervals.push_back(k);
                    if (intervals.size() > MaxIntervals) break;
                }
                if (intervals.size() > MaxIntervals) break;
            }
            if (intervals.size() > MaxIntervals) {
                mWide.push_back(i);
            } else {
                for (size_t k : intervals) mMembers[k].push_back(i);
            }
        }
    }
    // The items meeting q (all of them with all), in order.
    std::vector<size_t> meeting(const UCD::UnicodeSet & q, bool all = false) const {
        std::vector<size_t> found;
        if (all) {
            for (size_t i = 0; i < mSets.size(); i++) found.push_back(i);
            return found;
        }
        //  (The items taken are marked with the number of the query.)
        mSeen.resize(mSets.size(), 0);
        const size_t query = ++mQueries;
        auto take = [&](size_t i) {
            if (mSeen[i] != query) {
                mSeen[i] = query;
                found.push_back(i);
            }
        };
        for (const auto & range : q) {
            for (size_t k = interval(range.first); (k < mBounds.size()) && (mBounds[k] <= range.second); k++) {
                for (size_t i : mMembers[k]) take(i);
            }
        }
        for (size_t i : mWide) if (mSets[i].intersects(q)) take(i);
        for (size_t i : mAnyItems) take(i);
        std::sort(found.begin(), found.end());
        return found;
    }
private:
    static constexpr size_t MaxIntervals = 64;
    // The interval of a code point (the last bound not after it; the first
    // interval for a code point before every bound).
    size_t interval(UCD::codepoint_t cp) const {
        const auto f = std::upper_bound(mBounds.begin(), mBounds.end(), cp);
        return (f == mBounds.begin()) ? 0 : static_cast<size_t>(f - mBounds.begin() - 1);
    }
    std::vector<UCD::UnicodeSet> mSets;
    std::vector<bool> mAny;
    std::vector<UCD::codepoint_t> mBounds;
    std::vector<std::vector<size_t>> mMembers;
    std::vector<size_t> mWide;
    std::vector<size_t> mAnyItems;
    mutable std::vector<size_t> mSeen;
    mutable size_t mQueries = 0;
};

}

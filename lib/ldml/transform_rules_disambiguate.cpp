/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Order disambiguation for rules with single character items.
//
//  A later rule L (key l1 ... lm, no contexts) is replaced by rules matching
//  where L matches and none of its earlier overlapping rules E matches.  The
//  earlier rules are explored together from the position of L, as automata
//  over single character items: at each position, the possible characters
//  (those of L's key item, within the key, or any character or the end of
//  the text, beyond it) are divided into classes by the items of the rules
//  still matching.  The characters of no remaining item end the matching of
//  all of them: a replacement rule is generated for the path so far.  A
//  rule whose items are all matched blocks L on that path.  Rules with
//  items before the position are explored in the same way, leftward, for
//  the paths on which they match their key.

#include <ldml/transform_rules.h>
#include "charset_analysis.h"
#include <re/adt/adt.h>
#include <algorithm>
#include <map>
#include <set>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

// A single character item of a pattern: a set, possibly optional (x?).
struct Item {
    RE * set;
    bool optional;
};

// Does a set contain strings or the text boundary (possibly through variables)?
bool hasStringsOrBoundary(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && hasStringsOrBoundary(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (isa<Seq>(a) || isa<Start>(a) || isa<End>(a) || hasStringsOrBoundary(a)) return true;
        }
    }
    return false;
}

class Disambiguator {
public:
    Disambiguator(DisambiguationStats & stats) : mStats(stats) {}

    // Whether rule L is replaced (by the pieces, possibly none if L is
    // masked by its earlier rules), given its earlier overlapping rules.
    bool disambiguate(ConversionRule * L, const std::vector<ConversionRule *> & earlier, std::vector<Rule *> & pieces);

private:
    // An earlier rule: its items from the position outward, following it
    // (including its key) or preceding it.
    struct Earlier {
        const ConversionRule * rule;
        std::vector<Item> items;
        bool after;
    };
    // A state of the exploration: an earlier rule and its next item.
    using State = std::pair<unsigned, unsigned>;

    bool isCharItem(RE * re) {
        return CharSetAnalysis::isSet(re) && !hasStringsOrBoundary(re) && !mAnalysis.setOf(re, false).empty();
    }
    bool parseItems(RE * re, std::vector<Item> & items, bool segments, std::string * reason = nullptr);
    bool parseEarlier(const ConversionRule * e, Earlier & p, std::string & reason);
    std::string itemReason(RE * e);
    void unresolved(const std::string & reason, size_t n = 1) {
        mStats.pairsUnresolved += n;
        mStats.unresolvedReasons[reason] += n;
    }
    bool possessiveIsExact(const std::vector<Item> & items);
    RE * unionOf(const std::vector<RE *> & sets) {
        return sets.size() == 1 ? sets[0] : makeAlt(sets.begin(), sets.end());
    }
    // A negated set, which also matches beyond the ends of the text.
    RE * negated(const std::vector<RE *> & sets) {
        return makeAlt({makeDiff(makeAny(), unionOf(sets)), makeStart(), makeEnd()});
    }
    RE * subtract(RE * x, const UCD::UnicodeSet & k);

    // Explore the earlier rules following the position (after) or preceding
    // it, from the k-th position outward.  For the following side, the first
    // positions are those of the key of L; rules with items preceding the
    // position that match their key become pending.  Each path on which all
    // rules fail yields the items of the path (and, following the position,
    // the rules pending).
    void explore(const std::vector<Earlier> & rules, bool after, size_t k, std::set<State> states,
                 std::vector<RE *> path, std::set<unsigned> pending,
                 std::vector<std::pair<std::vector<RE *>, std::set<unsigned>>> & out);

    DisambiguationStats & mStats;
    CharSetAnalysis mAnalysis;
    RuleOverlapAnalysis mOverlaps;
    std::vector<Item> mKey;         // the key items of L
};

// Why an element of a pattern is not a single character item.
std::string Disambiguator::itemReason(RE * e) {
    if (isa<Capture>(e)) return "segment";
    if (const Rep * rep = dyn_cast<Rep>(e)) {
        if (rep->getLB() == 0 && rep->getUB() == 1) return itemReason(rep->getRE()) + " under ?";
        return "repetition (*, + or nested)";
    }
    if (isa<Start>(e) || isa<End>(e)) return "anchor ^ or $";
    if (isa<Reference>(e)) return "segment reference";
    if (const Name * n = dyn_cast<Name>(e)) {
        if (isFunctionCall(n)) return "function call";
        if (n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) return "variable that is not a set";
    }
    if (CharSetAnalysis::isSet(e)) {
        if (mAnalysis.setOf(e, false).empty()) return "set of strings only";
        return "set with strings or [$]";
    }
    return "other";
}

//  The single character items of a pattern; segments (which do not affect
//  matching) are included if segments is set.
bool Disambiguator::parseItems(RE * re, std::vector<Item> & items, bool segments, std::string * reason) {
    if (re == nullptr) return true;
    std::vector<RE *> elements;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        elements.assign(seq->begin(), seq->end());
    } else {
        elements.push_back(re);
    }
    for (RE * e : elements) {
        if (Capture * c = dyn_cast<Capture>(e)) {
            if (segments && parseItems(c->getCapturedRE(), items, segments, reason)) continue;
            if (reason && reason->empty()) *reason = itemReason(e);
            return false;
        } else if (Rep * rep = dyn_cast<Rep>(e)) {
            if (rep->getLB() != 0 || rep->getUB() != 1 || !isCharItem(rep->getRE())) {
                if (reason) *reason = itemReason(e);
                return false;
            }
            items.push_back(Item{rep->getRE(), true});
        } else if (isCharItem(e)) {
            items.push_back(Item{e, false});
        } else {
            if (reason) *reason = itemReason(e);
            return false;
        }
    }
    return true;
}

bool Disambiguator::parseEarlier(const ConversionRule * e, Earlier & p, std::string & reason) {
    const RuleSide * src = e->getLeftSide();
    std::vector<Item> text;
    std::string why;
    p.rule = e;
    if (!parseItems(src->getText(), text, true, &why)) {
        reason = "E: text to replace has a " + why;
        return false;
    }
    if (text.empty() || text[0].optional) {
        reason = "E: text to replace is empty or optional";
        return false;
    }
    p.items = text;
    if (text.size() > 1 || src->hasAfterContext()) {
        if (src->hasBeforeContext()) {
            reason = "E: items both before and after the position";
            return false;
        }
        if (!parseItems(src->getAfterContext(), p.items, true, &why)) {
            reason = "E: after context has a " + why;
            return false;
        }
        p.after = true;
    } else if (src->hasBeforeContext()) {
        std::vector<Item> before;
        if (!parseItems(src->getBeforeContext(), before, true, &why)) {
            reason = "E: before context has a " + why;
            return false;
        }
        // The key, then the items preceding the position, outward.
        p.items.insert(p.items.end(), before.rbegin(), before.rend());
        p.after = false;
    } else {
        p.after = true;
    }
    if (!possessiveIsExact(p.after ? p.items : std::vector<Item>(p.items.begin() + 1, p.items.end()))) {
        reason = "E: optional item overlaps following items (possessive)";
        return false;
    }
    return true;
}

//  Possessive matching (an optional item takes a matching character) agrees
//  with the regular expression interpretation if no optional item may match
//  a character that the following items could match were it skipped.
bool Disambiguator::possessiveIsExact(const std::vector<Item> & items) {
    for (size_t i = 0; i < items.size(); i++) {
        if (!items[i].optional) continue;
        const UCD::UnicodeSet s = mAnalysis.setOf(items[i].set, false);
        for (size_t j = i + 1; j < items.size(); j++) {
            if (s.intersects(mAnalysis.setOf(items[j].set, false))) return false;
            if (!items[j].optional) break;
        }
    }
    return true;
}

// The set x without the characters k, preserving the structure of unions.
RE * Disambiguator::subtract(RE * x, const UCD::UnicodeSet & k) {
    const UCD::UnicodeSet xs = mAnalysis.setOf(x, false);
    if ((xs - k).empty()) return nullptr;
    if (!xs.intersects(k)) return x;
    if (const CC * cc = dyn_cast<CC>(x)) {
        return makeCC(*cc - k);
    }
    if (Alt * alt = dyn_cast<Alt>(x)) {
        std::vector<RE *> members;
        for (RE * m : *alt) {
            if (RE * r = subtract(m, k)) members.push_back(r);
        }
        return members.size() == 1 ? members[0] : makeAlt(members.begin(), members.end());
    }
    return makeDiff(x, makeCC(k));
}

void Disambiguator::explore(const std::vector<Earlier> & rules, bool after, size_t k, std::set<State> states,
                            std::vector<RE *> path, std::set<unsigned> pending,
                            std::vector<std::pair<std::vector<RE *>, std::set<unsigned>>> & out) {
    // Skip optional items; rules whose items are all matched either block
    // the path or (for the key of a rule with items preceding the position)
    // become pending.
    std::vector<State> work(states.begin(), states.end());
    while (!work.empty()) {
        const State s = work.back();
        work.pop_back();
        const Earlier & e = rules[s.first];
        const size_t end = (after && !e.after) ? 1 : e.items.size();
        if (s.second < end && e.items[s.second].optional && states.insert(State{s.first, s.second + 1}).second) {
            work.push_back(State{s.first, s.second + 1});
        }
    }
    std::set<State> active;
    for (const State & s : states) {
        const Earlier & e = rules[s.first];
        const size_t end = (after && !e.after) ? 1 : e.items.size();
        if (s.second < end) {
            active.insert(s);
        } else if (after && !e.after) {
            pending.insert(s.first);
        } else {
            return;     // an earlier rule matches: L is blocked on this path
        }
    }
    if (active.empty()) {
        out.emplace_back(path, pending);
        return;
    }
    // The characters possible at this position.
    const bool inKey = after && k < mKey.size();
    UCD::UnicodeSet universe = inKey ? mAnalysis.setOf(mKey[k].set, false) : UCD::UnicodeSet(0, UCD::UNICODE_MAX);
    // The item sets of the active states and the classes of characters.
    std::vector<RE *> itemSets;
    std::vector<UCD::UnicodeSet> itemChars;
    for (const State & s : active) {
        RE * set = rules[s.first].items[s.second].set;
        if (std::find(itemSets.begin(), itemSets.end(), set) == itemSets.end()) {
            itemSets.push_back(set);
            itemChars.push_back(mAnalysis.setOf(set, false));
        }
    }
    UCD::UnicodeSet covered;
    for (const UCD::UnicodeSet & c : itemChars) covered = covered + c;
    // The characters of no item: all the rules fail.
    if (inKey) {
        if (RE * rest = subtract(mKey[k].set, covered)) {
            std::vector<RE *> p = path;
            p.push_back(rest);
            out.emplace_back(p, pending);
        }
    } else {
        std::vector<RE *> p = path;
        p.push_back(negated(itemSets));
        out.emplace_back(p, pending);
    }
    // The classes of characters of the items: those in the same item sets.
    std::map<std::vector<bool>, UCD::UnicodeSet> classes;
    UCD::UnicodeSet within = universe & covered;
    std::vector<UCD::UnicodeSet> parts{within};
    for (const UCD::UnicodeSet & c : itemChars) {
        std::vector<UCD::UnicodeSet> refined;
        for (const UCD::UnicodeSet & part : parts) {
            UCD::UnicodeSet in = part & c;
            UCD::UnicodeSet notIn = part - c;
            if (!in.empty()) refined.push_back(in);
            if (!notIn.empty()) refined.push_back(notIn);
        }
        parts = std::move(refined);
    }
    std::sort(parts.begin(), parts.end(), [](const UCD::UnicodeSet & a, const UCD::UnicodeSet & b) {
        return a.front().first < b.front().first;
    });
    for (const UCD::UnicodeSet & part : parts) {
        // The representation of the class: an item set, the key item, or the characters.
        RE * cls = nullptr;
        for (size_t i = 0; i < itemSets.size() && cls == nullptr; i++) {
            if (itemChars[i] == part) cls = itemSets[i];
        }
        if (cls == nullptr && inKey && part == universe) cls = mKey[k].set;
        if (cls == nullptr) cls = makeCC(part);
        std::set<State> next;
        for (const State & s : active) {
            if (mAnalysis.setOf(rules[s.first].items[s.second].set, false).intersects(part)) {
                next.insert(State{s.first, s.second + 1});
            }
        }
        std::vector<RE *> p = path;
        p.push_back(cls);
        explore(rules, after, k + 1, next, p, pending, out);
    }
}

bool Disambiguator::disambiguate(ConversionRule * L, const std::vector<ConversionRule *> & earlier, std::vector<Rule *> & pieces) {
    const RuleSide * src = L->getLeftSide();
    if (L->getDirection() != Direction::Forward) {
        unresolved("L: not a forward rule", earlier.size());
        return false;
    }
    if (src->hasBeforeContext() || src->hasAfterContext()) {
        unresolved(std::string("L: has ") + (src->hasBeforeContext() && src->hasAfterContext() ? "before and after contexts"
                                             : src->hasBeforeContext() ? "a before context" : "an after context"), earlier.size());
        return false;
    }
    mKey.clear();
    std::string why;
    if (!parseItems(src->getText(), mKey, false, &why)) {
        unresolved("L: text to replace has a " + why, earlier.size());
        return false;
    }
    if (mKey.empty() || std::any_of(mKey.begin(), mKey.end(), [](const Item & i) {return i.optional;})) {
        unresolved("L: text to replace is empty or has optional items", earlier.size());
        return false;
    }
    std::vector<Earlier> rules;
    for (const ConversionRule * e : earlier) {
        Earlier p;
        std::string reason;
        if (parseEarlier(e, p, reason)) {
            rules.push_back(p);
        } else {
            unresolved(reason);
        }
    }
    if (rules.empty()) return false;
    // The paths following the position on which all the rules fail.
    std::vector<std::pair<std::vector<RE *>, std::set<unsigned>>> paths;
    std::set<State> initial;
    for (unsigned j = 0; j < rules.size(); j++) initial.insert(State{j, 0});
    explore(rules, true, 0, initial, {}, {}, paths);
    pieces.clear();
    for (auto & path : paths) {
        // The key: the classes of the path within the key, then the rest of the key.
        std::vector<RE *> key(path.first.begin(), path.first.begin() + std::min(path.first.size(), mKey.size()));
        for (size_t i = key.size(); i < mKey.size(); i++) key.push_back(mKey[i].set);
        RE * afterContext = nullptr;
        if (path.first.size() > mKey.size()) {
            afterContext = makeSeq(path.first.begin() + mKey.size(), path.first.end());
        }
        // The paths preceding the position on which the pending rules fail.
        std::vector<std::pair<std::vector<RE *>, std::set<unsigned>>> befores;
        if (path.second.empty()) {
            befores.emplace_back(std::vector<RE *>{}, std::set<unsigned>{});
        } else {
            std::set<State> pendingStates;
            for (unsigned j : path.second) pendingStates.insert(State{j, 1});
            explore(rules, false, 0, pendingStates, {}, {}, befores);
        }
        for (auto & before : befores) {
            RE * beforeContext = nullptr;
            if (!before.first.empty()) {
                std::reverse(before.first.begin(), before.first.end());
                beforeContext = makeSeq(before.first.begin(), before.first.end());
            }
            RuleSide * side = RuleSide::Create(beforeContext, makeSeq(key.begin(), key.end()), false, nullptr, 0, afterContext);
            pieces.push_back(makeConversionRule(side, Direction::Forward, L->getRightSide()));
        }
    }
    // Verify that no piece overlaps a resolved rule.
    for (Rule * piece : pieces) {
        for (const Earlier & e : rules) {
            if (mOverlaps.mayOverlap(e.rule, cast<ConversionRule>(piece))) mStats.verificationFailures++;
        }
    }
    mStats.pairsResolved += rules.size();
    mStats.rulesReplaced++;
    mStats.rulesAdded += pieces.size();
    return true;
}

} // end anonymous namespace

std::vector<Rule *> DisambiguateOrder(const std::vector<Rule *> & rules, DisambiguationStats * stats) {
    DisambiguationStats localStats;
    DisambiguationStats & s = stats ? *stats : localStats;
    const std::vector<RuleOverlap> overlaps = findRuleOverlaps(rules);
    s.overlapsBefore = overlaps.size();
    std::map<size_t, std::vector<ConversionRule *>> earlierOf;
    for (const RuleOverlap & o : overlaps) {
        earlierOf[o.later].push_back(cast<ConversionRule>(rules[o.earlier]));
    }
    Disambiguator d(s);
    std::vector<Rule *> result;
    for (size_t i = 0; i < rules.size(); i++) {
        auto f = earlierOf.find(i);
        std::vector<Rule *> pieces;
        if (f != earlierOf.end() && d.disambiguate(cast<ConversionRule>(rules[i]), f->second, pieces)) {
            // No pieces: L is masked by its earlier rules.
            result.insert(result.end(), pieces.begin(), pieces.end());
            continue;
        }
        result.push_back(rules[i]);
    }
    s.overlapsAfter = findRuleOverlaps(result).size();
    return result;
}

}

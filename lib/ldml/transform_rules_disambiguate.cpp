/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Order disambiguation for rules with single character items.
//
//  A later rule L (key l1 ... lm, no contexts) is replaced by rules matching
//  where L matches and none of its earlier overlapping rules E matches.  The
//  earlier rules are explored together from the position of L, as automata
//  whose transitions are sets of characters (from sets, the characters of
//  strings in sets, and repetitions) or the text boundary: at each
//  position, the possible characters (those of L's key item, within the
//  key, or any character or the end of the text, beyond it) are divided
//  into classes by the transitions of the rules still matching.  The
//  characters of no transition end the matching of all of them: a
//  replacement rule is generated for the path so far.  A rule whose
//  automaton accepts blocks L on that path.  Classes returning to the same
//  states repeat (cls*).   Rules with items before the position are
//  explored in the same way, leftward, for the paths on which they match
//  their key.

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

// A single character item of a pattern: a set, possibly with strings and
// the text boundary, repeated from lb to ub times (ub < 0: unbounded).
struct Element {
    RE * set;                                   // the set as written
    RE * chars;                                 // its characters (or nullptr)
    UCD::UnicodeSet charSet;
    std::vector<std::vector<codepoint_t>> strings;
    bool boundary;                              // [$], ^ or $
    int lb;
    int ub;
};

// An automaton for the elements of a pattern, in the direction of matching.
struct NFA {
    enum class Kind {Epsilon, Chars, Boundary};
    struct Edge {
        unsigned to;
        Kind kind;
        RE * set;
        UCD::UnicodeSet chars;
    };
    std::vector<std::vector<Edge>> out;
    unsigned start = 0;
    unsigned accept = 0;
    unsigned addState() {
        out.emplace_back();
        return static_cast<unsigned>(out.size() - 1);
    }
    void addEdge(unsigned from, unsigned to, Kind k, RE * set = nullptr, UCD::UnicodeSet chars = UCD::UnicodeSet()) {
        out[from].push_back(Edge{to, k, set, std::move(chars)});
    }
};

// One occurrence of an element from state s to state t.
void addElement(NFA & n, const Element & e, unsigned s, unsigned t) {
    if (e.chars) n.addEdge(s, t, NFA::Kind::Chars, e.chars, e.charSet);
    if (e.boundary) n.addEdge(s, t, NFA::Kind::Boundary);
    for (const std::vector<codepoint_t> & str : e.strings) {
        unsigned cur = s;
        for (size_t i = 0; i < str.size(); i++) {
            const unsigned next = (i + 1 == str.size()) ? t : n.addState();
            n.addEdge(cur, next, NFA::Kind::Chars, makeCC(str[i]), UCD::UnicodeSet(str[i]));
            cur = next;
        }
    }
}

NFA buildNFA(const std::vector<Element> & elements) {
    NFA n;
    n.start = n.addState();
    unsigned cur = n.start;
    for (const Element & e : elements) {
        for (int i = 0; i < e.lb; i++) {
            const unsigned next = n.addState();
            addElement(n, e, cur, next);
            cur = next;
        }
        if (e.ub < 0) {
            const unsigned loop = n.addState();
            n.addEdge(cur, loop, NFA::Kind::Epsilon);
            addElement(n, e, loop, loop);
            cur = loop;
        } else {
            for (int i = e.lb; i < e.ub; i++) {
                const unsigned next = n.addState();
                addElement(n, e, cur, next);
                n.addEdge(cur, next, NFA::Kind::Epsilon);
                cur = next;
            }
        }
    }
    n.accept = cur;
    return n;
}

class Disambiguator {
public:
    Disambiguator(DisambiguationStats & stats) : mStats(stats) {}

    // Whether rule L is replaced (by the pieces, possibly none if L is
    // masked by its earlier rules), given its earlier overlapping rules.
    bool disambiguate(ConversionRule * L, const std::vector<ConversionRule *> & earlier, std::vector<Rule *> & pieces);

private:
    // An earlier rule: an automaton for its items following the position
    // (its key and after context) and, for a rule with a before context
    // (after is false), for its items preceding the position (outward).
    // A rule with a before context matching its following items becomes
    // pending: it blocks L only if its preceding items match too.
    struct Earlier {
        const ConversionRule * rule;
        bool after;
        NFA forward;
        NFA backward;
    };
    // A state of the exploration: an earlier rule and a state of its automaton.
    using State = std::pair<unsigned, unsigned>;
    using Paths = std::vector<std::pair<std::vector<RE *>, std::set<unsigned>>>;

    bool isCharItem(RE * re) {
        return CharSetAnalysis::isSet(re) && !hasStringsOrBoundary(re) && !mAnalysis.setOf(re, false).empty();
    }
    bool parseKey(RE * re, std::vector<RE *> & key, bool segments, std::string & reason);
    bool parseElements(RE * re, std::vector<Element> & elements, bool reversed, std::string & reason);
    bool setElement(RE * re, bool reversed, Element & e);
    bool parseEarlier(const ConversionRule * e, Earlier & p, std::string & reason);
    std::string itemReason(RE * e);
    void unresolved(const std::string & reason, size_t n = 1) {
        mStats.pairsUnresolved += n;
        mStats.unresolvedReasons[reason] += n;
    }
    bool possessiveIsExact(const std::vector<Element> & elements, std::string & reason);
    RE * unionOf(const std::vector<RE *> & sets) {
        if (sets.empty()) return makeCC();
        return sets.size() == 1 ? sets[0] : makeAlt(sets.begin(), sets.end());
    }
    // A negated set, which also matches beyond the ends of the text unless
    // excluding the boundary.
    RE * negated(const std::vector<RE *> & sets, bool boundary) {
        RE * complement = makeDiff(makeAny(), unionOf(sets));
        return boundary ? makeAlt({complement, makeStart(), makeEnd()}) : complement;
    }
    RE * subtract(RE * x, const UCD::UnicodeSet & k);
    const NFA & automaton(const Earlier & e, bool after) const {
        return after ? e.forward : e.backward;
    }
    void closure(const std::vector<Earlier> & rules, bool after, std::set<State> & states);

    // Explore the earlier rules following the position (after) or preceding
    // it, from the k-th position outward.  For the following side, the first
    // positions are those of the key of L; rules with items preceding the
    // position that match their key become pending.  Each path on which all
    // rules fail yields the items of the path (and, following the position,
    // the rules pending).  Returns false if the paths cannot be expressed.
    bool explore(const std::vector<Earlier> & rules, bool after, size_t k, std::set<State> states,
                 std::vector<RE *> path, std::set<unsigned> pending, bool atBoundary,
                 std::vector<std::set<State>> seen, Paths & out);

    DisambiguationStats & mStats;
    CharSetAnalysis mAnalysis;
    RuleOverlapAnalysis mOverlaps;
    std::vector<RE *> mAfter;       // the items of L following its position: its key, then its after context
    size_t mKeyLength = 0;          // the number of key items
    std::vector<RE *> mBefore;      // the items of L's before context, outward
    std::string mFailure;           // why the exploration failed
};

// The limits of the exploration.
constexpr size_t MaxPathLength = 64;
constexpr size_t MaxPaths = 4096;

// Why an element of a pattern is not a single character item.
std::string Disambiguator::itemReason(RE * e) {
    if (isa<Capture>(e)) return "segment";
    if (isa<Rep>(e)) return "repetition of other than a set";
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

// A variable that is not a set, whose definition is expanded into its items.
static bool isSequenceVariable(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition());
    }
    return false;
}

// The items of L: single character items (no repetitions or strings), with
// variables that are not sets expanded and, if segments is set, the items
// of segments included.
bool Disambiguator::parseKey(RE * re, std::vector<RE *> & key, bool segments, std::string & reason) {
    std::vector<RE *> elements;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        elements.assign(seq->begin(), seq->end());
    } else {
        elements.push_back(re);
    }
    for (RE * e : elements) {
        if (isSequenceVariable(e)) {
            if (!parseKey(cast<Name>(e)->getDefinition(), key, segments, reason)) return false;
            continue;
        }
        if (segments && isa<Capture>(e)) {
            if (!parseKey(cast<Capture>(e)->getCapturedRE(), key, segments, reason)) return false;
            continue;
        }
        if (!isCharItem(e)) {
            reason = itemReason(e);
            return false;
        }
        key.push_back(e);
    }
    return true;
}

//  The text to replace of L rebuilt with its items replaced by the given
//  classes (in the order of parseKey): segments are rebuilt (recording the
//  new captures) and variables that are not sets are expanded.
static RE * rebuildKey(RE * re, const std::vector<RE *> & items, size_t & next,
                       std::map<const Capture *, Capture *> & captures) {
    if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> elements;
        for (RE * e : *seq) elements.push_back(rebuildKey(e, items, next, captures));
        return makeSeq(elements.begin(), elements.end());
    }
    if (Capture * c = dyn_cast<Capture>(re)) {
        Capture * rebuilt = makeCapture(c->getName(), rebuildKey(c->getCapturedRE(), items, next, captures));
        captures.emplace(c, rebuilt);
        return rebuilt;
    }
    if (isSequenceVariable(re)) {
        return rebuildKey(cast<Name>(re)->getDefinition(), items, next, captures);
    }
    return items[next++];
}

//  A result with its references to the captures replaced by the rebuilt captures.
static RE * remapReferences(RE * re, const std::map<const Capture *, Capture *> & captures) {
    if (re == nullptr) return nullptr;
    if (Reference * ref = dyn_cast<Reference>(re)) {
        auto f = captures.find(ref->getCapture());
        return f == captures.end() ? re : makeReference(ref->getName(), f->second, ref->getInstance());
    }
    if (Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) {
            RE * arg = remapReferences(n->getDefinition(), captures);
            return (arg != n->getDefinition()) ? makeFunctionCall(getFunctionID(n), arg) : re;
        }
        return re;
    }
    if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> elements;
        bool changed = false;
        for (RE * e : *seq) {
            elements.push_back(remapReferences(e, captures));
            changed |= elements.back() != e;
        }
        return changed ? makeSeq(elements.begin(), elements.end()) : re;
    }
    return re;
}

static RuleSide * remapReferences(const RuleSide * s, const std::map<const Capture *, Capture *> & captures) {
    return RuleSide::Create(remapReferences(s->getBeforeContext(), captures), remapReferences(s->getCompletedResult(), captures),
                            s->hasCursor(), remapReferences(s->getResultToRevisit(), captures), s->getCursorOffset(),
                            remapReferences(s->getAfterContext(), captures));
}

// Collect the strings of a set (possibly through variables).
static void collectSetStrings(RE * re, std::vector<std::vector<codepoint_t>> & strings) {
    if (Alt * alt = dyn_cast<Alt>(re)) {
        for (RE * a : *alt) {
            if (Seq * seq = dyn_cast<Seq>(a)) {
                std::vector<codepoint_t> str;
                for (RE * c : *seq) str.push_back(lo_codepoint(cast<CC>(c)->front()));
                strings.push_back(str);
            } else {
                collectSetStrings(a, strings);
            }
        }
    } else if (Name * n = dyn_cast<Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition()) collectSetStrings(n->getDefinition(), strings);
    }
}

static bool hasBoundary(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && hasBoundary(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (isa<Start>(a) || isa<End>(a) || hasBoundary(a)) return true;
        }
    }
    return false;
}

// The characters of a set without its strings and text boundary.
static RE * charPart(RE * re, const UCD::UnicodeSet & chars) {
    if (!hasStringsOrBoundary(re)) return re;
    if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> members;
        for (RE * a : *alt) {
            if (!isa<Seq>(a) && !isa<Start>(a) && !isa<End>(a) && !hasStringsOrBoundary(a)) {
                members.push_back(a);
            } else if (!isa<Seq>(a) && !isa<Start>(a) && !isa<End>(a)) {
                return makeCC(chars);
            }
        }
        if (members.size() == 1) return members[0];
        return makeAlt(members.begin(), members.end());
    }
    return makeCC(chars);
}

// A set as an element (strings reversed for matching right to left).
bool Disambiguator::setElement(RE * re, bool reversed, Element & e) {
    if (isa<Start>(re) || isa<End>(re)) {
        e = Element{re, nullptr, UCD::UnicodeSet(), {}, true, 1, 1};
        return true;
    }
    if (!CharSetAnalysis::isSet(re)) return false;
    e.set = re;
    e.charSet = mAnalysis.setOf(re, false);
    e.chars = e.charSet.empty() ? nullptr : charPart(re, e.charSet);
    e.strings.clear();
    collectSetStrings(re, e.strings);
    if (reversed) {
        for (auto & str : e.strings) std::reverse(str.begin(), str.end());
    }
    e.boundary = hasBoundary(re);
    e.lb = e.ub = 1;
    return e.chars || e.boundary || !e.strings.empty();
}

//  The elements of a pattern (reversed if matched right to left); segments
//  (which do not affect matching) are disregarded.
bool Disambiguator::parseElements(RE * re, std::vector<Element> & elements, bool reversed, std::string & reason) {
    if (re == nullptr) return true;
    std::vector<RE *> items;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        items.assign(seq->begin(), seq->end());
    } else {
        items.push_back(re);
    }
    if (reversed) std::reverse(items.begin(), items.end());
    for (RE * item : items) {
        if (Capture * c = dyn_cast<Capture>(item)) {
            if (!parseElements(c->getCapturedRE(), elements, reversed, reason)) return false;
            continue;
        }
        if (isSequenceVariable(item)) {
            // Variables that are not sets are expanded into their items.
            if (!parseElements(cast<Name>(item)->getDefinition(), elements, reversed, reason)) return false;
            continue;
        }
        Element e;
        if (Rep * rep = dyn_cast<Rep>(item)) {
            bool ok = setElement(rep->getRE(), reversed, e);
            if (!ok && isSequenceVariable(rep->getRE())) {
                // A repeated variable must expand to a single set.
                std::vector<Element> expanded;
                std::string why;
                ok = parseElements(cast<Name>(rep->getRE())->getDefinition(), expanded, reversed, why)
                    && expanded.size() == 1 && expanded[0].lb == 1 && expanded[0].ub == 1;
                if (ok) e = expanded[0];
            }
            if (!ok || e.boundary) {
                reason = itemReason(item);
                return false;
            }
            e.lb = rep->getLB();
            e.ub = rep->getUB() == Rep::UNBOUNDED_REP ? -1 : rep->getUB();
        } else if (!setElement(item, reversed, e)) {
            reason = itemReason(item);
            return false;
        }
        elements.push_back(e);
    }
    return true;
}

//  Possessive matching (a repetition takes all the characters it can, and a
//  set with strings takes its longest matching string) agrees with the
//  regular expression interpretation if no repeated or optional element may
//  match a character that the following elements could match, and each set
//  with strings is unambiguous (its strings are not prefixes of one another,
//  and their first characters are not characters of the set).
bool Disambiguator::possessiveIsExact(const std::vector<Element> & elements, std::string & reason) {
    auto first = [](const Element & e) {
        UCD::UnicodeSet f = e.charSet;
        for (const auto & str : e.strings) f = f + UCD::UnicodeSet(str[0]);
        return f;
    };
    for (size_t i = 0; i < elements.size(); i++) {
        const Element & e = elements[i];
        for (size_t a = 0; a < e.strings.size(); a++) {
            if (e.charSet.contains(e.strings[a][0])) {
                reason = "E: set with strings starting with its characters";
                return false;
            }
            for (size_t b = 0; b < e.strings.size(); b++) {
                const auto & x = e.strings[a];
                const auto & y = e.strings[b];
                if (a != b && x.size() <= y.size() && std::equal(x.begin(), x.end(), y.begin())) {
                    reason = "E: set with strings that are prefixes of others";
                    return false;
                }
            }
        }
        if (e.lb == e.ub) continue;
        const UCD::UnicodeSet f = first(e);
        for (size_t j = i + 1; j < elements.size(); j++) {
            if (f.intersects(first(elements[j])) || (e.boundary && elements[j].boundary)) {
                reason = "E: repeated or optional item overlaps following items (possessive)";
                return false;
            }
            if (elements[j].lb > 0) break;
        }
    }
    return true;
}

bool Disambiguator::parseEarlier(const ConversionRule * e, Earlier & p, std::string & reason) {
    const RuleSide * src = e->getLeftSide();
    std::vector<Element> text;
    std::string why;
    p.rule = e;
    if (!parseElements(src->getText(), text, false, why)) {
        reason = "E: text to replace has a " + why;
        return false;
    }
    if (text.empty() || text[0].lb == 0 || (text[0].boundary && !text[0].chars)) {
        reason = "E: text to replace is empty or optional";
        return false;
    }
    if (!parseElements(src->getAfterContext(), text, false, why)) {
        reason = "E: after context has a " + why;
        return false;
    }
    if (!possessiveIsExact(text, reason)) return false;
    p.forward = buildNFA(text);
    p.after = !src->hasBeforeContext();
    if (src->hasBeforeContext()) {
        std::vector<Element> before;
        if (!parseElements(src->getBeforeContext(), before, true, why)) {
            reason = "E: before context has a " + why;
            return false;
        }
        if (!possessiveIsExact(before, reason)) return false;
        p.backward = buildNFA(before);
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

//  The states reachable by empty transitions, keeping only those with other
//  transitions or accepting (so that equal sets represent equal futures).
void Disambiguator::closure(const std::vector<Earlier> & rules, bool after, std::set<State> & states) {
    std::vector<State> work(states.begin(), states.end());
    while (!work.empty()) {
        const State s = work.back();
        work.pop_back();
        for (const NFA::Edge & e : automaton(rules[s.first], after).out[s.second]) {
            if (e.kind == NFA::Kind::Epsilon && states.insert(State{s.first, e.to}).second) {
                work.push_back(State{s.first, e.to});
            }
        }
    }
    for (auto i = states.begin(); i != states.end(); ) {
        const NFA & n = automaton(rules[i->first], after);
        const auto & edges = n.out[i->second];
        const bool keep = i->second == n.accept ||
            std::any_of(edges.begin(), edges.end(), [](const NFA::Edge & e) {return e.kind != NFA::Kind::Epsilon;});
        i = keep ? std::next(i) : states.erase(i);
    }
}

bool Disambiguator::explore(const std::vector<Earlier> & rules, bool after, size_t k, std::set<State> states,
                            std::vector<RE *> path, std::set<unsigned> pending, bool atBoundary,
                            std::vector<std::set<State>> seen, Paths & out) {
    if (path.size() > MaxPathLength || out.size() > MaxPaths) {
        mFailure = "exploration limit";
        return false;
    }
    closure(rules, after, states);
    // Rules whose items are all matched either block the path or (for the
    // key of a rule with items preceding the position) become pending.
    std::set<State> active;
    for (const State & s : states) {
        const Earlier & e = rules[s.first];
        if (s.second == automaton(e, after).accept) {
            if (after && !e.after) {
                pending.insert(s.first);
                continue;
            }
            return true;    // an earlier rule matches: L is blocked on this path
        }
        active.insert(s);
    }
    // The transitions of the active states.
    std::vector<RE *> edgeSets;
    std::vector<UCD::UnicodeSet> edgeChars;
    std::set<State> boundaryNext;
    for (const State & s : active) {
        for (const NFA::Edge & e : automaton(rules[s.first], after).out[s.second]) {
            if (e.kind == NFA::Kind::Chars && !atBoundary) {
                if (std::find(edgeSets.begin(), edgeSets.end(), e.set) == edgeSets.end()) {
                    edgeSets.push_back(e.set);
                    edgeChars.push_back(e.chars);
                }
            } else if (e.kind == NFA::Kind::Boundary) {
                boundaryNext.insert(State{s.first, e.to});
            }
        }
    }
    // Within the items of L, the characters are restricted to those of L.
    const std::vector<RE *> & lItems = after ? mAfter : mBefore;
    const bool inKey = k < lItems.size();
    if (inKey) boundaryNext.clear();
    if (atBoundary) {
        // Beyond the end of the text, only the text boundary may match.
        if (boundaryNext.empty()) {
            out.emplace_back(path, pending);
            return true;
        }
        std::set<State> next = boundaryNext;
        closure(rules, after, next);
        if (next == states) {
            out.emplace_back(path, pending);    // the boundary repeated: no progress
            return true;
        }
        return explore(rules, after, k, next, path, pending, true, seen, out);
    }
    if (edgeSets.empty() && boundaryNext.empty()) {
        out.emplace_back(path, pending);
        return true;
    }
    if (!inKey) {
        if (std::find(seen.begin(), seen.end(), states) != seen.end()) {
            mFailure = "E: repetition not expressible with single character items";
            return false;
        }
        seen.push_back(states);
    }
    // The classes of characters: those in the same transition sets.
    const UCD::UnicodeSet universe = inKey ? mAnalysis.setOf(lItems[k], false) : UCD::UnicodeSet(0, UCD::UNICODE_MAX);
    UCD::UnicodeSet covered;
    for (const UCD::UnicodeSet & c : edgeChars) covered = covered + c;
    std::vector<UCD::UnicodeSet> parts;
    if (!(universe & covered).empty()) parts.push_back(universe & covered);
    for (const UCD::UnicodeSet & c : edgeChars) {
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
    // The representation and the next states of each class.
    std::vector<RE *> reprs;
    std::vector<std::set<State>> nexts;
    for (const UCD::UnicodeSet & part : parts) {
        RE * cls = nullptr;
        for (size_t i = 0; i < edgeSets.size() && cls == nullptr; i++) {
            if (edgeChars[i] == part) cls = edgeSets[i];
        }
        if (cls == nullptr && inKey && part == universe) cls = lItems[k];
        if (cls == nullptr) cls = makeCC(part);
        std::set<State> next;
        for (const State & s : active) {
            for (const NFA::Edge & e : automaton(rules[s.first], after).out[s.second]) {
                if (e.kind == NFA::Kind::Chars && e.chars.intersects(part)) next.insert(State{s.first, e.to});
            }
        }
        closure(rules, after, next);
        reprs.push_back(cls);
        nexts.push_back(next);
    }
    // Classes returning to the same states repeat: cls*.
    std::vector<RE *> loop;
    if (!inKey) {
        for (size_t i = 0; i < parts.size(); i++) {
            if (nexts[i] == states) loop.push_back(reprs[i]);
        }
        if (!loop.empty()) path.push_back(makeRep(unionOf(loop), 0, Rep::UNBOUNDED_REP));
    }
    // The characters of no transition: all the rules fail.
    if (inKey) {
        if (RE * rest = subtract(lItems[k], covered)) {
            std::vector<RE *> p = path;
            p.push_back(rest);
            out.emplace_back(p, pending);
        }
    } else {
        std::vector<RE *> p = path;
        p.push_back(negated(edgeSets, boundaryNext.empty()));
        out.emplace_back(p, pending);
        // The text boundary.
        if (!boundaryNext.empty()) {
            std::vector<RE *> b = path;
            b.push_back(makeTextBoundary());
            if (!explore(rules, after, k + 1, boundaryNext, b, pending, true, seen, out)) return false;
        }
    }
    for (size_t i = 0; i < parts.size(); i++) {
        if (!inKey && nexts[i] == states) continue;     // repeated
        std::vector<RE *> p = path;
        p.push_back(reprs[i]);
        if (!explore(rules, after, k + 1, nexts[i], p, pending, false, seen, out)) return false;
    }
    return true;
}

bool Disambiguator::disambiguate(ConversionRule * L, const std::vector<ConversionRule *> & earlier, std::vector<Rule *> & pieces) {
    const RuleSide * src = L->getLeftSide();
    if (L->getDirection() != Direction::Forward) {
        unresolved("L: not a forward rule", earlier.size());
        return false;
    }
    mAfter.clear();
    mBefore.clear();
    std::string why;
    if (!parseKey(src->getText(), mAfter, true, why)) {
        unresolved("L: text to replace has a " + why, earlier.size());
        return false;
    }
    if (mAfter.empty()) {
        unresolved("L: text to replace is empty", earlier.size());
        return false;
    }
    mKeyLength = mAfter.size();
    if (src->hasAfterContext() && !parseKey(src->getAfterContext(), mAfter, false, why)) {
        unresolved("L: after context has a " + why, earlier.size());
        return false;
    }
    if (src->hasBeforeContext()) {
        if (!parseKey(src->getBeforeContext(), mBefore, false, why)) {
            unresolved("L: before context has a " + why, earlier.size());
            return false;
        }
        std::reverse(mBefore.begin(), mBefore.end());
    }
    std::vector<Earlier> rules;
    for (const ConversionRule * e : earlier) {
        Earlier p;
        std::string reason;
        if (parseEarlier(e, p, reason)) {
            rules.push_back(std::move(p));
        } else {
            unresolved(reason);
        }
    }
    if (rules.empty()) return false;
    // The paths following the position on which all the rules fail.
    Paths paths;
    std::set<State> initial;
    for (unsigned j = 0; j < rules.size(); j++) initial.insert(State{j, rules[j].forward.start});
    std::vector<std::pair<std::vector<RE *>, std::vector<RE *>>> sides;   // (before, following) paths
    bool ok = explore(rules, true, 0, initial, {}, {}, false, {}, paths);
    for (auto & path : paths) {
        if (!ok) break;
        // The paths preceding the position on which the pending rules fail.
        Paths befores;
        if (path.second.empty()) {
            befores.emplace_back(std::vector<RE *>{}, std::set<unsigned>{});
        } else {
            std::set<State> pendingStates;
            for (unsigned j : path.second) pendingStates.insert(State{j, rules[j].backward.start});
            ok = explore(rules, false, 0, pendingStates, {}, {}, false, {}, befores);
        }
        for (auto & before : befores) sides.emplace_back(before.first, path.first);
    }
    if (!ok) {
        unresolved(mFailure.compare(0, 3, "E: ") == 0 ? mFailure : "E: " + mFailure, rules.size());
        return false;
    }
    pieces.clear();
    for (auto & side : sides) {
        // The classes of the paths within the items of L, then the rest of
        // the items of L: the key, the after context and the before context.
        std::vector<RE *> following = side.second;
        for (size_t i = following.size(); i < mAfter.size(); i++) following.push_back(mAfter[i]);
        // The key keeps the structure of L's text to replace (its segments).
        std::vector<RE *> keyItems(following.begin(), following.begin() + mKeyLength);
        std::map<const Capture *, Capture *> captures;
        size_t next = 0;
        RE * key = rebuildKey(src->getText(), keyItems, next, captures);
        RE * afterContext = nullptr;
        if (following.size() > mKeyLength) {
            afterContext = makeSeq(following.begin() + mKeyLength, following.end());
        }
        std::vector<RE *> before = side.first;
        for (size_t i = before.size(); i < mBefore.size(); i++) before.push_back(mBefore[i]);
        RE * beforeContext = nullptr;
        if (!before.empty()) {
            std::reverse(before.begin(), before.end());
            beforeContext = makeSeq(before.begin(), before.end());
        }
        RuleSide * rs = RuleSide::Create(beforeContext, key, false, nullptr, 0, afterContext);
        RuleSide * result = captures.empty() ? L->getRightSide() : remapReferences(L->getRightSide(), captures);
        pieces.push_back(makeConversionRule(rs, Direction::Forward, result));
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

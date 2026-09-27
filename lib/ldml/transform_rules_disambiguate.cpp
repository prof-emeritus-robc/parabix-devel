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
    // An item of L: a single character set, repeated from lb to ub times
    // (ub < 0: unbounded).
    struct LItem {
        RE * set;
        UCD::UnicodeSet chars;
        int lb;
        int ub;
        bool boundary;      // the text boundary (^, $ or [$]), the outermost item of its side
    };
    // The progress of L on one side: its current item and the repetitions
    // of it matched (capped, so that repetitions return to the same state),
    // or done (beyond L's items).
    struct LState {
        size_t i;
        int c;
        bool done;
        bool operator==(const LState & o) const {return i == o.i && c == o.c && done == o.done;}
    };
    // A step of a path: a class of characters, matched by an item of L (or
    // beyond L's items: -1).
    struct Step {
        RE * re;
        int item;
    };
    // A path on which the earlier rules fail, the rules pending, and the
    // final state of L.
    struct Found {
        std::vector<Step> path;
        std::set<unsigned> pending;
        LState l;
    };
    using Paths = std::vector<Found>;

    bool isCharItem(RE * re) {
        return CharSetAnalysis::isSet(re) && !hasStringsOrBoundary(re) && !mAnalysis.setOf(re, false).empty();
    }
    bool parseL(RE * re, std::vector<LItem> & items, bool segments, std::string & reason);
    bool possessiveIsExactL(const std::vector<LItem> & items);
    // The items of L that may match the next character (with their counts),
    // and whether L may end before it.
    void lCandidates(const std::vector<LItem> & items, const LState & s,
                     std::vector<std::pair<size_t, int>> & candidates, bool & canEnd);
    LState lAdvance(const std::vector<LItem> & items, size_t j, int c);
    // A canonical state: fully repeated items are passed, and L beyond its
    // items is done.
    LState lNormalize(const std::vector<LItem> & items, LState s);
    // The replacement of each item of L given a path and the final state of L.
    std::vector<std::vector<RE *>> lReplacement(const std::vector<LItem> & items, const std::vector<Step> & path,
                                                const LState & final, std::vector<RE *> & beyond);
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
    // it, outward, together with the items of L on that side.  Rules with
    // items preceding the position whose following items match become
    // pending.  Each path on which all rules fail yields the steps of the
    // path (and, following the position, the rules pending).  Returns false
    // if the paths cannot be expressed.
    bool explore(const std::vector<Earlier> & rules, bool after, std::set<State> states, LState l,
                 std::vector<Step> path, std::set<unsigned> pending, bool atBoundary,
                 std::vector<std::pair<std::set<State>, LState>> seen, Paths & out);

    DisambiguationStats & mStats;
    CharSetAnalysis mAnalysis;
    RuleOverlapAnalysis mOverlaps;
    std::vector<LItem> mAfter;      // the items of L following its position: its key, then its after context
    size_t mKeyLength = 0;          // the number of key items
    std::vector<LItem> mBefore;     // the items of L's before context, outward
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

// The items of L: single character items (no strings), possibly repeated,
// with variables that are not sets expanded and, if segments is set, the
// items of segments included.
bool Disambiguator::parseL(RE * re, std::vector<LItem> & items, bool segments, std::string & reason) {
    std::vector<RE *> elements;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        elements.assign(seq->begin(), seq->end());
    } else {
        elements.push_back(re);
    }
    for (RE * e : elements) {
        if (isSequenceVariable(e)) {
            if (!parseL(cast<Name>(e)->getDefinition(), items, segments, reason)) return false;
            continue;
        }
        if (segments && isa<Capture>(e)) {
            if (!parseL(cast<Capture>(e)->getCapturedRE(), items, segments, reason)) return false;
            continue;
        }
        if (Rep * rep = dyn_cast<Rep>(e)) {
            if (!isCharItem(rep->getRE())) {
                reason = itemReason(e);
                return false;
            }
            RE * set = rep->getRE();
            items.push_back(LItem{set, mAnalysis.setOf(set, false), rep->getLB(),
                                  rep->getUB() == Rep::UNBOUNDED_REP ? -1 : rep->getUB(), false});
            continue;
        }
        if (isa<Start>(e) || isa<End>(e) || (isTextBoundary(e))) {
            items.push_back(LItem{e, UCD::UnicodeSet(), 1, 1, true});
            continue;
        }
        if (!isCharItem(e)) {
            reason = itemReason(e);
            return false;
        }
        items.push_back(LItem{e, mAnalysis.setOf(e, false), 1, 1, false});
    }
    return true;
}

//  L's matching is deterministic (as ICU's possessive matching) if no
//  repeated or optional item may match a character that the following
//  items could match.
bool Disambiguator::possessiveIsExactL(const std::vector<LItem> & items) {
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].lb == items[i].ub) continue;
        for (size_t j = i + 1; j < items.size(); j++) {
            if (items[i].chars.intersects(items[j].chars)) return false;
            if (items[j].lb > 0) break;
        }
    }
    return true;
}

void Disambiguator::lCandidates(const std::vector<LItem> & items, const LState & s,
                                std::vector<std::pair<size_t, int>> & candidates, bool & canEnd) {
    candidates.clear();
    canEnd = s.done;
    if (s.done) return;
    size_t i = s.i;
    int c = s.c;
    for (;;) {
        if (i == items.size()) {
            canEnd = true;
            return;
        }
        const LItem & item = items[i];
        if (item.ub < 0 || c < item.ub) candidates.emplace_back(i, c);
        if (c < item.lb) return;
        i++;
        c = 0;
    }
}

Disambiguator::LState Disambiguator::lAdvance(const std::vector<LItem> & items, size_t j, int c) {
    const LItem & item = items[j];
    const int cap = item.ub < 0 ? item.lb : item.ub;
    return lNormalize(items, LState{j, std::min(c + 1, cap), false});
}

Disambiguator::LState Disambiguator::lNormalize(const std::vector<LItem> & items, LState s) {
    while (!s.done) {
        if (s.i == items.size()) return LState{0, 0, true};
        const LItem & item = items[s.i];
        if (item.ub < 0 || s.c < item.ub) break;
        s.i++;
        s.c = 0;
    }
    return s;
}

std::vector<std::vector<RE *>> Disambiguator::lReplacement(const std::vector<LItem> & items, const std::vector<Step> & path,
                                                           const LState & final, std::vector<RE *> & beyond) {
    std::vector<std::vector<RE *>> replacement(items.size());
    beyond.clear();
    for (const Step & step : path) {
        if (step.item < 0) {
            beyond.push_back(step.re);
        } else {
            replacement[step.item].push_back(step.re);
        }
    }
    if (!final.done) {
        for (size_t j = final.i; j < items.size(); j++) {
            const LItem & item = items[j];
            // The rest of the current item, and the following items.
            const int c = (j == final.i) ? final.c : 0;
            const int lb = std::max(item.lb - c, 0);
            const int ub = item.ub < 0 ? -1 : item.ub - c;
            if (ub == 0) continue;
            if (lb == 1 && ub == 1) {
                replacement[j].push_back(item.set);
            } else {
                replacement[j].push_back(makeRep(item.set, lb, ub < 0 ? Rep::UNBOUNDED_REP : ub));
            }
        }
    }
    return replacement;
}

//  The text to replace of L rebuilt with its items replaced by the given
//  replacements (in the order of parseL): segments are rebuilt (recording the
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
    // ^ at the start of the text to replace (without a before context) is a
    // condition preceding the position.
    RE * textRE = src->getText();
    RE * leadingStart = nullptr;
    if (!src->hasBeforeContext()) {
        if (isa<Start>(textRE)) {
            leadingStart = textRE;
            textRE = makeSeq();
        } else if (const Seq * seq = dyn_cast<Seq>(textRE)) {
            if (!seq->empty() && isa<Start>(seq->front())) {
                leadingStart = seq->front();
                textRE = makeSeq(seq->begin() + 1, seq->end());
            }
        }
    }
    if (!parseElements(textRE, text, false, why)) {
        reason = "E: text to replace has a " + why;
        return false;
    }
    if (std::all_of(text.begin(), text.end(), [](const Element & e) {return e.lb == 0;})
            && !src->hasBeforeContext() && !src->hasAfterContext() && !leadingStart) {
        // An insertion everywhere (repeating indefinitely).
        reason = "E: empty text to replace without contexts";
        return false;
    }
    if (!parseElements(src->getAfterContext(), text, false, why)) {
        reason = "E: after context has a " + why;
        return false;
    }
    // The anchors ^ and $ must be on their own sides of the position.
    auto anchored = [](const std::vector<Element> & elements, bool start) {
        return std::any_of(elements.begin(), elements.end(), [start](const Element & e) {
            return start ? isa<Start>(e.set) : isa<End>(e.set);
        });
    };
    if (anchored(text, true)) {
        reason = "E: ^ following the position";
        return false;
    }
    if (!possessiveIsExact(text, reason)) return false;
    p.forward = buildNFA(text);
    p.after = !src->hasBeforeContext() && !leadingStart;
    if (!p.after) {
        std::vector<Element> before;
        if (leadingStart) {
            Element start;
            setElement(leadingStart, true, start);
            before.push_back(start);
        } else if (!parseElements(src->getBeforeContext(), before, true, why)) {
            reason = "E: before context has a " + why;
            return false;
        }
        if (anchored(before, false)) {
            reason = "E: $ preceding the position";
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

bool Disambiguator::explore(const std::vector<Earlier> & rules, bool after, std::set<State> states, LState l,
                            std::vector<Step> path, std::set<unsigned> pending, bool atBoundary,
                            std::vector<std::pair<std::set<State>, LState>> seen, Paths & out) {
    if (path.size() > MaxPathLength || out.size() > MaxPaths) {
        mFailure = "exploration limit";
        return false;
    }
    closure(rules, after, states);
    // Rules whose items are all matched either block the path or (for a
    // rule with items preceding the position) become pending.
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
    // The items of L that may match the next character; L may end here
    // (then any character or the text boundary may follow).
    const std::vector<LItem> & lItems = after ? mAfter : mBefore;
    std::vector<std::pair<size_t, int>> candidates;
    bool canEnd = false;
    lCandidates(lItems, l, candidates, canEnd);
    // An item of L that is the text boundary.
    int boundaryItem = -1;
    int boundaryCount = 0;
    for (auto i = candidates.begin(); i != candidates.end(); ) {
        if (lItems[i->first].boundary) {
            boundaryItem = static_cast<int>(i->first);
            boundaryCount = i->second;
            i = candidates.erase(i);
        } else {
            ++i;
        }
    }
    const bool boundaryAllowed = canEnd || boundaryItem >= 0;
    if (!boundaryAllowed) boundaryNext.clear();
    const LState done{0, 0, true};
    if (atBoundary) {
        // Beyond the end of the text, L must end (after a boundary item).
        if (boundaryItem >= 0) {
            l = lAdvance(lItems, boundaryItem, boundaryCount);
            lCandidates(lItems, l, candidates, canEnd);
        }
        if (!canEnd) return true;   // L does not match here
    }
    if (atBoundary) {
        // Beyond the end of the text, only the text boundary may match.
        if (boundaryNext.empty()) {
            out.push_back(Found{path, pending, l});
            return true;
        }
        std::set<State> next = boundaryNext;
        closure(rules, after, next);
        if (next == states) {
            out.push_back(Found{path, pending, l});     // the boundary repeated: no progress
            return true;
        }
        return explore(rules, after, next, l, path, pending, true, seen, out);
    }
    if (edgeSets.empty() && boundaryNext.empty()) {
        out.push_back(Found{path, pending, l});
        return true;
    }
    if (std::find(seen.begin(), seen.end(), std::make_pair(states, l)) != seen.end()) {
        mFailure = "E: repetition not expressible with single character items";
        return false;
    }
    seen.emplace_back(states, l);
    // The characters possible at this position.
    UCD::UnicodeSet universe;
    std::vector<RE *> candidateSets;
    for (const auto & cand : candidates) {
        universe = universe + lItems[cand.first].chars;
        candidateSets.push_back(lItems[cand.first].set);
    }
    if (canEnd) universe = UCD::UnicodeSet(0, UCD::UNICODE_MAX);
    UCD::UnicodeSet covered;
    for (const UCD::UnicodeSet & c : edgeChars) covered = covered + c;
    // The classes of characters: those in the same transition sets and the
    // same item of L.
    auto refine = [&](UCD::UnicodeSet within) {
        std::vector<UCD::UnicodeSet> parts;
        if (!within.empty()) parts.push_back(within);
        std::vector<UCD::UnicodeSet> sets = edgeChars;
        for (const auto & cand : candidates) sets.push_back(lItems[cand.first].chars);
        for (const UCD::UnicodeSet & c : sets) {
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
        return parts;
    };
    // The item of L matching a class (or -1 beyond L's items), and L's next state.
    auto lStep = [&](const UCD::UnicodeSet & part, int & item) {
        for (const auto & cand : candidates) {
            if (lItems[cand.first].chars.intersects(part)) {
                item = static_cast<int>(cand.first);
                return lAdvance(lItems, cand.first, cand.second);
            }
        }
        item = -1;
        return done;
    };
    const std::vector<UCD::UnicodeSet> parts = refine(universe & covered);
    std::vector<Step> steps;
    std::vector<std::set<State>> nexts;
    std::vector<LState> lNexts;
    for (const UCD::UnicodeSet & part : parts) {
        int item = -1;
        const LState lNext = lStep(part, item);
        RE * cls = nullptr;
        for (size_t i = 0; i < edgeSets.size() && cls == nullptr; i++) {
            if (edgeChars[i] == part) cls = edgeSets[i];
        }
        if (cls == nullptr && item >= 0 && lItems[item].chars == part) cls = lItems[item].set;
        if (cls == nullptr) cls = makeCC(part);
        std::set<State> next;
        for (const State & s : active) {
            for (const NFA::Edge & e : automaton(rules[s.first], after).out[s.second]) {
                if (e.kind == NFA::Kind::Chars && e.chars.intersects(part)) next.insert(State{s.first, e.to});
            }
        }
        closure(rules, after, next);
        steps.push_back(Step{cls, item});
        nexts.push_back(next);
        lNexts.push_back(lNext);
    }
    // Classes returning to the same states repeat: cls*.
    std::vector<RE *> loop;
    int loopItem = -1;
    for (size_t i = 0; i < parts.size(); i++) {
        if (nexts[i] == states && lNexts[i] == l) {
            loop.push_back(steps[i].re);
            loopItem = steps[i].item;
        }
    }
    if (!loop.empty()) path.push_back(Step{makeRep(unionOf(loop), 0, Rep::UNBOUNDED_REP), loopItem});
    // The characters of no transition: all the rules fail.  Within the items
    // of L, the classes of L's items; beyond them, a negated set.
    for (const UCD::UnicodeSet & part : refine(universe - covered)) {
        int item = -1;
        const LState lNext = lStep(part, item);
        if (item < 0) continue;
        std::vector<Step> p = path;
        p.push_back(Step{subtract(lItems[item].set, covered), item});
        out.push_back(Found{p, pending, lNext});
    }
    if (canEnd) {
        std::vector<RE *> excluded = edgeSets;
        excluded.insert(excluded.end(), candidateSets.begin(), candidateSets.end());
        std::vector<Step> p = path;
        p.push_back(Step{negated(excluded, boundaryNext.empty() && boundaryItem < 0), -1});
        out.push_back(Found{p, pending, done});
    }
    // The text boundary: matched by a boundary item of L, or beyond L.
    if (boundaryAllowed && (!boundaryNext.empty() || boundaryItem >= 0)) {
        const LState lNext = boundaryItem >= 0 ? lAdvance(lItems, boundaryItem, boundaryCount) : done;
        std::vector<Step> b = path;
        b.push_back(Step{makeTextBoundary(), boundaryItem});
        if (boundaryNext.empty()) {
            out.push_back(Found{b, pending, lNext});    // all the rules fail
        } else if (!explore(rules, after, boundaryNext, lNext, b, pending, true, seen, out)) {
            return false;
        }
    }
    for (size_t i = 0; i < parts.size(); i++) {
        if (nexts[i] == states && lNexts[i] == l) continue;     // repeated
        std::vector<Step> p = path;
        p.push_back(steps[i]);
        if (!explore(rules, after, nexts[i], lNexts[i], p, pending, false, seen, out)) return false;
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
    // ^ at the start of the text to replace (without a before context) is a
    // condition preceding the position.
    RE * text = src->getText();
    std::vector<LItem> leading;
    if (!src->hasBeforeContext()) {
        if (const Seq * seq = dyn_cast<Seq>(text)) {
            if (!seq->empty() && isa<Start>(seq->front())) {
                leading.push_back(LItem{seq->front(), UCD::UnicodeSet(), 1, 1, true});
                text = makeSeq(seq->begin() + 1, seq->end());
            }
        }
    }
    if (!parseL(text, mAfter, true, why)) {
        unresolved("L: text to replace has a " + why, earlier.size());
        return false;
    }
    mKeyLength = mAfter.size();
    if (src->hasAfterContext() && !parseL(src->getAfterContext(), mAfter, false, why)) {
        unresolved("L: after context has a " + why, earlier.size());
        return false;
    }
    if (src->hasBeforeContext()) {
        if (!parseL(src->getBeforeContext(), mBefore, false, why)) {
            unresolved("L: before context has a " + why, earlier.size());
            return false;
        }
        std::reverse(mBefore.begin(), mBefore.end());
    }
    mBefore.insert(mBefore.end(), leading.begin(), leading.end());
    auto boundaryOutermost = [](const std::vector<LItem> & items) {
        for (size_t i = 0; i + 1 < items.size(); i++) {
            if (items[i].boundary) return false;
        }
        return true;
    };
    if (!boundaryOutermost(mAfter) || !boundaryOutermost(mBefore) ||
            std::any_of(mAfter.begin(), mAfter.begin() + mKeyLength, [](const LItem & i) {return i.boundary;})) {
        unresolved("L: text boundary within its items", earlier.size());
        return false;
    }
    if (!possessiveIsExactL(mAfter) || !possessiveIsExactL(mBefore)) {
        unresolved("L: repeated or optional item overlaps following items (possessive)", earlier.size());
        return false;
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
    std::vector<std::pair<Found, Found>> sides;     // (preceding, following) paths
    bool ok = explore(rules, true, initial, lNormalize(mAfter, LState{0, 0, false}), {}, {}, false, {}, paths);
    for (Found & path : paths) {
        if (!ok) break;
        // The paths preceding the position on which the pending rules fail.
        Paths befores;
        if (path.pending.empty()) {
            befores.push_back(Found{{}, {}, LState{0, 0, false}});
        } else {
            std::set<State> pendingStates;
            for (unsigned j : path.pending) pendingStates.insert(State{j, rules[j].backward.start});
            ok = explore(rules, false, pendingStates, lNormalize(mBefore, LState{0, 0, false}), {}, {}, false, {}, befores);
        }
        for (Found & before : befores) sides.emplace_back(before, path);
    }
    if (!ok) {
        unresolved(mFailure.compare(0, 3, "E: ") == 0 ? mFailure : "E: " + mFailure, rules.size());
        return false;
    }
    pieces.clear();
    for (auto & side : sides) {
        // The items of L replaced by the classes of the paths: the key (with
        // L's segments), the after context and the before context.
        std::vector<RE *> beyond;
        const std::vector<std::vector<RE *>> following = lReplacement(mAfter, side.second.path, side.second.l, beyond);
        std::vector<RE *> keyItems;
        for (size_t j = 0; j < mKeyLength; j++) keyItems.push_back(makeSeq(following[j].begin(), following[j].end()));
        std::map<const Capture *, Capture *> captures;
        size_t next = 0;
        RE * key = rebuildKey(text, keyItems, next, captures);
        std::vector<RE *> afterItems;
        for (size_t j = mKeyLength; j < mAfter.size(); j++) afterItems.insert(afterItems.end(), following[j].begin(), following[j].end());
        afterItems.insert(afterItems.end(), beyond.begin(), beyond.end());
        RE * afterContext = afterItems.empty() ? nullptr : makeSeq(afterItems.begin(), afterItems.end());
        const std::vector<std::vector<RE *>> preceding = lReplacement(mBefore, side.first.path, side.first.l, beyond);
        std::vector<RE *> beforeItems;
        for (const std::vector<RE *> & r : preceding) beforeItems.insert(beforeItems.end(), r.begin(), r.end());
        beforeItems.insert(beforeItems.end(), beyond.begin(), beyond.end());
        std::reverse(beforeItems.begin(), beforeItems.end());
        RE * beforeContext = beforeItems.empty() ? nullptr : makeSeq(beforeItems.begin(), beforeItems.end());
        RuleSide * rs = RuleSide::Create(beforeContext, key, false, nullptr, 0, afterContext);
        RuleSide * result = captures.empty() ? L->getRightSide() : remapReferences(L->getRightSide(), captures);
        pieces.push_back(makeConversionRule(rs, Direction::Forward, result));
    }
    // Verify that no piece overlaps a resolved rule.
    for (Rule * piece : pieces) {
        for (const Earlier & e : rules) {
            if (mOverlaps.mayOverlap(e.rule, cast<ConversionRule>(piece))) {
                mStats.verificationFailures++;
                mStats.failedPairs.emplace_back(e.rule, piece);
            }
        }
    }
    mStats.pairsResolved += rules.size();
    mStats.rulesReplaced++;
    mStats.rulesAdded += pieces.size();
    return true;
}

//  Splitting the sets with strings or the text boundary of a rule into
//  rules for their alternatives, in ICU's order of preference (the longest
//  strings, the characters, then the text boundary), e.g.
//  [{ch}{qu}ckq] → k ; into ch → k ; qu → k ; [ckq] → k ;.  As ICU does
//  not backtrack into a set, a set is split only if no alternative is a
//  prefix (or, preceding the position, a suffix) of another, or it is the
//  outermost item of its side.
class AlternativeSplitter {
public:
    // The rules replacing r, or empty if r is not split.
    std::vector<Rule *> split(const ConversionRule * r);
private:
    struct Leaf {
        RE * re;
        std::vector<RE *> alternatives;     // empty if not split
    };
    bool splittable(RE * re) {
        return CharSetAnalysis::isSet(re) && hasStringsOrBoundary(re) && !isa<Start>(re) && !isa<End>(re)
            && !isTextBoundary(re);
    }
    bool collect(RE * re, bool segments, std::vector<Leaf> & leaves);
    bool alternatives(Leaf & leaf, bool reversed, bool outermost);
    RE * rebuild(RE * re, bool segments, const std::vector<RE *> & choice, size_t & next,
                 std::map<const Capture *, Capture *> & captures);
    CharSetAnalysis mAnalysis;
};

bool AlternativeSplitter::collect(RE * re, bool segments, std::vector<Leaf> & leaves) {
    if (re == nullptr) return true;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) {
            if (!collect(e, segments, leaves)) return false;
        }
        return true;
    }
    if (segments && isa<Capture>(re)) return collect(cast<Capture>(re)->getCapturedRE(), segments, leaves);
    if (isSequenceVariable(re)) return collect(cast<Name>(re)->getDefinition(), segments, leaves);
    if (Rep * rep = dyn_cast<Rep>(re)) {
        if (splittable(rep->getRE())) return false;     // a repeated choice cannot be split
    }
    leaves.push_back(Leaf{re, {}});
    return true;
}

bool AlternativeSplitter::alternatives(Leaf & leaf, bool reversed, bool outermost) {
    RE * re = leaf.re;
    std::vector<std::vector<codepoint_t>> strings;
    collectSetStrings(re, strings);
    std::stable_sort(strings.begin(), strings.end(), [](const std::vector<codepoint_t> & a, const std::vector<codepoint_t> & b) {
        return a.size() > b.size();
    });
    const UCD::UnicodeSet chars = mAnalysis.setOf(re, false);
    if (!outermost) {
        // No alternative may be a prefix (suffix) of another.
        for (const auto & s : strings) {
            const codepoint_t first = reversed ? s.back() : s.front();
            if (chars.contains(first)) return false;
            for (const auto & t : strings) {
                if (&t == &s || t.size() >= s.size()) continue;
                if (reversed ? std::equal(t.rbegin(), t.rend(), s.rbegin()) : std::equal(t.begin(), t.end(), s.begin())) return false;
            }
        }
    }
    for (const auto & s : strings) {
        std::vector<RE *> cps;
        for (const codepoint_t c : s) cps.push_back(makeCC(c));
        leaf.alternatives.push_back(cps.size() == 1 ? cps[0] : makeSeq(cps.begin(), cps.end()));
    }
    if (!chars.empty()) leaf.alternatives.push_back(charPart(re, chars));
    if (hasBoundary(re)) leaf.alternatives.push_back(makeTextBoundary());
    return true;
}

RE * AlternativeSplitter::rebuild(RE * re, bool segments, const std::vector<RE *> & choice, size_t & next,
                                  std::map<const Capture *, Capture *> & captures) {
    if (re == nullptr) return nullptr;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> elements;
        bool changed = false;
        for (RE * e : *seq) {
            elements.push_back(rebuild(e, segments, choice, next, captures));
            changed |= elements.back() != e;
        }
        return changed ? makeSeq(elements.begin(), elements.end()) : re;
    }
    if (segments && isa<Capture>(re)) {
        Capture * c = cast<Capture>(re);
        RE * captured = rebuild(c->getCapturedRE(), segments, choice, next, captures);
        if (captured == c->getCapturedRE()) return re;
        Capture * rebuilt = makeCapture(c->getName(), captured);
        captures.emplace(c, rebuilt);
        return rebuilt;
    }
    if (isSequenceVariable(re)) {
        RE * def = cast<Name>(re)->getDefinition();
        RE * rebuilt = rebuild(def, segments, choice, next, captures);
        return rebuilt == def ? re : rebuilt;
    }
    RE * chosen = choice[next++];
    return chosen ? chosen : re;
}

std::vector<Rule *> AlternativeSplitter::split(const ConversionRule * r) {
    if (r->getDirection() != Direction::Forward) return {};
    const RuleSide * src = r->getLeftSide();
    // The leaves preceding the position (in text order; the first is
    // outermost) and following it (the text to replace, then the after
    // context; the last is outermost).
    std::vector<Leaf> before, following;
    if (!collect(src->getBeforeContext(), false, before)) return {};
    if (!collect(src->getText(), true, following)) return {};
    const size_t keyLeaves = following.size();
    if (!collect(src->getAfterContext(), false, following)) return {};
    bool any = false;
    size_t combinations = 1;
    for (size_t i = 0; i < before.size(); i++) {
        if (!splittable(before[i].re)) continue;
        if (!alternatives(before[i], true, i == 0)) return {};
        any = true;
        combinations *= before[i].alternatives.size();
    }
    for (size_t i = 0; i < following.size(); i++) {
        if (!splittable(following[i].re)) continue;
        if (!alternatives(following[i], false, i + 1 == following.size())) return {};
        any = true;
        combinations *= following[i].alternatives.size();
    }
    if (!any || combinations > 64) return {};
    // The combinations of alternatives, in order of preference.
    std::vector<Leaf *> leaves;
    for (Leaf & l : following) leaves.push_back(&l);
    for (Leaf & l : before) leaves.push_back(&l);
    std::vector<size_t> index(leaves.size(), 0);
    std::vector<Rule *> rules;
    for (;;) {
        std::vector<RE *> choice;
        for (size_t i = 0; i < leaves.size(); i++) {
            choice.push_back(leaves[i]->alternatives.empty() ? nullptr : leaves[i]->alternatives[index[i]]);
        }
        const std::vector<RE *> followingChoice(choice.begin(), choice.begin() + following.size());
        const std::vector<RE *> beforeChoice(choice.begin() + following.size(), choice.end());
        std::map<const Capture *, Capture *> captures;
        size_t next = 0;
        RE * text = rebuild(src->getText(), true, followingChoice, next, captures);
        assert (next == keyLeaves);
        RE * afterContext = rebuild(src->getAfterContext(), false, followingChoice, next, captures);
        next = 0;
        RE * beforeContext = rebuild(src->getBeforeContext(), false, beforeChoice, next, captures);
        RuleSide * side = RuleSide::Create(beforeContext, text, false, nullptr, 0, afterContext);
        RuleSide * result = captures.empty() ? r->getRightSide() : remapReferences(r->getRightSide(), captures);
        rules.push_back(makeConversionRule(side, Direction::Forward, result));
        // The next combination (the last leaf varying fastest).
        size_t i = leaves.size();
        while (i > 0) {
            --i;
            if (leaves[i]->alternatives.empty()) continue;
            if (++index[i] < leaves[i]->alternatives.size()) break;
            index[i] = 0;
            if (i == 0) return rules;
        }
        if (std::all_of(index.begin(), index.end(), [](size_t x) {return x == 0;})) return rules;
    }
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
    AlternativeSplitter splitter;
    RuleOverlapAnalysis analysis;
    std::vector<Rule *> result;
    for (size_t i = 0; i < rules.size(); i++) {
        auto f = earlierOf.find(i);
        if (f == earlierOf.end()) {
            result.push_back(rules[i]);
            continue;
        }
        ConversionRule * L = cast<ConversionRule>(rules[i]);
        const DisambiguationStats initial = s;
        std::vector<Rule *> pieces;
        bool replaced = d.disambiguate(L, f->second, pieces);
        if (!replaced || s.pairsUnresolved != initial.pairsUnresolved) {
            // Not completely resolved: try splitting the sets of L with
            // strings or the text boundary into rules for their alternatives,
            // each disambiguated from the earlier rules and the preceding
            // alternatives that it overlaps; used only if all are resolved.
            const std::vector<Rule *> alternatives = splitter.split(L);
            if (!alternatives.empty()) {
                const DisambiguationStats first = s;
                s = initial;
                std::vector<Rule *> altPieces;
                bool ok = true;
                for (size_t k = 0; k < alternatives.size() && ok; k++) {
                    ConversionRule * A = cast<ConversionRule>(alternatives[k]);
                    std::vector<ConversionRule *> earlier;
                    for (ConversionRule * e : f->second) {
                        if (analysis.mayOverlap(e, A)) earlier.push_back(e);
                    }
                    for (size_t j = 0; j < k; j++) {
                        ConversionRule * B = cast<ConversionRule>(alternatives[j]);
                        if (analysis.mayOverlap(B, A)) earlier.push_back(B);
                    }
                    if (earlier.empty()) {
                        altPieces.push_back(A);
                        continue;
                    }
                    const size_t unresolved = s.pairsUnresolved;
                    std::vector<Rule *> p;
                    ok = d.disambiguate(A, earlier, p) && s.pairsUnresolved == unresolved;
                    altPieces.insert(altPieces.end(), p.begin(), p.end());
                }
                if (ok) {
                    pieces = altPieces;
                    replaced = true;
                    s.rulesSplit++;
                    s.splitRules += alternatives.size();
                } else {
                    s = first;
                }
            }
        }
        if (replaced) {
            // No pieces: L is masked by its earlier rules.
            result.insert(result.end(), pieces.begin(), pieces.end());
        } else {
            result.push_back(L);
        }
    }
    s.overlapsAfter = findRuleOverlaps(result).size();
    return result;
}

}

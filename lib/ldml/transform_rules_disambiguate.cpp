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
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include <algorithm>
#include <functional>
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
            if (isa<Seq>(a) || isBoundary(a) || hasStringsOrBoundary(a)) return true;
        }
    }
    return false;
}

// A single character item of a pattern: a set, possibly with strings and
// the text boundary, repeated from lb to ub times (ub < 0: unbounded).
static void collectSetStrings(RE * re, std::vector<std::vector<codepoint_t>> & strings);
static bool hasBoundary(const RE * re);

// Whether an item is the text boundary alone (^, $ or [$], possibly as a
// variable).
static bool isBoundaryItem(const RE * re) {
    while (const Name * n = dyn_cast<Name>(re)) {
        if (isTextBoundary(n)) return true;
        if (isFunctionCall(n) || n->getDefinition() == nullptr) return false;
        re = n->getDefinition();
    }
    if (isBoundary(re)) return true;
    // [$]: the boundary with no characters.
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        bool boundary = false;
        for (const RE * a : *alt) {
            if (isBoundary(a)) {
                boundary = true;
            } else if (!isa<CC>(a) || !cast<CC>(a)->empty()) {
                return false;
            }
        }
        return boundary;
    }
    return false;
}
static RE * charPart(RE * re, const UCD::UnicodeSet & chars);

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
    enum class Kind {Epsilon, Chars, Boundary, String};
    struct Edge {
        unsigned to;
        Kind kind;
        RE * set;
        UCD::UnicodeSet chars;
        std::vector<codepoint_t> str;       // a String edge: a string of a set, taken as a whole
    };
    std::vector<std::vector<Edge>> out;
    unsigned start = 0;
    unsigned accept = 0;
    unsigned addState() {
        out.emplace_back();
        return static_cast<unsigned>(out.size() - 1);
    }
    void addEdge(unsigned from, unsigned to, Kind k, RE * set = nullptr, UCD::UnicodeSet chars = UCD::UnicodeSet()) {
        out[from].push_back(Edge{to, k, set, std::move(chars), {}});
    }
    void addStringEdge(unsigned from, unsigned to, RE * set, const std::vector<codepoint_t> & str) {
        out[from].push_back(Edge{to, Kind::String, set, UCD::UnicodeSet(), str});
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
        // The characters and strings of the element are repeated; the text
        // boundary (a zero-width match) counts as one more repetition, and
        // then ends the element (as ICU's quantifiers stop on no progress).
        Element body = e;
        body.boundary = false;
        std::vector<unsigned> counts{cur};     // the states after c repetitions (c < ub)
        for (int i = 0; i < e.lb; i++) {
            const unsigned next = n.addState();
            addElement(n, body, cur, next);
            cur = next;
            counts.push_back(cur);
        }
        if (e.ub < 0) {
            const unsigned loop = n.addState();
            n.addEdge(cur, loop, NFA::Kind::Epsilon);
            addElement(n, body, loop, loop);
            cur = loop;
            counts.push_back(loop);
        } else {
            for (int i = e.lb; i < e.ub; i++) {
                const unsigned next = n.addState();
                addElement(n, body, cur, next);
                n.addEdge(cur, next, NFA::Kind::Epsilon);
                cur = next;
                counts.push_back(cur);
            }
            counts.pop_back();      // ub repetitions: no more
        }
        if (e.boundary) {
            for (size_t c = 0; c < counts.size(); c++) {
                if (static_cast<int>(c) + 1 >= e.lb) n.addEdge(counts[c], cur, NFA::Kind::Boundary);
            }
        }
    }
    n.accept = cur;
    return n;
}

//  An automaton for elements under ICU's matching.  A repeated or optional
//  element takes each alternative it can (it is passed over only where it
//  matches nothing), and a set with strings takes its longest matching
//  string, or else a character.  These choices are expressed as conditions
//  on the following text: choosing an alternative a (a string, or a
//  character) of a set requires that no longer string of the set extending
//  a follows, and passing over an element requires that none of its
//  alternatives follows.  A condition is tracked in the trie of the strings
//  of its set: it is satisfied when the text departs from the trie (or at
//  the text boundary) and violated when a string (or, for passing over, a
//  character of the set) is completed.  The states are (element,
//  repetitions matched, the node of a string being matched, the pending
//  conditions); the pattern is matched when its elements are matched and
//  no conditions are pending.
class ICUAutomatonBuilder {
public:
    ICUAutomatonBuilder(const std::vector<Element> & elements) : mElements(elements) {
        for (const Element & e : elements) {
            Trie t;
            t.nodes.emplace_back();
            for (const auto & str : e.strings) {
                int node = 0;
                for (const codepoint_t c : str) {
                    auto f = t.nodes[node].children.find(c);
                    if (f == t.nodes[node].children.end()) {
                        const int child = static_cast<int>(t.nodes.size());
                        t.nodes[node].children.emplace(c, child);
                        t.nodes.emplace_back();
                        node = child;
                    } else {
                        node = f->second;
                    }
                }
                t.nodes[node].terminal = true;
            }
            mTries.push_back(std::move(t));
        }
    }
    NFA build();
private:
    struct TrieNode {
        std::map<codepoint_t, int> children;
        bool terminal = false;
    };
    struct Trie {
        std::vector<TrieNode> nodes;
    };
    // A condition: no string of element's set below node (nor, if chars,
    // a character of the set) follows.
    struct Condition {
        size_t element;
        int node;
        bool chars;
        bool operator<(const Condition & o) const {
            return std::tie(element, node, chars) < std::tie(o.element, o.node, o.chars);
        }
        bool operator==(const Condition & o) const {
            return element == o.element && node == o.node && chars == o.chars;
        }
    };
    struct State {
        size_t i;           // the current element (elements.size(): matched)
        int c;              // repetitions matched
        int node;           // the node of a string being matched, or -1
        std::vector<Condition> pending;
        bool operator<(const State & o) const {
            return std::tie(i, c, node, pending) < std::tie(o.i, o.c, o.node, o.pending);
        }
    };
    unsigned stateOf(State s);
    // The conditions after a codepoint: false if one is violated.
    bool advance(const std::vector<Condition> & pending, codepoint_t cp, std::vector<Condition> & next) const;
    // The codepoint sets on which the conditions behave uniformly.
    void refine(const std::vector<Condition> & pending, std::vector<UCD::UnicodeSet> & parts) const;
    // Add the transitions on the characters of chars to the state with the
    // conditions pending, then (for each class, on which the conditions and
    // the sets of split behave uniformly) to target(cp, conditions).
    void addCharEdges(unsigned from, const UCD::UnicodeSet & chars, const std::vector<Condition> & pending,
                      const std::function<State(codepoint_t, std::vector<Condition>)> & target,
                      const std::vector<UCD::UnicodeSet> & split = {});
    int cap(size_t j, int c) const {
        const Element & e = mElements[j];
        return e.ub < 0 ? std::min(c + 1, e.lb) : c + 1;
    }
    State normalize(State s) const {
        while (s.node < 0 && s.i < mElements.size() && mElements[s.i].ub >= 0 && s.c >= mElements[s.i].ub) {
            s.i++;
            s.c = 0;
        }
        return s;
    }
    // The condition for choosing an alternative ending at node (none if the
    // node has no children).
    void choose(size_t j, int node, std::vector<Condition> & pending) const {
        if (!mTries[j].nodes[node].children.empty()) pending.push_back(Condition{j, node, false});
    }

    const std::vector<Element> & mElements;
    std::vector<Trie> mTries;
    NFA mNFA;
    std::map<State, unsigned> mIds;
    std::vector<State> mWork;
};

unsigned ICUAutomatonBuilder::stateOf(State s) {
    std::sort(s.pending.begin(), s.pending.end());
    s.pending.erase(std::unique(s.pending.begin(), s.pending.end()), s.pending.end());
    s = normalize(s);
    if (s.i == mElements.size() && s.node < 0 && s.pending.empty()) return mNFA.accept;
    auto f = mIds.find(s);
    if (f != mIds.end()) return f->second;
    const unsigned id = mNFA.addState();
    mIds.emplace(s, id);
    mWork.push_back(s);
    return id;
}

bool ICUAutomatonBuilder::advance(const std::vector<Condition> & pending, codepoint_t cp, std::vector<Condition> & next) const {
    next.clear();
    for (const Condition & cond : pending) {
        if (cond.chars && mElements[cond.element].charSet.contains(cp)) return false;
        const TrieNode & n = mTries[cond.element].nodes[cond.node];
        auto f = n.children.find(cp);
        if (f == n.children.end()) continue;    // satisfied
        const TrieNode & child = mTries[cond.element].nodes[f->second];
        if (child.terminal) return false;
        next.push_back(Condition{cond.element, f->second, false});
    }
    return true;
}

void ICUAutomatonBuilder::refine(const std::vector<Condition> & pending, std::vector<UCD::UnicodeSet> & parts) const {
    std::vector<UCD::UnicodeSet> sets;
    for (const Condition & cond : pending) {
        if (cond.chars) sets.push_back(mElements[cond.element].charSet);
        for (const auto & child : mTries[cond.element].nodes[cond.node].children) sets.emplace_back(child.first);
    }
    for (const UCD::UnicodeSet & set : sets) {
        std::vector<UCD::UnicodeSet> refined;
        for (const UCD::UnicodeSet & part : parts) {
            UCD::UnicodeSet in = part & set;
            UCD::UnicodeSet notIn = part - set;
            if (!in.empty()) refined.push_back(in);
            if (!notIn.empty()) refined.push_back(notIn);
        }
        parts = std::move(refined);
    }
}

void ICUAutomatonBuilder::addCharEdges(unsigned from, const UCD::UnicodeSet & chars, const std::vector<Condition> & pending,
                                       const std::function<State(codepoint_t, std::vector<Condition>)> & target,
                                       const std::vector<UCD::UnicodeSet> & split) {
    std::vector<UCD::UnicodeSet> parts;
    if (!chars.empty()) parts.push_back(chars);
    refine(pending, parts);
    for (const UCD::UnicodeSet & set : split) {
        std::vector<UCD::UnicodeSet> refined;
        for (const UCD::UnicodeSet & part : parts) {
            UCD::UnicodeSet in = part & set;
            UCD::UnicodeSet notIn = part - set;
            if (!in.empty()) refined.push_back(in);
            if (!notIn.empty()) refined.push_back(notIn);
        }
        parts = std::move(refined);
    }
    for (const UCD::UnicodeSet & part : parts) {
        const codepoint_t cp = part.front().first;
        std::vector<Condition> next;
        if (!advance(pending, cp, next)) continue;
        const unsigned to = stateOf(target(cp, next));
        mNFA.addEdge(from, to, NFA::Kind::Chars, makeCC(part), part);
    }
}

NFA ICUAutomatonBuilder::build() {
    mNFA.accept = mNFA.addState();
    mNFA.start = stateOf(State{0, 0, -1, {}});
    while (!mWork.empty()) {
        const State s = mWork.back();
        mWork.pop_back();
        const unsigned from = mIds.at(s);
        if (s.node >= 0) {
            // Within a string of the set of element s.i.
            const TrieNode & n = mTries[s.i].nodes[s.node];
            for (const auto & child : n.children) {
                const codepoint_t cp = child.first;
                const int node = child.second;
                addCharEdges(from, UCD::UnicodeSet(cp), s.pending, [&](codepoint_t, std::vector<Condition> next) {
                    return State{s.i, s.c, node, next};
                });
                if (mTries[s.i].nodes[node].terminal) {
                    // The string is chosen: no longer string may follow.
                    addCharEdges(from, UCD::UnicodeSet(cp), s.pending, [&](codepoint_t, std::vector<Condition> next) {
                        choose(s.i, node, next);
                        return State{s.i, cap(s.i, s.c), -1, next};
                    });
                }
            }
            continue;
        }
        if (s.i == mElements.size()) {
            // Matched, with conditions pending on the following text.
            addCharEdges(from, UCD::UnicodeSet(0, UCD::UNICODE_MAX), s.pending, [&](codepoint_t, std::vector<Condition> next) {
                return State{s.i, 0, -1, next};
            });
            mNFA.addEdge(from, mNFA.accept, NFA::Kind::Boundary);
            continue;
        }
        // The elements that may match next: passing over an element requires
        // that none of its alternatives follows.
        std::vector<Condition> pending = s.pending;
        size_t j = s.i;
        int c = s.c;
        for (;;) {
            if (j == mElements.size()) {
                mNFA.addEdge(from, stateOf(State{j, 0, -1, pending}), NFA::Kind::Epsilon);
                break;
            }
            const Element & e = mElements[j];
            if (e.ub < 0 || c < e.ub) {
                const int next = cap(j, c);
                // A character of the set: no string starting with it may follow.
                std::vector<UCD::UnicodeSet> firsts;
                for (const auto & child : mTries[j].nodes[0].children) firsts.emplace_back(child.first);
                addCharEdges(from, e.charSet, pending, [&](codepoint_t cp, std::vector<Condition> after) {
                    auto f = mTries[j].nodes[0].children.find(cp);
                    if (f != mTries[j].nodes[0].children.end()) choose(j, f->second, after);
                    return State{j, next, -1, after};
                }, firsts);
                // The first character of a string of the set.
                for (const auto & child : mTries[j].nodes[0].children) {
                    const codepoint_t cp = child.first;
                    const int node = child.second;
                    addCharEdges(from, UCD::UnicodeSet(cp), pending, [&](codepoint_t, std::vector<Condition> after) {
                        return State{j, c, node, after};
                    });
                    if (mTries[j].nodes[node].terminal) {
                        addCharEdges(from, UCD::UnicodeSet(cp), pending, [&](codepoint_t, std::vector<Condition> after) {
                            choose(j, node, after);
                            return State{j, next, -1, after};
                        });
                    }
                }
                // The text boundary (the conditions are satisfied there).
                // The text boundary (the conditions are satisfied there): a
                // zero-width match, counted, which ends the element.
                if (e.boundary && c + 1 >= e.lb) {
                    mNFA.addEdge(from, stateOf(State{j + 1, 0, -1, {}}), NFA::Kind::Boundary);
                }
            }
            if (c < e.lb) break;
            pending.push_back(Condition{j, 0, true});
            j++;
            c = 0;
        }
    }
    return mNFA;
}

// Merge the states of an automaton with the same behaviour: the coarsest
// partition in which the states of a block have transitions of the same
// kinds on the same characters into the same blocks.  (The conditions of
// ICUAutomatonBuilder distinguish states that may nevertheless behave alike,
// which would otherwise form cycles of several steps.)
// Whether the strings of the sets of the elements may be taken as single
// steps (for repeated sets with strings): each string begins with a
// character of its set, and none is a prefix of another.  Then, as ICU
// takes the longest string of a set, a text beginning with a string of the
// set is matched by that string, and otherwise by a character.
bool stringsAsSteps(const std::vector<Element> & elements) {
    bool repeated = false;
    for (const Element & e : elements) {
        if (e.strings.empty()) continue;
        repeated |= e.lb != e.ub;
        for (const auto & x : e.strings) {
            if (x.size() < 2 || !e.charSet.contains(x[0])) return false;
            for (const auto & y : e.strings) {
                if (&x != &y && x.size() <= y.size() && std::equal(x.begin(), x.end(), y.begin())) return false;
            }
        }
    }
    return repeated;
}

// An automaton under ICU's possessive matching, with the strings of sets as
// String edges (see stringsAsSteps).  The states are (element, repetitions);
// an element takes the characters (and strings) that the preceding elements
// that may be passed over cannot take, and the pattern is matched once its
// remaining elements may be empty.
NFA buildStringStepNFA(const std::vector<Element> & elements) {
    NFA n;
    n.accept = n.addState();
    std::map<std::pair<size_t, int>, unsigned> ids;
    std::vector<std::pair<size_t, int>> work;
    auto cap = [&](size_t j, int c) {
        const Element & e = elements[j];
        return e.ub < 0 ? std::min(c + 1, e.lb) : c + 1;
    };
    auto stateOf = [&](size_t i, int c) -> unsigned {
        while (i < elements.size() && elements[i].ub >= 0 && c >= elements[i].ub) {
            i++;
            c = 0;
        }
        bool complete = i == elements.size() || c >= elements[i].lb;
        for (size_t j = i + 1; j < elements.size() && complete; j++) complete = elements[j].lb == 0;
        if (complete) return n.accept;
        const auto key = std::make_pair(i, c);
        auto f = ids.find(key);
        if (f != ids.end()) return f->second;
        const unsigned id = n.addState();
        ids.emplace(key, id);
        work.push_back(key);
        return id;
    };
    n.start = stateOf(0, 0);
    while (!work.empty()) {
        const auto key = work.back();
        work.pop_back();
        const unsigned from = ids.at(key);
        UCD::UnicodeSet taken;
        bool boundaryTaken = false;
        size_t j = key.first;
        int c = key.second;
        while (j < elements.size()) {
            const Element & e = elements[j];
            if (e.ub < 0 || c < e.ub) {
                const unsigned to = stateOf(j, cap(j, c));
                const UCD::UnicodeSet takeable = e.charSet - taken;
                if (!takeable.empty()) {
                    RE * set = !(takeable == e.charSet) ? makeCC(takeable) : hasBoundary(e.set) ? e.chars : e.set;
                    n.addEdge(from, to, NFA::Kind::Chars, set, takeable);
                }
                for (const auto & str : e.strings) {
                    if (takeable.contains(str[0])) n.addStringEdge(from, to, e.set, str);
                }
                if (e.boundary && !boundaryTaken && c + 1 >= e.lb) {
                    n.addEdge(from, stateOf(j + 1, 0), NFA::Kind::Boundary);
                }
                taken = taken + e.charSet;
                boundaryTaken |= e.boundary;
            }
            if (c < e.lb) break;
            j++;
            c = 0;
        }
    }
    return n;
}

NFA reduce(const NFA & n) {
    const size_t N = n.out.size();
    struct Transition {
        NFA::Kind kind;
        unsigned to;
        UCD::UnicodeSet chars;
    };
    // The transitions of a state into the blocks, merged by kind and block.
    auto transitions = [&](unsigned s, const std::vector<unsigned> & block) {
        std::vector<Transition> t;
        for (const NFA::Edge & e : n.out[s]) {
            const unsigned to = block[e.to];
            if (e.kind == NFA::Kind::Epsilon && to == block[s]) continue;
            auto f = std::find_if(t.begin(), t.end(), [&](const Transition & x) {return x.kind == e.kind && x.to == to;});
            if (f == t.end()) {
                t.push_back(Transition{e.kind, to, e.chars});
            } else {
                f->chars = f->chars + e.chars;
            }
        }
        std::sort(t.begin(), t.end(), [](const Transition & a, const Transition & b) {
            return std::make_pair(a.kind, a.to) < std::make_pair(b.kind, b.to);
        });
        return t;
    };
    auto less = [](const std::pair<unsigned, std::vector<Transition>> & a, const std::pair<unsigned, std::vector<Transition>> & b) {
        if (a.first != b.first) return a.first < b.first;
        if (a.second.size() != b.second.size()) return a.second.size() < b.second.size();
        for (size_t i = 0; i < a.second.size(); i++) {
            const Transition & x = a.second[i];
            const Transition & y = b.second[i];
            if (x.kind != y.kind) return x.kind < y.kind;
            if (x.to != y.to) return x.to < y.to;
            const int c = x.chars.compare(y.chars);
            if (c != 0) return c < 0;
        }
        return false;
    };
    std::vector<unsigned> block(N);
    for (unsigned s = 0; s < N; s++) block[s] = (s == n.accept) ? 1 : 0;
    size_t blocks = 0;
    for (;;) {
        std::map<std::pair<unsigned, std::vector<Transition>>, unsigned, decltype(less)> ids(less);
        std::vector<unsigned> next(N);
        for (unsigned s = 0; s < N; s++) {
            auto key = std::make_pair(block[s], transitions(s, block));
            auto f = ids.find(key);
            if (f == ids.end()) f = ids.emplace(std::move(key), static_cast<unsigned>(ids.size())).first;
            next[s] = f->second;
        }
        block = std::move(next);
        if (ids.size() == blocks) break;
        blocks = ids.size();
    }
    NFA r;
    for (size_t b = 0; b < blocks; b++) r.addState();
    std::vector<bool> done(blocks, false);
    for (unsigned s = 0; s < N; s++) {
        if (done[block[s]]) continue;
        done[block[s]] = true;
        for (const Transition & t : transitions(s, block)) {
            RE * set = nullptr;
            if (t.kind == NFA::Kind::Chars) {
                for (const NFA::Edge & e : n.out[s]) {
                    if (e.kind == t.kind && block[e.to] == t.to && e.chars == t.chars) set = e.set;
                }
                if (set == nullptr) set = makeCC(t.chars);
            }
            r.addEdge(block[s], t.to, t.kind, set, t.chars);
        }
    }
    r.start = block[n.start];
    r.accept = block[n.accept];
    return r;
}

// Is there a text beginning with a match of each automaton?
bool compatible(const NFA & a, const NFA & b) {
    using S = std::tuple<unsigned, unsigned, bool>;     // (a state, b state, at the text boundary)
    std::set<S> visited;
    std::vector<S> pending{S{a.start, b.start, false}};
    auto visit = [&](S s) {
        if (visited.insert(s).second) pending.push_back(s);
    };
    while (!pending.empty()) {
        const S s = pending.back();
        pending.pop_back();
        const unsigned qa = std::get<0>(s);
        const unsigned qb = std::get<1>(s);
        const bool ended = std::get<2>(s);
        const bool acceptA = qa == a.accept;
        const bool acceptB = qb == b.accept;
        if (acceptA && acceptB) return true;
        for (const NFA::Edge & e : a.out[qa]) {
            if (e.kind == NFA::Kind::Epsilon) visit(S{e.to, qb, ended});
            else if (e.kind == NFA::Kind::Boundary) visit(S{e.to, qb, true});
        }
        for (const NFA::Edge & e : b.out[qb]) {
            if (e.kind == NFA::Kind::Epsilon) visit(S{qa, e.to, ended});
            else if (e.kind == NFA::Kind::Boundary) visit(S{qa, e.to, true});
        }
        if (ended) continue;
        if (acceptA) {
            for (const NFA::Edge & e : b.out[qb]) {
                if (e.kind == NFA::Kind::Chars && !e.chars.empty()) visit(S{qa, e.to, false});
            }
        } else if (acceptB) {
            for (const NFA::Edge & e : a.out[qa]) {
                if (e.kind == NFA::Kind::Chars && !e.chars.empty()) visit(S{e.to, qb, false});
            }
        } else {
            for (const NFA::Edge & ea : a.out[qa]) {
                if (ea.kind != NFA::Kind::Chars) continue;
                for (const NFA::Edge & eb : b.out[qb]) {
                    if (eb.kind == NFA::Kind::Chars && ea.chars.intersects(eb.chars)) visit(S{ea.to, eb.to, false});
                }
            }
        }
    }
    return false;
}

// The automaton matching only the empty string.
NFA emptyNFA() {
    NFA n;
    n.start = n.accept = n.addState();
    return n;
}

class Disambiguator {
public:
    Disambiguator(DisambiguationStats & stats) : mStats(stats) {}

    // Whether rule L is replaced (by the pieces, possibly none if L is
    // masked by its earlier rules), given its earlier overlapping rules.
    bool disambiguate(ConversionRule * L, const std::vector<ConversionRule *> & earlier, std::vector<Rule *> & pieces);

    // Whether an earlier rule e may match where r matches under ICU's
    // matching, given that they overlap as regular expressions.
    bool mayOverlapUnderICU(const ConversionRule * e, const ConversionRule * r) {
        Earlier p;
        std::string reason;
        if (!parseEarlier(e, p, reason, true)) return true;
        return mayOverlapPossessive(p, r);
    }

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
        bool possessive = false;    // an automaton for possessive matching was needed
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
        // A repeated set that also matches the text boundary (as negated
        // sets do); valid only if a following item requires a character.
        bool setBoundary = false;
        // The strings of a repeated set, each beginning with one of its
        // characters and none a prefix of another (reversed preceding the
        // position).
        std::vector<std::vector<codepoint_t>> strings;
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
    bool parseEarlier(const ConversionRule * e, Earlier & p, std::string & reason, bool possessive = false);
    // The automaton for elements: for possessive matching (if it differs from
    // the regular expression interpretation, or if possessive is set).
    bool automaton(const std::vector<Element> & elements, bool possessive, NFA & n, bool & isPossessive, std::string & reason);
    // May the rules match at the same position (with possessive matching)?
    bool mayOverlapPossessive(const Earlier & e, const ConversionRule * r);
    std::string itemReason(RE * e);
    void unresolved(const std::string & reason, size_t n = 1) {
        mStats.pairsUnresolved += n;
        mStats.unresolvedReasons[reason] += n;
    }
    bool possessiveIsExact(const std::vector<Element> & elements, std::string & reason);
    RE * unionOf(const std::vector<RE *> & sets) {
        // (Without repetitions of a set or a variable.)
        std::vector<RE *> distinct;
        for (RE * set : sets) {
            const bool repeated = std::any_of(distinct.begin(), distinct.end(), [set](RE * d) {
                if (d == set) return true;
                const Name * a = dyn_cast<Name>(d);
                const Name * b = dyn_cast<Name>(set);
                return a && b && a->getFullName() == b->getFullName();
            });
            if (!repeated) distinct.push_back(set);
        }
        if (distinct.empty()) return makeCC();
        return distinct.size() == 1 ? distinct[0] : makeAlt(distinct.begin(), distinct.end());
    }
    // A negated set, which also matches the text boundary (Start or End for a
    // before or after context) unless boundary is nullptr.
    RE * negated(const std::vector<RE *> & sets, RE * boundary) {
        // (Of the characters of the sets: their strings begin with them.)
        std::vector<RE *> chars;
        for (RE * set : sets) {
            std::vector<std::vector<codepoint_t>> strings;
            collectSetStrings(set, strings);
            chars.push_back(strings.empty() ? set : charPart(set, mAnalysis.setOf(set, false)));
        }
        RE * complement = makeDiff(makeAny(), unionOf(chars));
        return boundary ? makeAlt({complement, boundary}) : complement;
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
    // The states of the rules and of L after a string (as a whole where a
    // rule or L has it as a string of a set, otherwise character by
    // character); false if L cannot match it.
    bool stringStep(const std::vector<Earlier> & rules, bool after, const std::set<State> & active, LState l,
                    const std::vector<codepoint_t> & str, std::set<State> & next, LState & lNext);
    // The (preceding, following) paths on which all the rules fail.
    bool exploreSides(const std::vector<Earlier> & rules, std::vector<std::pair<Found, Found>> & sides);

    DisambiguationStats & mStats;
    CharSetAnalysis mAnalysis;
    RuleOverlapAnalysis mOverlaps;
    std::vector<LItem> mAfter;      // the items of L following its position: its key, then its after context
    size_t mKeyLength = 0;          // the number of key items
    std::vector<LItem> mBefore;     // the items of L's before context, outward
    std::string mFailure;           // why the exploration failed
    std::set<const Capture *> mReferenced;  // the segments of L referenced in its result
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
            // Segments within repetitions that are not referenced are ignored.
            RE * set = rep->getRE();
            while (isa<Capture>(set) && mReferenced.count(cast<Capture>(set)) == 0) {
                set = cast<Capture>(set)->getCapturedRE();
            }
            std::vector<std::vector<codepoint_t>> strings;
            collectSetStrings(set, strings);
            const UCD::UnicodeSet chars = mAnalysis.setOf(set, false);
            bool stringsOK = true;
            for (const auto & x : strings) {
                stringsOK &= x.size() > 1 && chars.contains(x[0]);
                for (const auto & y : strings) {
                    if (&x != &y && x.size() <= y.size() && std::equal(x.begin(), x.end(), y.begin())) stringsOK = false;
                }
            }
            if (!CharSetAnalysis::isSet(set) || !stringsOK || chars.empty()) {
                reason = itemReason(e);
                return false;
            }
            LItem item{set, chars, rep->getLB(), rep->getUB() == Rep::UNBOUNDED_REP ? -1 : rep->getUB(), false};
            item.setBoundary = hasBoundary(set);
            item.strings = strings;
            items.push_back(item);
            continue;
        }
        if (isBoundaryItem(e)) {
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

//  The segments referenced in a result.
static void collectReferences(const RE * re, std::set<const Capture *> & referenced) {
    if (re == nullptr) return;
    if (const Reference * ref = dyn_cast<Reference>(re)) {
        referenced.insert(ref->getCapture());
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) collectReferences(n->getDefinition(), referenced);
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collectReferences(e, referenced);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) collectReferences(e, referenced);
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        collectReferences(rep->getRE(), referenced);
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        collectReferences(c->getCapturedRE(), referenced);
    }
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
            if (isBoundary(a) || hasBoundary(a)) return true;
        }
    }
    return false;
}

// The characters of a set without its strings and text boundary.
static RE * charPart(RE * re, const UCD::UnicodeSet & chars) {
    // (A variable that includes the boundary in some context is replaced by
    // its characters, as a use elsewhere may resolve the boundary differently.)
    if (!hasStringsOrBoundary(re) && !mayIncludeTextBoundary(re)) return re;
    if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> members;
        for (RE * a : *alt) {
            if (!isa<Seq>(a) && !isBoundary(a) && !hasStringsOrBoundary(a)) {
                members.push_back(a);
            } else if (!isa<Seq>(a) && !isBoundary(a)) {
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
            // (Segments do not affect matching.)
            RE * body = rep->getRE();
            while (Capture * c = dyn_cast<Capture>(body)) body = c->getCapturedRE();
            bool ok = setElement(body, reversed, e);
            if (!ok && isSequenceVariable(body)) {
                // A repeated variable must expand to a single set.
                std::vector<Element> expanded;
                std::string why;
                ok = parseElements(cast<Name>(body)->getDefinition(), expanded, reversed, why)
                    && expanded.size() == 1 && expanded[0].lb == 1 && expanded[0].ub == 1;
                if (ok) e = expanded[0];
            }
            if (!ok || isa<Start>(e.set) || isa<End>(e.set)) {
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

bool Disambiguator::automaton(const std::vector<Element> & elements, bool possessive, NFA & n, bool & isPossessive,
                              std::string & reason) {
    std::string why;
    if (!possessive && possessiveIsExact(elements, why)) {
        n = buildNFA(elements);
        return true;
    }
    if (!possessive && stringsAsSteps(elements)) {
        // For the exploration (verification uses the exact automaton).
        n = buildStringStepNFA(elements);
        isPossessive = true;
        return true;
    }
    n = reduce(ICUAutomatonBuilder(elements).build());
    isPossessive = true;
    return true;
}

bool Disambiguator::mayOverlapPossessive(const Earlier & explored, const ConversionRule * r) {
    // (The exact automata of the earlier rule, not those of the exploration.)
    Earlier e;
    Earlier p;
    std::string reason;
    if (!parseEarlier(explored.rule, e, reason, true) || !parseEarlier(r, p, reason, true)) {
        return mOverlaps.mayOverlap(explored.rule, r);
    }
    const NFA empty = emptyNFA();
    return compatible(e.forward, p.forward) && compatible(e.after ? empty : e.backward, p.after ? empty : p.backward);
}

bool Disambiguator::parseEarlier(const ConversionRule * e, Earlier & p, std::string & reason, bool possessive) {
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
    if (!automaton(text, possessive, p.forward, p.possessive, reason)) return false;
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
        if (!automaton(before, possessive, p.backward, p.possessive, reason)) return false;
    }
    return true;
}

// The set x without the characters k, preserving the structure of unions.
RE * Disambiguator::subtract(RE * x, const UCD::UnicodeSet & k) {
    // (Of the characters of x only: not its strings or the text boundary.)
    const UCD::UnicodeSet xs = mAnalysis.setOf(x, false);
    x = charPart(x, xs);
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

bool Disambiguator::stringStep(const std::vector<Earlier> & rules, bool after, const std::set<State> & active, LState l,
                               const std::vector<codepoint_t> & str, std::set<State> & next, LState & lNext) {
    next.clear();
    for (const State & s : active) {
        const NFA & n = automaton(rules[s.first], after);
        bool whole = false;
        for (const NFA::Edge & e : n.out[s.second]) {
            if (e.kind == NFA::Kind::String && e.str == str) {
                next.insert(State{s.first, e.to});
                whole = true;
            }
        }
        if (whole) continue;
        std::set<State> cur{s};
        for (const codepoint_t cp : str) {
            std::set<State> following;
            for (const State & c : cur) {
                if (c.second == n.accept) {
                    following.insert(c);    // matched within the string
                    continue;
                }
                for (const NFA::Edge & e : n.out[c.second]) {
                    if (e.kind == NFA::Kind::Chars && e.chars.contains(cp)) following.insert(State{c.first, e.to});
                }
            }
            closure(rules, after, following);
            cur = std::move(following);
        }
        next.insert(cur.begin(), cur.end());
    }
    closure(rules, after, next);
    // L: the first of its possible items that may take the first character
    // takes the string as a whole if it is one of its strings.
    const std::vector<LItem> & lItems = after ? mAfter : mBefore;
    const LState done{0, 0, true};
    std::vector<std::pair<size_t, int>> candidates;
    bool canEnd = false;
    lCandidates(lItems, l, candidates, canEnd);
    for (const auto & cand : candidates) {
        const LItem & item = lItems[cand.first];
        if (item.boundary || !item.chars.contains(str[0])) continue;
        if (std::find(item.strings.begin(), item.strings.end(), str) != item.strings.end()) {
            lNext = lAdvance(lItems, cand.first, cand.second);
            return true;
        }
        break;
    }
    for (const codepoint_t cp : str) {
        if (l.done) break;
        lCandidates(lItems, l, candidates, canEnd);
        bool taken = false;
        for (const auto & cand : candidates) {
            if (!lItems[cand.first].boundary && lItems[cand.first].chars.contains(cp)) {
                l = lAdvance(lItems, cand.first, cand.second);
                taken = true;
                break;
            }
        }
        if (!taken) {
            if (!canEnd) return false;
            l = done;
        }
    }
    lNext = l;
    return true;
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
    std::vector<std::vector<codepoint_t>> strings;     // the strings of sets that may follow (in matching order)
    for (const State & s : active) {
        for (const NFA::Edge & e : automaton(rules[s.first], after).out[s.second]) {
            if (e.kind == NFA::Kind::String && !atBoundary) {
                if (std::find(strings.begin(), strings.end(), e.str) == strings.end()) strings.push_back(e.str);
            } else if (e.kind == NFA::Kind::Chars && !atBoundary) {
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
    // A text beginning with a string of a set is matched by the string where
    // a rule (or L) has the set: the string must lead to the same states as
    // its first character, and joins its class (which, as a set with
    // strings, then matches as ICU does, taking the string where present).
    for (const auto & cand : candidates) {
        for (const auto & str : lItems[cand.first].strings) {
            if (std::find(strings.begin(), strings.end(), str) == strings.end()) strings.push_back(str);
        }
    }
    auto stringRE = [after](const std::vector<codepoint_t> & str) {
        std::vector<RE *> cps;
        if (after) {
            for (const codepoint_t cp : str) cps.push_back(makeCC(cp));
        } else {
            for (auto i = str.rbegin(); i != str.rend(); ++i) cps.push_back(makeCC(*i));
        }
        return makeSeq(cps.begin(), cps.end());
    };
    // Whether a class already includes a string (as a set with strings).
    auto includes = [after](RE * cls, const std::vector<codepoint_t> & str) {
        std::vector<std::vector<codepoint_t>> within;
        collectSetStrings(cls, within);
        std::vector<codepoint_t> text = str;
        if (!after) std::reverse(text.begin(), text.end());
        return std::find(within.begin(), within.end(), text) != within.end();
    };
    for (const auto & str : strings) {
        size_t i = 0;
        while (i < parts.size() && !parts[i].contains(str[0])) i++;
        if (i == parts.size()) continue;    // L cannot match it, or all the rules fail (below)
        std::set<State> next;
        LState lNext;
        if (!stringStep(rules, after, active, l, str, next, lNext) || !(next == nexts[i]) || !(lNext == lNexts[i])) {
            mFailure = "E: set with strings whose strings and characters lead to different states";
            return false;
        }
        if (!includes(steps[i].re, str)) steps[i].re = makeAlt({steps[i].re, stringRE(str)});
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
        RE * cls = subtract(lItems[item].set, covered);
        for (const auto & str : lItems[item].strings) {
            if (part.contains(str[0]) && !includes(cls, str)) cls = makeAlt({cls, stringRE(str)});
        }
        std::vector<Step> p = path;
        p.push_back(Step{cls, item});
        out.push_back(Found{p, pending, lNext});
    }
    const bool boundaryFails = boundaryNext.empty() && boundaryItem < 0;
    UCD::UnicodeSet excludedChars = covered;
    for (const auto & cand : candidates) excludedChars = excludedChars + lItems[cand.first].chars;
    // (Unless the negated set is empty.)
    if (canEnd && (boundaryFails || !(excludedChars == UCD::UnicodeSet(0, UCD::UNICODE_MAX)))) {
        std::vector<RE *> excluded = edgeSets;
        excluded.insert(excluded.end(), candidateSets.begin(), candidateSets.end());
        std::vector<Step> p = path;
        RE * const boundary = after ? static_cast<RE *>(makeEnd()) : static_cast<RE *>(makeStart());
        p.push_back(Step{negated(excluded, boundaryFails ? boundary : nullptr), -1});
        out.push_back(Found{p, pending, done});
    }
    // The text boundary: matched by a boundary item of L, or beyond L.
    if (boundaryAllowed && (!boundaryNext.empty() || boundaryItem >= 0)) {
        const LState lNext = boundaryItem >= 0 ? lAdvance(lItems, boundaryItem, boundaryCount) : done;
        std::vector<Step> b = path;
        b.push_back(Step{makeBoundarySet(!after), boundaryItem});
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

bool Disambiguator::exploreSides(const std::vector<Earlier> & rules, std::vector<std::pair<Found, Found>> & sides) {
    sides.clear();
    mFailure.clear();
    // The paths following the position on which all the rules fail.
    Paths paths;
    std::set<State> initial;
    for (unsigned j = 0; j < rules.size(); j++) initial.insert(State{j, rules[j].forward.start});
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
    return ok;
}

bool Disambiguator::disambiguate(ConversionRule * L, const std::vector<ConversionRule *> & earlier, std::vector<Rule *> & pieces) {
    const RuleSide * src = L->getLeftSide();
    if (L->getDirection() != Direction::Forward) {
        unresolved("L: not a forward rule", earlier.size());
        return false;
    }
    mAfter.clear();
    mReferenced.clear();
    const RuleSide * res = L->getRightSide();
    for (const RE * part : {res->getBeforeContext(), res->getCompletedResult(), res->getResultToRevisit(), res->getAfterContext()}) {
        collectReferences(part, mReferenced);
    }
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
    // Likewise $ at the end of the text to replace (without an after
    // context) is a condition following it.
    // (A segment of the boundary alone becomes an empty segment.)
    RE * trailing = nullptr;
    const Capture * trailingSegment = nullptr;
    Capture * emptySegment = nullptr;
    if (!src->hasAfterContext()) {
        if (const Seq * seq = dyn_cast<Seq>(text)) {
            auto isBoundary = [](const RE * e) {return !isa<Start>(e) && isBoundaryItem(e);};
            if (seq->size() > 1 && isBoundary(seq->back())) {
                trailing = seq->back();
                text = makeSeq(seq->begin(), seq->end() - 1);
            } else if (seq->size() > 1 && isa<Capture>(seq->back()) && isBoundary(cast<Capture>(seq->back())->getCapturedRE())) {
                trailingSegment = cast<Capture>(seq->back());
                trailing = trailingSegment->getCapturedRE();
                emptySegment = makeCapture(trailingSegment->getName(), makeSeq());
                std::vector<RE *> elements(seq->begin(), seq->end() - 1);
                elements.push_back(emptySegment);
                text = makeSeq(elements.begin(), elements.end());
            }
        }
    }
    if (!parseL(text, mAfter, true, why)) {
        unresolved("L: text to replace has a " + why, earlier.size());
        return false;
    }
    mKeyLength = mAfter.size();
    if (trailing) mAfter.push_back(LItem{trailing, UCD::UnicodeSet(), 1, 1, true});
    if (src->hasAfterContext() && !parseL(src->getAfterContext(), mAfter, false, why)) {
        unresolved("L: after context has a " + why, earlier.size());
        return false;
    }
    if (src->hasBeforeContext()) {
        if (!parseL(src->getBeforeContext(), mBefore, true, why)) {
            unresolved("L: before context has a " + why, earlier.size());
            return false;
        }
        std::reverse(mBefore.begin(), mBefore.end());
        for (LItem & item : mBefore) {
            for (auto & str : item.strings) std::reverse(str.begin(), str.end());
        }
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
    // A repeated set with the text boundary must be followed by an item
    // requiring a character (the boundary then never leads to a match).
    auto deadBoundaries = [](const std::vector<LItem> & items) {
        for (size_t i = 0; i < items.size(); i++) {
            if (!items[i].setBoundary) continue;
            if (std::none_of(items.begin() + i + 1, items.end(), [](const LItem & x) {
                    return x.lb > 0 && !x.boundary && !x.chars.empty();})) return false;
        }
        return true;
    };
    if (!deadBoundaries(mAfter) || !deadBoundaries(mBefore)) {
        unresolved("L: repeated set with the text boundary as its outermost required item", earlier.size());
        return false;
    }
    // L is followed possessively in the exploration; the replacement rules
    // are then verified with possessive matching.
    auto hasStrings = [](const std::vector<LItem> & items) {
        return std::any_of(items.begin(), items.end(), [](const LItem & i) {return !i.strings.empty();});
    };
    const bool possessiveL = !possessiveIsExactL(mAfter) || !possessiveIsExactL(mBefore) || hasStrings(mAfter) || hasStrings(mBefore);
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
    std::vector<std::pair<Found, Found>> sides;     // (preceding, following) paths
    auto failure = [this]() {
        return mFailure.compare(0, 3, "E: ") == 0 ? mFailure : "E: " + mFailure;
    };
    if (!exploreSides(rules, sides)) {
        // The rules that cannot be explored alone remain unresolved; the
        // others may still be resolved together.
        if (rules.size() == 1) {
            unresolved(failure());
            return false;
        }
        const std::string joint = failure();
        std::vector<Earlier> alone;
        for (Earlier & e : rules) {
            std::vector<Earlier> one{e};
            std::vector<std::pair<Found, Found>> s;
            if (exploreSides(one, s)) {
                alone.push_back(std::move(e));
            } else {
                unresolved(failure());
            }
        }
        const size_t dropped = rules.size() - alone.size();
        rules = std::move(alone);
        if (dropped == 0 || rules.empty() || !exploreSides(rules, sides)) {
            if (dropped == 0) mFailure = joint;
            if (!rules.empty()) unresolved(failure(), rules.size());
            return false;
        }
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
        if (trailingSegment) captures.emplace(trailingSegment, captures.at(emptySegment));
        std::vector<RE *> afterItems;
        for (size_t j = mKeyLength; j < mAfter.size(); j++) afterItems.insert(afterItems.end(), following[j].begin(), following[j].end());
        afterItems.insert(afterItems.end(), beyond.begin(), beyond.end());
        RE * afterContext = afterItems.empty() ? nullptr : makeSeq(afterItems.begin(), afterItems.end());
        // (The before context is explored outward: its items, and the
        // classes of each, are reversed; the classes beyond L's items
        // precede it.)
        const std::vector<std::vector<RE *>> preceding = lReplacement(mBefore, side.first.path, side.first.l, beyond);
        std::vector<RE *> beforeItems(beyond.rbegin(), beyond.rend());
        if (src->hasBeforeContext()) {
            std::vector<RE *> items;
            for (auto r = preceding.rbegin(); r != preceding.rend(); ++r) items.push_back(makeSeq(r->rbegin(), r->rend()));
            size_t next = 0;
            beforeItems.push_back(rebuildKey(src->getBeforeContext(), items, next, captures));
        } else {
            for (auto r = preceding.rbegin(); r != preceding.rend(); ++r) beforeItems.insert(beforeItems.end(), r->rbegin(), r->rend());
        }
        RE * beforeContext = makeSeq(beforeItems.begin(), beforeItems.end());
        if (const Seq * seq = dyn_cast<Seq>(beforeContext)) {
            if (seq->empty()) beforeContext = nullptr;
        }
        RuleSide * rs = RuleSide::Create(beforeContext, key, false, nullptr, 0, afterContext);
        RuleSide * result = captures.empty() ? L->getRightSide() : remapReferences(L->getRightSide(), captures);
        pieces.push_back(makeConversionRule(rs, Direction::Forward, result));
    }
    // Paths differing only in the (capped) repetitions of L's items may give
    // the same rule.
    std::set<std::string> printed;
    pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [&](Rule * piece) {
        return !printed.insert(printRule(piece)).second;
    }), pieces.end());
    // Verify that no piece overlaps a resolved rule.
    for (Rule * piece : pieces) {
        for (const Earlier & e : rules) {
            const ConversionRule * r = cast<ConversionRule>(piece);
            if (e.possessive || possessiveL ? mayOverlapPossessive(e, r) : mOverlaps.mayOverlap(e.rule, r)) {
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
//  not backtrack into a set, an alternative that is a prefix (or, preceding
//  the position, a suffix) of a longer one applies only where the longer
//  one does not match: each alternative rule comes with blocking rules, which
//  match where a longer alternative of one of its sets matches (unless the
//  set is the outermost item of its side).
class AlternativeSplitter {
public:
    // The rules replacing r, or empty if r is not split, with the blocking
    // rules of each.
    std::vector<Rule *> split(const ConversionRule * r, std::vector<std::vector<ConversionRule *>> & blockers);
private:
    struct Leaf {
        RE * re;
        std::vector<RE *> alternatives;     // empty if not split
        // For each alternative, the longer strings of the set that it may
        // begin (or, preceding the position, end).
        std::vector<std::vector<std::vector<codepoint_t>>> longer;
    };
    bool splittable(RE * re) {
        return CharSetAnalysis::isSet(re) && hasStringsOrBoundary(re) && !isBoundary(re);
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
    // The strings longer than t that t begins (ends, if reversed).
    auto longerThan = [&](const std::vector<codepoint_t> & t) {
        std::vector<std::vector<codepoint_t>> longer;
        if (outermost) return longer;
        for (const auto & s : strings) {
            if (s.size() <= t.size()) continue;
            if (reversed ? std::equal(t.rbegin(), t.rend(), s.rbegin()) : std::equal(t.begin(), t.end(), s.begin())) {
                longer.push_back(s);
            }
        }
        return longer;
    };
    for (const auto & s : strings) {
        std::vector<RE *> cps;
        for (const codepoint_t c : s) cps.push_back(makeCC(c));
        leaf.alternatives.push_back(cps.size() == 1 ? cps[0] : makeSeq(cps.begin(), cps.end()));
        leaf.longer.push_back(longerThan(s));
    }
    if (!chars.empty()) {
        leaf.alternatives.push_back(charPart(re, chars));
        std::vector<std::vector<codepoint_t>> longer;
        if (!outermost) {
            for (const auto & s : strings) {
                if (s.size() > 1 && chars.contains(reversed ? s.back() : s.front())) longer.push_back(s);
            }
        }
        leaf.longer.push_back(longer);
    }
    if (hasBoundary(re)) {
        leaf.alternatives.push_back(makeBoundarySet(reversed));
        leaf.longer.emplace_back();
    }
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
        if (isBoundary(captured)) {
            // A segment of the text boundary alone is empty (keeping the
            // numbering of the segments).
            Capture * rebuilt = makeCapture(c->getName(), makeSeq());
            captures.emplace(c, rebuilt);
            return makeSeq({rebuilt, captured});
        }
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

std::vector<Rule *> AlternativeSplitter::split(const ConversionRule * r, std::vector<std::vector<ConversionRule *>> & blockers) {
    blockers.clear();
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
        // The blocking rules: the rule up to a leaf (following the position,
        // or preceding it from within) with a longer string in its place.
        blockers.emplace_back();
        for (size_t i = 0; i < leaves.size(); i++) {
            if (leaves[i]->alternatives.empty()) continue;
            for (const std::vector<codepoint_t> & str : leaves[i]->longer[index[i]]) {
                std::vector<RE *> cps;
                for (const codepoint_t c : str) cps.push_back(makeCC(c));
                std::vector<RE *> cut = choice;
                cut[i] = makeSeq(cps.begin(), cps.end());
                if (i < following.size()) {
                    for (size_t j = i + 1; j < following.size(); j++) cut[j] = makeSeq();
                } else {
                    // Preceding the position: the key (matched after the
                    // before context) and the outer leaves are not needed.
                    for (size_t j = 0; j < following.size(); j++) cut[j] = makeSeq();
                    for (size_t j = following.size(); j < i; j++) cut[j] = makeSeq();
                }
                std::map<const Capture *, Capture *> c;
                size_t n = 0;
                const std::vector<RE *> f(cut.begin(), cut.begin() + following.size());
                const std::vector<RE *> b(cut.begin() + following.size(), cut.end());
                RE * t = rebuild(src->getText(), true, f, n, c);
                RE * a = rebuild(src->getAfterContext(), false, f, n, c);
                n = 0;
                RE * bc = rebuild(src->getBeforeContext(), false, b, n, c);
                auto nonEmpty = [](RE * x) -> RE * {
                    if (x == nullptr) return nullptr;
                    if (const Seq * seq = dyn_cast<Seq>(x)) return seq->empty() ? nullptr : x;
                    return x;
                };
                RuleSide * bs = RuleSide::Create(nonEmpty(bc), t, false, nullptr, 0, nonEmpty(a));
                blockers.back().push_back(makeConversionRule(bs, Direction::Forward, RuleSide::Create(nullptr, makeSeq(), false, nullptr, 0, nullptr)));
            }
        }
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

//  The characters of a pattern (the members of its sets and its literal
//  characters).  Returns false if the pattern may produce other characters
//  (references to captures, function calls).
static bool patternChars(const re::RE * re, UCD::UnicodeSet & chars, CharSetAnalysis & sets) {
    if (re == nullptr) return true;
    if (isa<re::CC>(re) || isa<re::PropertyExpression>(re) || isa<re::Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)) {
        chars = chars + sets.setOf(re, true);
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) if (!patternChars(e, chars, sets)) return false;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * a : *alt) if (!patternChars(a, chars, sets)) return false;
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        return patternChars(rep->getRE(), chars, sets);
    } else if (const re::Capture * c = dyn_cast<re::Capture>(re)) {
        return patternChars(c->getCapturedRE(), chars, sets);
    } else if (const re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return false;
        return patternChars(name->getDefinition(), chars, sets);
    } else if (isa<re::Reference>(re)) {
        return false;
    }
    return true;
}

//  A set of single characters (no strings; and no text boundary, unless allowed)?
static bool isCharacterSet(const re::RE * re, CharSetAnalysis & sets, bool allowBoundary = false) {
    return re && CharSetAnalysis::isSet(re) && (allowBoundary || !mayIncludeTextBoundary(re))
        && sets.setOf(re, false) == sets.setOf(re, true);
}

static bool isEmptyResult(const RuleSide * result) {
    const re::Seq * seq = dyn_cast_or_null<re::Seq>(result->getText());
    return seq && seq->empty() && (!result->hasCursor() || result->getCursorOffset() == 0);
}

//  The deletion closure of the before contexts of the rules (see transform_rules.h).
static std::vector<Rule *> closeDeletionContexts(const std::vector<Rule *> & rules, DisambiguationStats & stats) {
    std::vector<Rule *> result = rules;
    CharSetAnalysis sets;
    for (size_t first = 0; first < result.size(); ) {
        size_t last = first;     // the group of conversion rules [first, last)
        while (last < result.size() && !isa<TransformRule>(result[last]) && !isa<FilterRule>(result[last])) last++;
        for (size_t i = first; i < last; i++) {
            ConversionRule * const L = dyn_cast<ConversionRule>(result[i]);
            if (L == nullptr || L->getDirection() != Direction::Forward) continue;
            const RuleSide * const source = L->getSourceSide(Direction::Forward);
            if (!isEmptyResult(L->getResultSide(Direction::Forward)) || source->hasAfterContext()
                    || !isCharacterSet(source->getText(), sets)) continue;
            const re::Seq * const before = dyn_cast_or_null<re::Seq>(source->getBeforeContext());
            if (before == nullptr || before->size() < 2) continue;
            const re::Rep * const star = dyn_cast<re::Rep>(before->back());
            const re::RE * const anchor = (*before)[before->size() - 2];
            //  The sets of A and S may include the text boundary (e.g., as negated sets):
            //  S, preceded by A, never matches it; A may match it only at the start of
            //  the text, where the closure holds as well.
            if (star == nullptr || star->getUB() != re::Rep::UNBOUNDED_REP
                    || !isCharacterSet(star->getRE(), sets, true) || !isCharacterSet(anchor, sets, true)) continue;
            const UCD::UnicodeSet D = sets.setOf(source->getText(), false);
            const UCD::UnicodeSet S = sets.setOf(star->getRE(), false);
            //  A must be disjoint from D: possessive matching of [S D]* (backward from the
            //  position) then stops where that of S* does, or, at a D character that was
            //  not deleted, fails as S* followed by A does.
            if ((D - S).empty() || !(sets.setOf(anchor, false) & D).empty()) continue;
            //  No other rule of the group converts or may produce D characters.
            bool independent = true;
            for (size_t j = first; j < last && independent; j++) {
                const ConversionRule * const R = dyn_cast<ConversionRule>(result[j]);
                if (R == nullptr || R == L) continue;
                UCD::UnicodeSet text, produced;
                independent = R->getDirection() == Direction::Forward
                    && patternChars(R->getSourceSide(Direction::Forward)->getText(), text, sets)
                    && patternChars(R->getResultSide(Direction::Forward)->getText(), produced, sets)
                    && (text & D).empty() && (produced & D).empty();
            }
            if (!independent) continue;
            std::vector<re::RE *> items(before->begin(), before->end());
            items.back() = re::makeRep(re::makeAlt({star->getRE(), source->getText()}), star->getLB(), star->getUB());
            RuleSide * const closed = RuleSide::Create(re::makeSeq(items.begin(), items.end()), source->getCompletedResult(),
                                                       source->hasCursor(), source->getResultToRevisit(),
                                                       source->getCursorOffset(), nullptr);
            result[i] = ConversionRule::Create(closed, Direction::Forward, L->getResultSide(Direction::Forward));
            stats.deletionClosures++;
        }
        first = last + 1;
    }
    return result;
}

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
    std::vector<size_t> origin;     // the index of the rule each result rule replaces
    for (size_t i = 0; i < rules.size(); i++) {
        auto f = earlierOf.find(i);
        if (f == earlierOf.end()) {
            result.push_back(rules[i]);
            origin.push_back(i);
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
            std::vector<std::vector<ConversionRule *>> blockers;
            const std::vector<Rule *> alternatives = splitter.split(L, blockers);
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
                    // (The blocking rules are not counted as pairs resolved.)
                    size_t blocking = 0;
                    for (ConversionRule * B : blockers[k]) {
                        if (analysis.mayOverlap(B, A)) {
                            earlier.push_back(B);
                            blocking++;
                        }
                    }
                    if (earlier.empty()) {
                        altPieces.push_back(A);
                        continue;
                    }
                    const size_t unresolved = s.pairsUnresolved;
                    std::vector<Rule *> p;
                    ok = d.disambiguate(A, earlier, p) && s.pairsUnresolved == unresolved;
                    if (ok) s.pairsResolved -= blocking;
                    altPieces.insert(altPieces.end(), p.begin(), p.end());
                }
                if (ok) {
                    pieces = altPieces;
                    replaced = true;
                    s.rulesReplaced = initial.rulesReplaced + 1;
                    s.rulesAdded = initial.rulesAdded + pieces.size();
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
            origin.insert(origin.end(), pieces.size(), i);
        } else {
            result.push_back(L);
            origin.push_back(i);
        }
    }
    // The remaining overlaps, under ICU's matching (pieces may be disjoint
    // from an earlier rule only under its longest-match or possessive
    // matching), counted as pairs of the original rules.
    std::set<std::pair<size_t, size_t>> remaining;
    for (const RuleOverlap & o : findRuleOverlaps(result)) {
        const std::pair<size_t, size_t> pair(origin[o.earlier], origin[o.later]);
        if (remaining.count(pair) == 0 && d.mayOverlapUnderICU(cast<ConversionRule>(result[o.earlier]), cast<ConversionRule>(result[o.later]))) {
            remaining.insert(pair);
        }
    }
    s.overlapsAfter = remaining.size();
    result = closeDeletionContexts(result, s);
    return result;
}

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Overlap analysis of conversion rules.
//
//  The patterns of rules are translated to nondeterministic finite automata
//  whose transitions are labelled by sets of codepoints (with Unicode
//  properties resolved), by the empty string, or by the text boundary, a
//  zero-width transition possible only at the end of the text (for the
//  patterns following a position) or at the start of the text (for the
//  patterns preceding a position, whose automata are reversed).  As in ICU,
//  the text boundary [$] matches beyond the start of the text in before
//  contexts and beyond the end of the text in after contexts, and ^ at the
//  start of the text to replace is a condition on the preceding text.
//
//  Two patterns are compatible if some text begins with a match of each;
//  this is decided by a search of the product of their automata, in which
//  a pattern that has been completely matched accepts any continuation.

#include <ldml/transform_rules.h>
#include "charset_analysis.h"
#include <re/adt/adt.h>
#include <ucd/core/unicode_set.h>
#include <map>
#include <set>
#include <tuple>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

struct NFA {
    enum class Kind {Epsilon, Chars, Boundary};
    struct Edge {
        unsigned to;
        Kind kind;
        UCD::UnicodeSet chars;
    };
    std::vector<std::vector<Edge>> out;
    unsigned start = 0;
    unsigned accept = 0;
    unsigned addState() {
        out.emplace_back();
        return static_cast<unsigned>(out.size() - 1);
    }
    void addEdge(unsigned from, unsigned to, Kind k, UCD::UnicodeSet chars = UCD::UnicodeSet()) {
        out[from].push_back(Edge{to, k, std::move(chars)});
    }
    // Append the states of another automaton, returning the offset of its states.
    unsigned append(const NFA & other) {
        const unsigned offset = static_cast<unsigned>(out.size());
        for (const auto & edges : other.out) {
            out.emplace_back();
            for (const Edge & e : edges) {
                out.back().push_back(Edge{e.to + offset, e.kind, e.chars});
            }
        }
        return offset;
    }
};

NFA concat(const NFA & a, const NFA & b) {
    NFA n;
    n.append(a);
    const unsigned offset = n.append(b);
    n.start = a.start;
    n.addEdge(a.accept, b.start + offset, NFA::Kind::Epsilon);
    n.accept = b.accept + offset;
    return n;
}

NFA reverse(const NFA & a) {
    NFA n;
    for (size_t i = 0; i < a.out.size(); i++) n.addState();
    for (unsigned q = 0; q < a.out.size(); q++) {
        for (const NFA::Edge & e : a.out[q]) {
            n.addEdge(e.to, q, e.kind, e.chars);
        }
    }
    n.start = a.accept;
    n.accept = a.start;
    return n;
}

// Is there a text that begins with a match of each automaton?
bool compatible(const NFA & a, const NFA & b) {
    using State = std::tuple<unsigned, unsigned, bool>;  // (a state, b state, at end of text)
    std::set<State> visited;
    std::vector<State> pending{State{a.start, b.start, false}};
    auto visit = [&](State s) {
        if (visited.insert(s).second) pending.push_back(s);
    };
    while (!pending.empty()) {
        const State s = pending.back();
        pending.pop_back();
        const unsigned qa = std::get<0>(s);
        const unsigned qb = std::get<1>(s);
        const bool ended = std::get<2>(s);
        const bool acceptA = qa == a.accept;
        const bool acceptB = qb == b.accept;
        if (acceptA && acceptB) return true;
        for (const NFA::Edge & e : a.out[qa]) {
            if (e.kind == NFA::Kind::Epsilon) visit(State{e.to, qb, ended});
            else if (e.kind == NFA::Kind::Boundary) visit(State{e.to, qb, true});
        }
        for (const NFA::Edge & e : b.out[qb]) {
            if (e.kind == NFA::Kind::Epsilon) visit(State{qa, e.to, ended});
            else if (e.kind == NFA::Kind::Boundary) visit(State{qa, e.to, true});
        }
        if (ended) continue;
        // A character matched by both; a completed match accepts any character.
        if (acceptA) {
            for (const NFA::Edge & e : b.out[qb]) {
                if (e.kind == NFA::Kind::Chars && !e.chars.empty()) visit(State{qa, e.to, false});
            }
        } else if (acceptB) {
            for (const NFA::Edge & e : a.out[qa]) {
                if (e.kind == NFA::Kind::Chars && !e.chars.empty()) visit(State{e.to, qb, false});
            }
        } else {
            for (const NFA::Edge & ea : a.out[qa]) {
                if (ea.kind != NFA::Kind::Chars) continue;
                for (const NFA::Edge & eb : b.out[qb]) {
                    if (eb.kind == NFA::Kind::Chars && ea.chars.intersects(eb.chars)) {
                        visit(State{ea.to, eb.to, false});
                    }
                }
            }
        }
    }
    return false;
}

// Which side of a position a pattern is matched on: the text before the
// position, where ^ and [$] match at the start of the text, or the text
// after it, where $ and [$] match at the end of the text.
enum class Side {Before, After};

class NFABuilder {
public:
    NFABuilder(CharSetAnalysis & sets) : mSets(sets) {}
    // An automaton for the pattern in the forward direction (nullptr: empty).
    NFA build(const RE * re, Side side) {
        NFA n;
        n.start = n.addState();
        n.accept = n.addState();
        if (re) {
            build(n, re, n.start, n.accept, side);
        } else {
            n.addEdge(n.start, n.accept, NFA::Kind::Epsilon);
        }
        return n;
    }
private:
    void build(NFA & n, const RE * re, unsigned from, unsigned to, Side side);
    void anyString(NFA & n, unsigned from, unsigned to) {
        n.addEdge(from, to, NFA::Kind::Epsilon);
        n.addEdge(to, to, NFA::Kind::Chars, AllCodepoints);
    }
    CharSetAnalysis & mSets;
};

// Large bounded repetitions are treated as unbounded.
constexpr int MaxExpandedRepetitions = 16;

void NFABuilder::build(NFA & n, const RE * re, unsigned from, unsigned to, Side side) {
    if (CharSetAnalysis::isSetExpression(re)) {
        n.addEdge(from, to, NFA::Kind::Chars, mSets.setOf(re));
    } else if (isa<Start>(re)) {
        // The start of the text, which cannot follow a position.
        if (side == Side::Before) n.addEdge(from, to, NFA::Kind::Boundary);
    } else if (isa<End>(re)) {
        // The end of the text, which cannot precede a position.
        if (side == Side::After) n.addEdge(from, to, NFA::Kind::Boundary);
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        unsigned cur = from;
        for (size_t i = 0; i < seq->size(); i++) {
            const unsigned next = (i + 1 == seq->size()) ? to : n.addState();
            build(n, (*seq)[i], cur, next, side);
            cur = next;
        }
        if (seq->empty()) n.addEdge(from, to, NFA::Kind::Epsilon);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) build(n, a, from, to, side);
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        const int lb = rep->getLB();
        int ub = rep->getUB();
        if (ub != Rep::UNBOUNDED_REP && ub - lb > MaxExpandedRepetitions) ub = Rep::UNBOUNDED_REP;
        unsigned cur = from;
        for (int i = 0; i < lb; i++) {
            const unsigned next = n.addState();
            build(n, rep->getRE(), cur, next, side);
            cur = next;
        }
        if (ub == Rep::UNBOUNDED_REP) {
            const unsigned loop = n.addState();
            n.addEdge(cur, loop, NFA::Kind::Epsilon);
            build(n, rep->getRE(), loop, loop, side);
            n.addEdge(loop, to, NFA::Kind::Epsilon);
        } else {
            for (int i = lb; i < ub; i++) {
                const unsigned next = n.addState();
                build(n, rep->getRE(), cur, next, side);
                n.addEdge(cur, next, NFA::Kind::Epsilon);
                cur = next;
            }
            n.addEdge(cur, to, NFA::Kind::Epsilon);
        }
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        build(n, c->getCapturedRE(), from, to, side);
    } else if (const Name * nm = dyn_cast<Name>(re)) {
        if (nm->getDefinition() && !isFunctionCall(nm)) {
            build(n, nm->getDefinition(), from, to, side);
        } else {
            anyString(n, from, to);
        }
    } else {
        // References and anything else: any string.
        anyString(n, from, to);
    }
}

} // end anonymous namespace

struct RuleOverlapAnalysis::Impl {
    CharSetAnalysis sets;
    NFABuilder builder{sets};
    struct RuleAutomata {
        NFA before;     // the reversed before context
        NFA after;      // the text to replace followed by the after context
        NFA key;        // the text to replace
        NFA afterContext;
        NFA beforeContext;  // the before context, not reversed
    };
    std::map<const ConversionRule *, RuleAutomata> automata;

    const RuleAutomata & get(const ConversionRule * r) {
        auto f = automata.find(r);
        if (f != automata.end()) return f->second;
        const RuleSide * src = r->getLeftSide();
        RuleAutomata a;
        RE * before = src->getBeforeContext();
        RE * text = src->getText();
        // ^ at the start of the text to replace (^ x → y) is a before context.
        if (before == nullptr) {
            if (isa<Start>(text)) {
                before = text;
                text = makeSeq();
            } else if (const Seq * seq = dyn_cast<Seq>(text)) {
                if (!seq->empty() && isa<Start>(seq->front())) {
                    before = seq->front();
                    text = makeSeq(seq->begin() + 1, seq->end());
                }
            }
        }
        a.beforeContext = builder.build(before, Side::Before);
        a.before = reverse(a.beforeContext);
        a.key = builder.build(text, Side::After);
        a.afterContext = builder.build(src->getAfterContext(), Side::After);
        a.after = concat(a.key, a.afterContext);
        return automata.emplace(r, std::move(a)).first->second;
    }
};

RuleOverlapAnalysis::RuleOverlapAnalysis() : mImpl(new Impl()) {}

RuleOverlapAnalysis::~RuleOverlapAnalysis() = default;

bool RuleOverlapAnalysis::mayOverlap(const ConversionRule * r1, const ConversionRule * r2) {
    const auto & a1 = mImpl->get(r1);
    const auto & a2 = mImpl->get(r2);
    return compatible(a1.after, a2.after) && compatible(a1.before, a2.before);
}

bool RuleOverlapAnalysis::mayMatchWithin(const ConversionRule * s, const ConversionRule * r) {
    const auto & as = mImpl->get(s);
    const auto & ar = mImpl->get(r);
    const NFA & key = ar.key;
    // A position within a match of the text to replace of r follows a
    // character edge e of its automaton and precedes at least one character.
    // The text before the position ends with a match of the before context
    // followed by a path of the key ending with e; the text after it begins
    // with a nonempty path of the key from the target of e, followed by the
    // after context.
    for (unsigned q = 0; q < key.out.size(); q++) {
        for (const NFA::Edge & e : key.out[q]) {
            if (e.kind != NFA::Kind::Chars || e.chars.empty()) continue;
            // The prefix: the key with its accepting state replaced by one
            // reached only by e.
            NFA prefix = key;
            const unsigned end = prefix.addState();
            prefix.addEdge(q, end, NFA::Kind::Chars, e.chars);
            prefix.accept = end;
            const NFA behind = reverse(concat(ar.beforeContext, prefix));
            if (!compatible(as.before, behind)) continue;
            // The suffix: paths of the key from e.to with at least one
            // character, using two copies of the states (before and after
            // a character has been matched).
            NFA suffix;
            const unsigned states = static_cast<unsigned>(key.out.size());
            for (unsigned i = 0; i < 2 * states; i++) suffix.addState();
            for (unsigned p = 0; p < states; p++) {
                for (const NFA::Edge & k : key.out[p]) {
                    if (k.kind == NFA::Kind::Chars) {
                        suffix.addEdge(p, k.to + states, k.kind, k.chars);
                        suffix.addEdge(p + states, k.to + states, k.kind, k.chars);
                    } else {
                        suffix.addEdge(p, k.to, k.kind, k.chars);
                        suffix.addEdge(p + states, k.to + states, k.kind, k.chars);
                    }
                }
            }
            suffix.start = e.to;
            suffix.accept = key.accept + states;
            const NFA ahead = concat(suffix, ar.afterContext);
            if (compatible(as.after, ahead)) return true;
        }
    }
    return false;
}

std::vector<RuleOverlap> findRuleOverlaps(const std::vector<Rule *> & rules) {
    std::vector<RuleOverlap> overlaps;
    RuleOverlapAnalysis analysis;
    size_t groupStart = 0;
    for (size_t i = 0; i < rules.size(); i++) {
        if (isa<TransformRule>(rules[i])) {
            groupStart = i + 1;
            continue;
        }
        const ConversionRule * later = dyn_cast<ConversionRule>(rules[i]);
        if (later == nullptr || !appliesForward(later->getDirection())) continue;
        for (size_t j = groupStart; j < i; j++) {
            const ConversionRule * earlier = dyn_cast<ConversionRule>(rules[j]);
            if (earlier == nullptr || !appliesForward(earlier->getDirection())) continue;
            if (analysis.mayOverlap(earlier, later)) overlaps.push_back(RuleOverlap{j, i});
        }
    }
    return overlaps;
}

}

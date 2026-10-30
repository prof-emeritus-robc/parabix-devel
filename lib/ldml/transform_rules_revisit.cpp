/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Composition of the rules whose results have text to revisit with the rules
//  applying to that text (see ComposeRevisits in transform_rules.h).

#include <ldml/transform_rules.h>
#include <map>
#include <set>
#include <deque>
#include <algorithm>
#include <optional>
#include <ldml/transform_plan.h>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include "charset_analysis.h"
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

constexpr unsigned MaxEnumeration = 64;       // characters of an enumerated capture
constexpr unsigned MaxComposedRules = 1024;   // rules replacing one rule
constexpr unsigned MaxApplications = 8;       // rule applications along a path
constexpr unsigned MaxExtension = 4;          // characters matched beyond the text

//  Composition is abandoned for the rule by throwing NotComposed.
struct NotComposed {
    std::string reason;
};

bool hasStrings(const RE * re) {
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        return seq->size() != 1 || hasStrings(seq->front());
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (hasStrings(a)) return true;
        }
        return false;
    } else if (const Name * name = dyn_cast<Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return true;
        return hasStrings(name->getDefinition());
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return hasStrings(diff->getLH()) || hasStrings(diff->getRH());
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        return hasStrings(x->getLH()) || hasStrings(x->getRH());
    }
    return isa<Capture>(re) || isa<Reference>(re) || isa<Rep>(re);
}

//  Does a set include the text boundary (through variables, unions,
//  differences and intersections)?
bool hasBoundary(const RE * re) {
    if (isBoundary(re)) return true;
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && hasBoundary(n->getDefinition());
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (hasBoundary(a)) return true;
        }
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return hasBoundary(diff->getLH()) && !hasBoundary(diff->getRH());
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        return hasBoundary(x->getLH()) && hasBoundary(x->getRH());
    }
    return false;
}

//  An item of a context: a set of characters, possibly including the text
//  boundary, or a (possessive) repetition of such a set, or an optional one.
struct Item {
    UCD::UnicodeSet chars;
    bool boundary = false;
    bool star = false;
    bool optional = false;
    bool operator==(const Item & other) const {
        return chars == other.chars && boundary == other.boundary && star == other.star && optional == other.optional;
    }
};

//  A context: absent or empty (no condition), a sequence of items, or
//  (if not simple) another pattern, which can only be kept unchanged (or
//  combined with another, as regular expressions; see conjoin).
struct Context {
    RE * re = nullptr;        // the original pattern, while unchanged
    bool simple = true;
    std::vector<Item> items;
    bool empty() const {return simple && items.empty();}
};

//  Does a context always match (it is empty, or has only repetitions)?
bool alwaysMatches(const Context & c) {
    if (!c.simple) return false;
    for (const Item & i : c.items) {
        if (!i.star && !i.optional) return false;
    }
    return true;
}

//  Does context a imply context b (both before contexts, aligned at the end,
//  or after contexts, aligned at the start)?
bool implies(const Context & a, const Context & b, bool front) {
    if (alwaysMatches(b)) return true;
    if (a.re && a.re == b.re) return true;
    if (!a.simple || !b.simple || b.items.size() > a.items.size()) return false;
    for (size_t k = 0; k < b.items.size(); k++) {
        const Item & x = front ? a.items[k] : a.items[a.items.size() - 1 - k];
        const Item & y = front ? b.items[k] : b.items[b.items.size() - 1 - k];
        if (x.star || y.star || x.optional || y.optional) return false;
        if (!x.chars.subset(y.chars) || (x.boundary && !y.boundary)) return false;
    }
    return true;
}

enum class Match {Ok, Fail};

//  Match a character of a set s against the first item of an after context
//  (front) or the last item of a before context, removing the item matched.
//  The set s is restricted to the characters for which the match succeeds;
//  Fail if there are none.
Match consume(Context & c, UCD::UnicodeSet & s, bool front) {
    if (c.empty()) return Match::Ok;
    if (!c.simple) throw NotComposed{"a context (" + printPattern(c.re) + ") is not a sequence of sets"};
    c.re = nullptr;
    while (!c.items.empty()) {
        Item & item = front ? c.items.front() : c.items.back();
        //  An optional item, matched possessively: taken if the character is
        //  in its set, else skipped.
        if (item.optional) {
            if (s.subset(item.chars)) {
                if (front) c.items.erase(c.items.begin()); else c.items.pop_back();
                return Match::Ok;
            }
            if (!s.intersects(item.chars)) {
                if (front) c.items.erase(c.items.begin()); else c.items.pop_back();
                continue;
            }
            throw NotComposed{"an optional item in a context matches some of the characters"};
        }
        if (item.star) {
            if (s.subset(item.chars)) return Match::Ok;
            if (!s.intersects(item.chars)) {
                if (front) c.items.erase(c.items.begin()); else c.items.pop_back();
                continue;
            }
            throw NotComposed{"a repetition in a context matches some of the characters"};
        }
        s = s & item.chars;
        if (front) c.items.erase(c.items.begin()); else c.items.pop_back();
        return s.empty() ? Match::Fail : Match::Ok;
    }
    return Match::Ok;
}

RE * contextRE(const Context & c, bool before);

//  Contexts with repetitions or optional items, item by item, where each is
//  matched possessively: a repetition or optional item disjoint from the item
//  of the other context matches nothing there, and one including it matches
//  it.  Ok or Fail, or false if they cannot be combined so.
bool conjoinPossessive(const std::vector<Item> & a, const std::vector<Item> & b, bool front, Match & m,
                       std::vector<Item> & out) {
    std::deque<Item> x, y, items;
    for (const Item & i : a) {
        if (front) x.push_back(i); else x.push_front(i);
    }
    for (const Item & i : b) {
        if (front) y.push_back(i); else y.push_front(i);
    }
    m = Match::Ok;
    while (!x.empty() && !y.empty()) {
        Item & p = x.front();
        Item & q = y.front();
        if (p.boundary || q.boundary) return false;
        const bool pv = p.star || p.optional, qv = q.star || q.optional;
        if (!pv && !qv) {
            Item i;
            i.chars = p.chars & q.chars;
            if (i.chars.empty()) m = Match::Fail;
            items.push_back(i);
            x.pop_front();
            y.pop_front();
        } else if (pv && qv) {
            if (!(p == q)) return false;
            items.push_back(p);
            x.pop_front();
            y.pop_front();
        } else {
            std::deque<Item> & vs = pv ? x : y;
            std::deque<Item> & fs = pv ? y : x;
            Item & v = vs.front();
            Item & f = fs.front();
            if (!f.chars.intersects(v.chars)) {
                vs.pop_front();
            } else if (f.chars.subset(v.chars)) {
                items.push_back(f);
                fs.pop_front();
                if (v.optional) vs.pop_front();
            } else {
                return false;
            }
        }
    }
    for (const Item & i : x) items.push_back(i);
    for (const Item & i : y) items.push_back(i);
    out.assign(items.begin(), items.end());
    if (!front) std::reverse(out.begin(), out.end());
    return true;
}

//  Both contexts: before contexts are aligned at the end, after contexts at
//  the start.  Fail if they cannot both match.  Contexts that are not
//  sequences of sets, or have repetitions or optional items, are combined as
//  regular expressions: b (an after context) as a lookahead at the start of
//  a, or (a before context) as a lookbehind at its end.
Match conjoin(Context & a, const Context & b, bool front) {
    if (b.empty()) return Match::Ok;
    if (a.empty()) {
        a = b;
        return Match::Ok;
    }
    bool repeated = !a.simple || !b.simple;
    for (const Item & i : a.items) repeated |= i.star || i.optional;
    for (const Item & i : b.items) repeated |= i.star || i.optional;
    if (repeated) {
        if ((a.re && a.re == b.re) || (a.simple && b.simple && a.items == b.items)) return Match::Ok;
        std::vector<Item> items;
        Match m;
        if (a.simple && b.simple && conjoinPossessive(a.items, b.items, front, m, items)) {
            if (m == Match::Fail) return Match::Fail;
            a.re = nullptr;
            a.items = std::move(items);
            return Match::Ok;
        }
        RE * const x = contextRE(a, !front);
        RE * const y = contextRE(b, !front);
        a.re = front ? makeSeq({makeLookAheadAssertion(y), x}) : makeSeq({x, makeLookBehindAssertion(y)});
        a.simple = false;
        a.items.clear();
        return Match::Ok;
    }
    const size_t n = std::max(a.items.size(), b.items.size());
    std::vector<Item> items(n);
    for (size_t k = 0; k < n; ++k) {
        // k counts from the start (after context) or the end (before context)
        auto at = [&](const std::vector<Item> & v) -> const Item * {
            if (k >= v.size()) return nullptr;
            return front ? &v[k] : &v[v.size() - 1 - k];
        };
        const Item * x = at(a.items);
        const Item * y = at(b.items);
        Item & item = items[front ? k : n - 1 - k];
        if (x && y) {
            item.chars = x->chars & y->chars;
            item.boundary = x->boundary && y->boundary;
        } else {
            item = x ? *x : *y;
        }
        if (item.chars.empty() && !item.boundary) return Match::Fail;
    }
    a.re = nullptr;
    a.items = std::move(items);
    return Match::Ok;
}

RE * contextRE(const Context & c, bool before) {
    if (!c.simple || c.re) return c.re;
    if (c.items.empty()) return nullptr;
    std::vector<RE *> seq;
    for (const Item & i : c.items) {
        RE * e = makeCC(i.chars);
        if (i.boundary) {
            RE * const boundary = before ? static_cast<RE *>(makeStart()) : static_cast<RE *>(makeEnd());
            e = Alt::Create({e, boundary});
        }
        if (i.star) e = makeRep(e, 0, Rep::UNBOUNDED_REP);
        if (i.optional) e = makeRep(e, 0, 1);
        seq.push_back(e);
    }
    return makeSeq(seq.begin(), seq.end());
}

RE * literal(const std::u32string & s) {
    std::vector<RE *> seq;
    for (char32_t c : s) seq.push_back(makeCC(static_cast<codepoint_t>(c)));
    return makeSeq(seq.begin(), seq.end());
}

//  The string of a result, with references to captures given by refs.  If
//  replaced is given, it is set to the length of the string following the
//  last reference (if any).
bool resultString(const RE * re, const std::map<std::string, std::u32string> & refs, std::u32string & out,
                  size_t * replaced = nullptr) {
    if (re == nullptr) return true;
    if (const CC * cc = dyn_cast<CC>(re)) {
        if (cc->count() != 1) return false;
        out.push_back(static_cast<char32_t>(cc->min_codepoint()));
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) {
            if (!resultString(e, refs, out, replaced)) return false;
        }
        return true;
    } else if (const Name * name = dyn_cast<Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return false;
        return resultString(name->getDefinition(), refs, out, replaced);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        return alt->size() == 1 && resultString(alt->front(), refs, out, replaced);
    } else if (const Reference * ref = dyn_cast<Reference>(re)) {
        auto f = refs.find(ref->getName());
        if (f == refs.end()) return false;
        out += f->second;
        if (replaced) *replaced = out.size();
        return true;
    }
    return false;
}

void collectReferences(const RE * re, std::set<std::string> & names) {
    if (re == nullptr) return;
    if (const Reference * ref = dyn_cast<Reference>(re)) {
        names.insert(ref->getName());
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collectReferences(e, names);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) collectReferences(e, names);
    } else if (const Name * name = dyn_cast<Name>(re)) {
        if (isFunctionCall(name)) collectReferences(name->getDefinition(), names);
    }
}

//  The result of a rule: the completed result and the text to revisit, with
//  the cursor placed as ICU places it.  ICU builds a result with references
//  to captures (or function calls) piece by piece, recording the cursor
//  position before adding the literal text accumulated since the last
//  reference: the cursor is placed after the last reference preceding it,
//  or at the start of the result.  (ICU also places the cursor at the start
//  of the result on the first application of a rule with a literal result,
//  which is not reproduced.)
bool ruleResult(const RuleSide * result, const std::map<std::string, std::u32string> & refs,
                std::u32string & completed, std::u32string & revisit) {
    size_t replaced = 0;
    if (!resultString(result->getCompletedResult(), refs, completed, &replaced)) return false;
    if (!resultString(result->getResultToRevisit(), refs, revisit)) return false;
    std::set<std::string> names;
    collectReferences(result->getCompletedResult(), names);
    collectReferences(result->getResultToRevisit(), names);
    if (!names.empty() && !revisit.empty()) {
        revisit = completed.substr(replaced) + revisit;
        completed.erase(replaced);
    }
    return true;
}

//  A rule of the group, as applied to the text to revisit.
struct GroupRule {
    ConversionRule * rule;
    bool revisits = false;      // its result has text to revisit (before any rewriting)
    //  The sets of the items of the text, or of its first items if the text
    //  is not a fixed-length sequence of characters (then !fixed).
    std::vector<UCD::UnicodeSet> text;
    bool fixed = true;
    //  If !fixed: the characters with which a match may begin (or, for an
    //  insertion rule, which may follow it), if known.
    std::optional<UCD::UnicodeSet> first;
    bool startAnchor = false;   // ^ at the start of the text (as [$] ending the before context)
    bool endAnchor = false;     // $ at the end of the text (as [$] starting the after context)
    std::map<std::string, std::pair<unsigned, unsigned>> captures;  // items [first, last)
    Context before;
    Context after;
};

//  The characters of a match: known characters (of the text to revisit), or
//  characters beyond the text of the composed rule (index into Path::beyond).
struct MatchedChar {
    char32_t c;
    int beyond;     // -1 for a known character
};

//  A path of rule applications from the revisiting rule.
struct Path {
    std::u32string out;                         // the result so far
    std::u32string pending;                     // the text to revisit
    std::vector<UCD::UnicodeSet> beyond;        // the characters matched beyond the text
    Context before;                             // on the text before the text of the rule
    std::vector<std::pair<unsigned, Context>> after;  // on the text after it, at an offset
    std::vector<const Rule *> applied;          // the rules applied
};

//  Record a rule applying to the text to revisit (see RevisitRecord).
void noteTrigger(RevisitRecord * record, const GroupRule & g, unsigned depth, bool conditional, bool extends, bool blocking) {
    if (record == nullptr) return;
    for (RevisitTrigger & t : record->triggered) {
        if (t.rule == g.rule) {
            t.depth = std::min(t.depth, depth);
            t.conditional |= conditional;
            t.extends |= extends;
            t.blocking |= blocking;
            return;
        }
    }
    RevisitTrigger t{g.rule, depth};
    t.conditional = conditional;
    t.extends = extends;
    t.revisits = g.revisits;
    t.blocking = blocking;
    record->triggered.push_back(t);
}

class Composer {
public:
    Composer(RevisitStats * stats) : mStats(stats) {}
    std::vector<Rule *> compose(const std::vector<Rule *> & rules);
private:
    Context context(RE * re);
    bool collectItems(RE * re, std::vector<Item> & items);
    GroupRule groupRule(ConversionRule * r);
    void textItems(RE * re, GroupRule & g);
    bool firstChars(RE * re, UCD::UnicodeSet & first, bool & nullable);
    std::vector<Rule *> composeRule(ConversionRule * r, const std::vector<GroupRule> & group);
    void explore(Path p, RE * text, std::vector<Rule *> & composed);
    void apply(const GroupRule & g, Path q, const std::vector<MatchedChar> & matched,
               std::vector<int> referenced, RE * text, std::vector<Rule *> & composed);
    void finish(Path p, RE * text, std::vector<Rule *> & composed);
    Rule * suffixRevisit(ConversionRule * r);
    bool chainRevisitSplit(const std::vector<Rule *> & group, const UCD::UnicodeSet * filter,
                           std::vector<bool> & isChain, ChainRevisit & chain, std::string & reason);
    bool chainUnseen(const ConversionRule * r, const ChainRevisit & chain, const UCD::UnicodeSet & first, std::string & reason);
    bool markRunSplit(const std::vector<Rule *> & group, const UCD::UnicodeSet * filter,
                      std::vector<bool> & isEdit, std::vector<bool> & isKept, std::string & reason);
    bool markRunUnseen(const ConversionRule * r, const MarkRunEdit & e, std::string & reason);
    bool keptCandidate(const ConversionRule * r);
    CharSetAnalysis mSets;
    RevisitStats * mStats;
    RevisitRecord * mRecord = nullptr;
    const std::vector<GroupRule> * mGroup = nullptr;
    std::set<std::string> mEmitted;
};

bool Composer::collectItems(RE * re, std::vector<Item> & items) {
    if (CharSetAnalysis::isSet(re)) {
        if (hasStrings(re)) return false;
        items.push_back(Item{mSets.setOf(re, false), hasBoundary(re), false});
        return true;
    } else if (isBoundary(re)) {
        items.push_back(Item{UCD::UnicodeSet(), true, false});
        return true;
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) {
            if (!collectItems(e, items)) return false;
        }
        return true;
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        return collectItems(alt->front(), items);
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return collectItems(c->getCapturedRE(), items);
    } else if (Name * name = dyn_cast<Name>(re)) {
        return !isFunctionCall(name) && name->getDefinition() && collectItems(name->getDefinition(), items);
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        std::vector<Item> body;
        if (!collectItems(rep->getRE(), body) || body.size() != 1) return false;
        const int lb = rep->getLB();
        const int ub = rep->getUB();
        if (lb == 0 && ub == 1) {
            items.push_back(body.front());
            items.back().optional = true;
            return true;
        }
        if (ub != Rep::UNBOUNDED_REP && ub != lb) return false;
        for (int k = 0; k < lb; ++k) items.push_back(body.front());
        if (ub == Rep::UNBOUNDED_REP) {
            items.push_back(body.front());
            items.back().star = true;
        }
        return true;
    }
    return false;
}

Context Composer::context(RE * re) {
    Context c;
    c.re = re;
    if (re && !collectItems(re, c.items)) {
        c.simple = false;
        c.items.clear();
    }
    return c;
}

void Composer::textItems(RE * re, GroupRule & g) {
    if (!g.fixed) return;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) textItems(e, g);
    } else if (g.endAnchor) {
        g.fixed = false;
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        const unsigned first = g.text.size();
        textItems(c->getCapturedRE(), g);
        g.captures[c->getName()] = std::make_pair(first, static_cast<unsigned>(g.text.size()));
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        textItems(alt->front(), g);
    } else if (Name * name = dyn_cast<Name>(re);
               name && !isFunctionCall(name) && name->getDefinition() && !CharSetAnalysis::isSet(name->getDefinition())) {
        textItems(name->getDefinition(), g);
    } else if (CharSetAnalysis::isSet(re) && !hasStrings(re) && !hasBoundary(re)) {
        g.text.push_back(mSets.setOf(re, false));
    } else if (isa<Start>(re) && g.text.empty() && !g.endAnchor) {
        g.startAnchor = true;
    } else if (isa<End>(re) && !g.text.empty() && !g.endAnchor) {
        g.endAnchor = true;
    } else {
        g.fixed = false;
    }
}

//  The characters with which a match of re may begin (strings within sets by
//  their characters), added to first, and whether it may be empty; false if
//  not known.
bool Composer::firstChars(RE * re, UCD::UnicodeSet & first, bool & nullable) {
    if (CharSetAnalysis::isSet(re)) {
        first = first + mSets.setOf(re, true);
        nullable = false;
        return true;
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        nullable = true;
        for (RE * e : *seq) {
            bool n = false;
            if (!firstChars(e, first, n)) return false;
            if (!n) {
                nullable = false;
                break;
            }
        }
        return true;
    } else if (Alt * alt = dyn_cast<Alt>(re)) {
        nullable = false;
        for (RE * e : *alt) {
            bool n = false;
            if (!firstChars(e, first, n)) return false;
            nullable |= n;
        }
        return true;
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        if (!firstChars(rep->getRE(), first, nullable)) return false;
        nullable |= rep->getLB() == 0;
        return true;
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return firstChars(c->getCapturedRE(), first, nullable);
    } else if (Name * name = dyn_cast<Name>(re)) {
        return !isFunctionCall(name) && name->getDefinition() && firstChars(name->getDefinition(), first, nullable);
    } else if (isBoundary(re) || isa<Start>(re) || isa<End>(re)) {
        nullable = true;
        return true;
    }
    return false;
}

GroupRule Composer::groupRule(ConversionRule * r) {
    GroupRule g;
    g.rule = r;
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    textItems(source->getText(), g);
    if (g.text.empty()) g.fixed = false;
    g.before = context(source->getBeforeContext());
    g.after = context(source->getAfterContext());
    if (!g.fixed && !g.startAnchor) {
        //  The first characters of the text, and of the after context if the
        //  text may be empty (as that of an insertion rule).
        UCD::UnicodeSet first;
        bool nullable = false;
        if (firstChars(source->getText(), first, nullable)) {
            if (!nullable) {
                g.first = first;
            } else if (source->hasAfterContext() && firstChars(source->getAfterContext(), first, nullable) && !nullable) {
                g.first = first;
            }
        }
    }
    if (g.startAnchor) {
        if (!g.before.simple) g.fixed = false;
        g.before.re = nullptr;
        g.before.items.insert(g.before.items.begin(), Item{UCD::UnicodeSet(), true, false});
    }
    if (g.endAnchor) {
        if (!g.after.simple) g.fixed = false;
        g.after.re = nullptr;
        g.after.items.push_back(Item{UCD::UnicodeSet(), true, false});
    }
    return g;
}

//  Continue a path at the start of the text to revisit: with the rules of the
//  group in order, or (if none applies) passing its first character through.
void Composer::explore(Path p, RE * text, std::vector<Rule *> & composed) {
    if (p.pending.empty()) {
        finish(std::move(p), text, composed);
        return;
    }
    if (composed.size() > MaxComposedRules) {
        throw NotComposed{"more than " + std::to_string(MaxComposedRules) + " composed rules"};
    }
    for (const GroupRule & g : *mGroup) {
        Path q = p;
        std::vector<MatchedChar> matched;
        bool possible = true;
        for (size_t i = 0; i < g.text.size(); ++i) {
            if (i < p.pending.size()) {
                if (!g.text[i].contains(p.pending[i])) {
                    possible = false;
                    break;
                }
                matched.push_back(MatchedChar{p.pending[i], -1});
            } else {
                matched.push_back(MatchedChar{0, static_cast<int>(q.beyond.size())});
                q.beyond.push_back(g.text[i]);
            }
        }
        if (!possible) continue;
        //  A rule whose text is not of fixed length, which cannot begin here.
        if (!g.fixed && g.first && !g.first->contains(p.pending[0])) continue;
        if (!g.fixed) {
            noteTrigger(mRecord, g, p.applied.size() + 1, true, q.beyond.size() > p.beyond.size(), true);
            throw NotComposed{"a rule whose text is not a fixed-length sequence of characters may apply "
                              "to the text to revisit: " + printRule(g.rule)};
        }
        if (q.beyond.size() > MaxExtension) {
            noteTrigger(mRecord, g, p.applied.size() + 1, true, true, true);
            std::string rules;
            for (const Rule * r : p.applied) rules += printRule(r) + " then ";
            throw NotComposed{"the text to revisit is extended by more than " + std::to_string(MaxExtension) + " characters: "
                              + rules + printRule(g.rule)};
        }
        //  The before context of the rule is matched against the result so far,
        //  then against the text before the text of the composed rule.
        Context before = g.before;
        for (size_t k = p.out.size(); k-- > 0 && !before.empty(); ) {
            UCD::UnicodeSet s(p.out[k]);
            if (consume(before, s, false) == Match::Fail) {
                possible = false;
                break;
            }
        }
        if (!possible) continue;
        const bool beforeImplied = implies(p.before, before, false);
        if (conjoin(q.before, before, false) == Match::Fail) continue;
        //  The after context is matched against the rest of the text to revisit,
        //  then against the text beyond.
        Context after = g.after;
        const size_t rest = std::min(g.text.size(), p.pending.size());
        for (size_t k = rest; k < p.pending.size() && !after.empty(); ++k) {
            UCD::UnicodeSet s(p.pending[k]);
            if (consume(after, s, true) == Match::Fail) {
                possible = false;
                break;
            }
        }
        if (!possible) continue;
        //  A rule matching within the text to revisit, with its contexts matched
        //  by the known text, applies: the later rules are not tried.
        bool afterImplied = alwaysMatches(after);
        for (const auto & a : p.after) afterImplied |= a.first == q.beyond.size() && implies(a.second, after, true);
        const bool unconditional = q.beyond.size() == p.beyond.size() && beforeImplied && afterImplied;
        noteTrigger(mRecord, g, p.applied.size() + 1, !unconditional, q.beyond.size() > p.beyond.size(), false);
        q.after.emplace_back(q.beyond.size(), std::move(after));
        //  The characters beyond referenced by the result are enumerated.
        std::set<std::string> names;
        const RuleSide * result = g.rule->getResultSide(Direction::Forward);
        collectReferences(result->getCompletedResult(), names);
        collectReferences(result->getResultToRevisit(), names);
        std::set<int> referenced;
        for (const std::string & n : names) {
            auto f = g.captures.find(n);
            if (f == g.captures.end()) {
                throw NotComposed{"a rule applying to the text to revisit has an unknown reference: " + printRule(g.rule)};
            }
            for (unsigned i = f->second.first; i < f->second.second; ++i) {
                if (matched[i].beyond >= 0) referenced.insert(matched[i].beyond);
            }
        }
        apply(g, std::move(q), matched, std::vector<int>(referenced.begin(), referenced.end()), text, composed);
        if (unconditional) return;
    }
    p.out.push_back(p.pending.front());
    p.pending.erase(0, 1);
    explore(std::move(p), text, composed);
}

//  Apply a rule matching on a path, with each of the characters beyond that
//  its result references enumerated.
void Composer::apply(const GroupRule & g, Path q, const std::vector<MatchedChar> & matched,
                     std::vector<int> referenced, RE * text, std::vector<Rule *> & composed) {
    if (!referenced.empty()) {
        const int b = referenced.back();
        referenced.pop_back();
        const UCD::UnicodeSet chars = q.beyond[b];
        if (chars.count() > MaxEnumeration) {
            throw NotComposed{"a reference to more than " + std::to_string(MaxEnumeration) + " characters: " + printRule(g.rule)};
        }
        for (const auto & range : chars) {
            for (codepoint_t c = range.first; c <= range.second; ++c) {
                Path r = q;
                r.beyond[b] = UCD::UnicodeSet(c);
                apply(g, std::move(r), matched, referenced, text, composed);
            }
        }
        return;
    }
    std::map<std::string, std::u32string> refs;
    for (const auto & cap : g.captures) {
        std::u32string s;
        for (unsigned i = cap.second.first; i < cap.second.second; ++i) {
            s.push_back(matched[i].beyond < 0 ? matched[i].c : static_cast<char32_t>(q.beyond[matched[i].beyond].min_codepoint()));
        }
        refs.emplace(cap.first, std::move(s));
    }
    const RuleSide * result = g.rule->getResultSide(Direction::Forward);
    if (result->hasCursor() && result->getCursorOffset() != 0) {
        throw NotComposed{"a rule applying to the text to revisit has a cursor offset: " + printRule(g.rule)};
    }
    std::u32string completed, revisit;
    if (!ruleResult(result, refs, completed, revisit)) {
        throw NotComposed{"a rule applying to the text to revisit has a result that is not a fixed string: " + printRule(g.rule)};
    }
    q.applied.push_back(g.rule);
    if (q.applied.size() > MaxApplications) {
        std::string rules;
        for (size_t k = 0; k < 3; k++) rules += (k ? " then " : "") + printRule(q.applied[k]);
        throw NotComposed{"the revisits continue for more than " + std::to_string(MaxApplications) + " rule applications: "
                          + rules + " ..."};
    }
    q.out += completed;
    q.pending = revisit + q.pending.substr(std::min(q.pending.size(), matched.size()));
    explore(std::move(q), text, composed);
}

//  The composed rule of a complete path.
void Composer::finish(Path p, RE * text, std::vector<Rule *> & composed) {
    if (mRecord) {
        mRecord->paths++;
        mRecord->maxApplications = std::max<unsigned>(mRecord->maxApplications, p.applied.size());
    }
    Context after;
    for (auto & a : p.after) {
        for (size_t k = a.first; k < p.beyond.size(); ++k) {
            if (consume(a.second, p.beyond[k], true) == Match::Fail) return;
        }
        if (conjoin(after, a.second, true) == Match::Fail) return;
    }
    std::vector<RE *> items{text};
    for (const UCD::UnicodeSet & s : p.beyond) {
        if (s.empty()) return;
        items.push_back(makeCC(s));
    }
    RE * const before = contextRE(p.before, true);
    RE * const fullText = p.beyond.empty() ? text : makeSeq(items.begin(), items.end());
    RE * const afterRE = contextRE(after, false);
    //  A rule whose condition is that of an earlier rule never applies.
    const std::string key = (before ? printPattern(before) : "") + " { " + printPattern(fullText) + " } " + (afterRE ? printPattern(afterRE) : "");
    if (!mEmitted.insert(key).second) return;
    composed.push_back(makeConversionRule(makeRuleSide(before, fullText, afterRE), Direction::Forward,
                                          makeRuleSide(literal(p.out))));
}

//  The text with captures replaced by characters.
RE * replaceCaptures(RE * re, const std::map<std::string, codepoint_t> & chars) {
    if (Capture * c = dyn_cast<Capture>(re)) {
        auto f = chars.find(c->getName());
        if (f != chars.end()) return makeCC(f->second);
        RE * const content = replaceCaptures(c->getCapturedRE(), chars);
        return content == c->getCapturedRE() ? c : makeCapture(c->getName(), content);
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> items;
        bool changed = false;
        for (RE * e : *seq) {
            items.push_back(replaceCaptures(e, chars));
            changed |= items.back() != e;
        }
        return changed ? makeSeq(items.begin(), items.end()) : re;
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        RE * const e = replaceCaptures(alt->front(), chars);
        return e == alt->front() ? re : e;
    }
    return re;
}

void findCaptures(RE * re, std::map<std::string, Capture *> & found, bool repeated) {
    if (Capture * c = dyn_cast<Capture>(re)) {
        if (repeated) throw NotComposed{"a referenced capture is within a repetition"};
        found[c->getName()] = c;
        findCaptures(c->getCapturedRE(), found, repeated);
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) findCaptures(e, found, repeated);
    } else if (Alt * alt = dyn_cast<Alt>(re)) {
        for (RE * e : *alt) findCaptures(e, found, repeated || alt->size() != 1);
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        findCaptures(rep->getRE(), found, true);
    }
}

std::vector<Rule *> Composer::composeRule(ConversionRule * r, const std::vector<GroupRule> & group) {
    mGroup = &group;
    mEmitted.clear();
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    const RuleSide * result = r->getResultSide(Direction::Forward);
    if (result->getCursorOffset() != 0) {
        throw NotComposed{"the cursor is placed beyond the result"};
    }
    //  The captures referenced by the result are enumerated.
    std::set<std::string> names;
    collectReferences(result->getCompletedResult(), names);
    collectReferences(result->getResultToRevisit(), names);
    std::map<std::string, Capture *> captures;
    findCaptures(source->getText(), captures, false);
    std::vector<std::pair<std::string, std::vector<codepoint_t>>> enumerated;
    for (const std::string & n : names) {
        auto f = captures.find(n);
        if (f == captures.end()) throw NotComposed{"the result has an unknown reference"};
        GroupRule content;
        textItems(f->second->getCapturedRE(), content);
        if (!content.fixed || content.text.size() != 1 || !content.captures.empty()) {
            throw NotComposed{"a referenced capture is not a single character"};
        }
        if (content.text.front().count() > MaxEnumeration) {
            throw NotComposed{"a referenced capture has more than " + std::to_string(MaxEnumeration) + " characters"};
        }
        std::vector<codepoint_t> chars;
        for (const auto & range : content.text.front()) {
            for (codepoint_t c = range.first; c <= range.second; ++c) chars.push_back(c);
        }
        enumerated.emplace_back(n, std::move(chars));
    }
    std::vector<Rule *> composed;
    std::vector<size_t> index(enumerated.size(), 0);
    for (;;) {
        std::map<std::string, codepoint_t> chars;
        std::map<std::string, std::u32string> refs;
        for (size_t k = 0; k < enumerated.size(); ++k) {
            if (enumerated[k].second.empty()) return composed;
            const codepoint_t c = enumerated[k].second[index[k]];
            chars.emplace(enumerated[k].first, c);
            refs.emplace(enumerated[k].first, std::u32string(1, static_cast<char32_t>(c)));
        }
        Path p;
        if (!ruleResult(result, refs, p.out, p.pending)) {
            throw NotComposed{"the result is not a fixed string"};
        }
        p.before = context(source->getBeforeContext());
        p.after.emplace_back(0, context(source->getAfterContext()));
        explore(std::move(p), replaceCaptures(source->getText(), chars), composed);
        size_t k = 0;
        for (; k < enumerated.size(); ++k) {
            if (++index[k] < enumerated[k].second.size()) break;
            index[k] = 0;
        }
        if (k == enumerated.size()) break;
    }
    return composed;
}

//  The atoms of a text: its items, through sequences and variables defined as
//  strings, with captures as single atoms.
void textAtoms(RE * re, std::vector<RE *> & atoms) {
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) textAtoms(e, atoms);
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        textAtoms(alt->front(), atoms);
    } else if (Name * name = dyn_cast<Name>(re);
               name && !isFunctionCall(name) && name->getDefinition() && !CharSetAnalysis::isSet(name->getDefinition())) {
        textAtoms(name->getDefinition(), atoms);
    } else {
        atoms.push_back(re);
    }
}

//  A rule whose text to revisit is the text matched by the end of its text
//  to replace,
//      before { P S } after → A | S ;
//  leaves the text as
//      before { P } S after → A ;
//  does, with the cursor before S.  Either rule (with the cursor as ICU places
//  it, see ruleResult) or nullptr.
Rule * Composer::suffixRevisit(ConversionRule * r) {
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    const RuleSide * result = r->getResultSide(Direction::Forward);
    if (result->getCursorOffset() != 0) return nullptr;
    std::vector<RE *> completed, revisit, text;
    textAtoms(result->getCompletedResult() ? result->getCompletedResult() : makeSeq(), completed);
    textAtoms(result->getResultToRevisit(), revisit);
    textAtoms(source->getText(), text);
    //  With references, the completed result must end with one (ICU then
    //  revisits only the text to revisit).
    std::set<std::string> names;
    collectReferences(result->getCompletedResult(), names);
    collectReferences(result->getResultToRevisit(), names);
    if (!names.empty() && !completed.empty() && !isa<Reference>(completed.back())) return nullptr;
    if (revisit.empty() || revisit.size() >= text.size()) return nullptr;
    const size_t p = text.size() - revisit.size();
    for (size_t k = 0; k < revisit.size(); k++) {
        RE * const atom = text[p + k];
        if (const Reference * ref = dyn_cast<Reference>(revisit[k])) {
            const Capture * c = dyn_cast<Capture>(atom);
            if (c == nullptr || c->getName() != ref->getName()) return nullptr;
        } else {
            if (!CharSetAnalysis::isSet(revisit[k]) || hasStrings(revisit[k])) return nullptr;
            const UCD::UnicodeSet c = mSets.setOf(revisit[k], false);
            if (c.count() != 1 || !CharSetAnalysis::isSet(atom) || hasStrings(atom) || hasBoundary(atom)) return nullptr;
            if (!(mSets.setOf(atom, false) == c)) return nullptr;
        }
    }
    //  The completed result may only refer to captures of P.
    std::set<std::string> completedNames;
    collectReferences(result->getCompletedResult(), completedNames);
    for (size_t k = p; k < text.size(); k++) {
        if (const Capture * c = dyn_cast<Capture>(text[k]); c && completedNames.count(c->getName())) return nullptr;
    }
    std::vector<RE *> after;
    for (size_t k = p; k < text.size(); k++) {
        const Capture * c = dyn_cast<Capture>(text[k]);
        after.push_back(c ? c->getCapturedRE() : text[k]);
    }
    if (source->getAfterContext()) after.push_back(source->getAfterContext());
    return makeConversionRule(makeRuleSide(source->getBeforeContext(), makeSeq(text.begin(), text.begin() + p),
                                           makeSeq(after.begin(), after.end())),
                              Direction::Forward, makeRuleSide(result->getCompletedResult() ? result->getCompletedResult() : makeSeq()));
}

bool hasRevisit(const ConversionRule * c) {
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (!result->hasCursor()) return false;
    const Seq * revisit = dyn_cast_or_null<Seq>(result->getResultToRevisit());
    const bool empty = result->getResultToRevisit() == nullptr || (revisit && revisit->empty());
    return !empty || result->getCursorOffset() != 0;
}

//  Whether the prefix runs of a group (isRun) can be applied before the other
//  rules of the group, as a group of their own: the empty string if so, or
//  the reason why not.  On P S^n x, ICU applies the prefix run n times and
//  then the rules of the group to P x, after A^n; applied first, the prefix
//  runs leave A^n P x, to which the rest of the group is applied.  The rest of
//  the group must therefore
//    - not apply at P S before a prefix run would (no rule before a prefix run
//      may match its P followed by S, in its text or after context), and not
//      insert,
//    - not match P after the first character of its text (ICU would not reach
//      P),
//    - not match A, which ICU has converted, unless ICU revisits A (as with
//      (P) S → A | $1 ;) and the rule matches A alone,
//    - not see P S^n replaced by A^n P in its after contexts: each set of
//      the after contexts contains all of the characters of P, S and A, or
//      none (and in the first case A has as many characters as S, so that the
//      lengths are the same); unassigned code points are disregarded,
//    - not revisit P followed by S, or at the end of the text to revisit (where
//      S may follow), as the prefix runs no longer apply then.
//  The before contexts see A^n P as ICU does.  Within the prefix runs, the
//  families must be as prefixRunFamilies requires, and A may not contain
//  characters of P.
//
//  A trigger, C → B | p ; (C a set, no contexts, B a fixed string, p a
//  character of a set P), produces the P of a prefix run, which ICU then
//  revisits.  The triggers are applied first, as C → B p ; (in a group of
//  their own, before the prefix runs), except where ICU does not reach C:
//  where a covering rule, k c → … ; (no contexts, c of a trigger), consumes
//  it with the character k before it.  Then
//    - no rule before a trigger may match its C (ICU would apply it instead),
//    - no rule matches the characters of C within its text, other than the
//      covering rules (and the texts S of the prefix runs, where each such
//      character must be covered as well, after the character before it),
//    - ICU applies the covering rules wherever k c occurs: no other rule
//      matching k followed by c comes before a covering rule of k c, and no
//      rule matches k after the first character of its text,
//    - no rule revisits a character of C, and B contains no character of C,
//      P, S or k (as A, B is not to be matched by the other rules).
//  covered gives the pairs (k, c) of the covering rules.
struct PrefixRunTrigger {
    UCD::UnicodeSet text;           // C
    std::u32string replacement;     // B
    char32_t revisit;               // p
};

std::string prefixRunConflict(const std::vector<Rule *> & rules, const std::vector<bool> & isRun,
                              const std::vector<PrefixRun> & runs, const std::vector<bool> & isTrigger,
                              const std::vector<PrefixRunTrigger> & triggers,
                              std::vector<std::pair<UCD::UnicodeSet, UCD::UnicodeSet>> & covered) {
    const std::string families = prefixRunFamilies(runs);
    if (!families.empty()) return families;
    UCD::UnicodeSet first, run, runStart, replaced;
    bool sameLength = true;
    bool revisitsReplacement = true;
    for (const PrefixRun & pr : runs) {
        first = first + pr.first;
        for (const UCD::UnicodeSet & s : pr.run) run = run + s;
        runStart = runStart + pr.run.front();
        for (char32_t c : pr.replacement) replaced.insert(c);
        sameLength &= pr.replacement.size() == pr.run.size();
        revisitsReplacement &= pr.revisitsReplacement;
    }
    if (replaced.intersects(first)) return "a replacement may begin a prefix run";
    //  The characters of the triggers, and the covering rules.
    UCD::UnicodeSet triggered, triggerOutput;
    for (const PrefixRunTrigger & tr : triggers) {
        triggered = triggered + tr.text;
        for (char32_t c : tr.replacement) triggerOutput.insert(c);
    }
    std::vector<bool> isCovering(rules.size(), false);
    UCD::UnicodeSet coveringFirst;
    covered.clear();
    for (size_t k = 0; k < rules.size() && !triggers.empty(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(rules[k]);
        if (c == nullptr || isRun[k] || isTrigger[k]) continue;
        const RuleSide * source = c->getSourceSide(Direction::Forward);
        const std::vector<PatternItem> text = patternItems(source->getText());
        if (text.size() == 2 && !text[0].repeated && !text[1].repeated && text[1].chars.intersects(triggered)
                && !source->hasBeforeContext() && !source->hasAfterContext()) {
            isCovering[k] = true;
            covered.emplace_back(text[0].chars, text[1].chars & triggered);
            coveringFirst = coveringFirst + text[0].chars;
        }
    }
    if (!triggers.empty()) {
        if (triggered.intersects(first) || triggered.intersects(coveringFirst) || first.intersects(coveringFirst)) {
            return "the characters of the triggers, of P and before the triggers intersect";
        }
        if (triggerOutput.intersects(triggered + first + run + coveringFirst)) {
            return "a trigger's replacement may be matched by a prefix run or a trigger";
        }
        //  The characters of C in the texts S, after the characters before them.
        for (const PrefixRun & pr : runs) {
            for (size_t j = 0; j < pr.run.size(); ++j) {
                const UCD::UnicodeSet c = pr.run[j] & triggered;
                if (c.empty()) continue;
                if (j == 0) return "a prefix run matches the character of a trigger after P";
                UCD::UnicodeSet reached;
                for (const auto & kc : covered) {
                    if (pr.run[j - 1].subset(kc.first)) reached = reached + kc.second;
                }
                if (!c.subset(reached)) return "a prefix run matches the character of a trigger not covered";
            }
        }
        replaced = replaced + triggerOutput;
    }
    static const UCD::UnicodeSet unassigned = UCD::getPropertyObject(UCD::gc)->GetCodepointSet("Cn");
    const UCD::UnicodeSet changed = (first + run + replaced + triggered) - unassigned;
    //  The prefix runs after each rule.
    std::vector<std::vector<const PrefixRun *>> later(rules.size() + 1);
    size_t r = runs.size();
    for (size_t k = rules.size(); k-- > 0; ) {
        later[k] = later[k + 1];
        if (isRun[k]) later[k].push_back(&runs[--r]);
    }
    //  The triggers after each rule.
    std::vector<UCD::UnicodeSet> laterTriggers(rules.size() + 1);
    size_t tk = triggers.size();
    for (size_t k = rules.size(); k-- > 0; ) {
        laterTriggers[k] = laterTriggers[k + 1];
        if (isTrigger[k]) laterTriggers[k] = laterTriggers[k] + triggers[--tk].text;
    }
    for (size_t k = 0; k < rules.size(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(rules[k]);
        if (c == nullptr || isRun[k] || isTrigger[k]) continue;
        const RuleSide * source = c->getSourceSide(Direction::Forward);
        const std::vector<PatternItem> text = patternItems(source->getText());
        if (text.empty()) return "an insertion rule may apply: " + printRule(c);
        if (!triggers.empty()) {
            if (text[0].chars.intersects(laterTriggers[k])) return "a rule before a trigger may apply instead: " + printRule(c);
            for (size_t i = 0; i < text.size(); ++i) {
                if ((i > 0 || text[i].repeated) && text[i].chars.intersects(triggered) && !isCovering[k]) {
                    return "a rule matches the character of a trigger within its text: " + printRule(c);
                }
                if ((i > 0 || text[i].repeated) && text[i].chars.intersects(coveringFirst)) {
                    return "a rule matches a character before a trigger within its text: " + printRule(c);
                }
            }
            //  A rule matching k followed by c before the covering rules of k c.
            if (!isCovering[k] && text[0].chars.intersects(coveringFirst)) {
                const UCD::UnicodeSet * next = nullptr;
                std::vector<PatternItem> after;
                if (text.size() > 1) {
                    next = &text[1].chars;
                } else if (source->hasAfterContext()) {
                    after = patternItems(source->getAfterContext());
                    if (!after.empty() && !after[0].repeated) next = &after[0].chars;
                }
                for (size_t j = k + 1; j < rules.size(); ++j) {
                    if (!isCovering[j]) continue;
                    const std::vector<PatternItem> cover = patternItems(cast<ConversionRule>(rules[j])->getSourceSide(Direction::Forward)->getText());
                    if (text[0].chars.intersects(cover[0].chars) && (next == nullptr || next->intersects(cover[1].chars & triggered))) {
                        return "a rule before a covering rule may apply instead: " + printRule(c);
                    }
                }
            }
            if (RE * revisit = c->getResultSide(Direction::Forward)->getResultToRevisit()) {
                for (const PatternItem & item : patternItems(revisit)) {
                    if (item.chars.intersects(triggered)) return "a rule revisits the character of a trigger: " + printRule(c);
                }
            }
        }
        const bool single = text.size() == 1 && !text[0].repeated;
        //  What may follow the first character of the text: the next item of the
        //  text, or of the after context (nullptr: anything).
        const UCD::UnicodeSet * next = nullptr;
        std::vector<PatternItem> after;
        if (!single) {
            if (!text[0].repeated) next = &text[1].chars;
        } else if (source->hasAfterContext()) {
            after = patternItems(source->getAfterContext());
            if (!after.empty() && !after[0].repeated) next = &after[0].chars;
        }
        for (const PrefixRun * pr : later[k]) {
            if (text[0].chars.intersects(pr->first) && (next == nullptr || next->intersects(pr->run.front()))) {
                return "a rule before a prefix run may apply instead: " + printRule(c);
            }
        }
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i].chars.intersects(replaced) && !(single && revisitsReplacement)) {
                return "a rule may match a replacement: " + printRule(c);
            }
            if ((i > 0 || text[i].repeated) && text[i].chars.intersects(first)) {
                return "a rule may match a character to revisit within its text: " + printRule(c);
            }
        }
        if (source->hasAfterContext()) {
            for (const PatternItem & item : patternItems(source->getAfterContext())) {
                if (item.chars.intersects(changed) && !(changed.subset(item.chars) && sameLength)) {
                    return "an after context may see the text of a prefix run: " + printRule(c);
                }
            }
        }
        if (RE * revisit = c->getResultSide(Direction::Forward)->getResultToRevisit()) {
            const std::vector<PatternItem> items = patternItems(revisit);
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].chars.intersects(first)
                        && (items[i].repeated || i + 1 == items.size() || items[i + 1].chars.intersects(runStart))) {
                    return "a rule revisits a character to revisit of a prefix run: " + printRule(c);
                }
            }
        }
    }
    return "";
}

//  A rule inserting a fixed string M between captures and revisiting all,
//      (T1) ... (Ti) (Ti+1) ... (Tk) → | $1 ... $i M $i+1 ... $k ;
//  (each Tj a set, or a repetition of a set, the first rule of its group, no
//  contexts), as an insertion rule applied before the group (in a group of
//  its own),
//      T1 ... Ti { } Ti+1 ... Tk → M ;
//  with the sets within the filter (contexts see the characters outside it,
//  which the text of the rule cannot match).  ICU tries the rule first at
//  each position the cursor reaches, on the text not converted yet, so the
//  insertion rule applies at the same positions where
//    - no other rule matches a character of T1 after the first character of
//      its text (the cursor reaches every character of T1),
//    - no after context of another rule may see a character of T1 (and the
//      insertion after it),
//    - M contains no character of T1 or T2 (ICU revisits M), nor does a text
//      that another rule revisits after its first character T2, and if its
//      last character may be in T1, the text of that rule is a single set
//      within T1 (ICU tried the rule there, followed by the same text).
bool insertionRevisit(const ConversionRule * c, const std::vector<Rule *> & group, const UCD::UnicodeSet * filter,
                      Rule *& insertion) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->hasBeforeContext() || source->hasAfterContext()) return false;
    if (!result->hasCursor() || result->getCursorOffset() != 0 || result->getResultToRevisit() == nullptr) return false;
    std::u32string completed;
    if (!resultString(result->getCompletedResult(), {}, completed) || !completed.empty()) return false;
    std::vector<RE *> text, revisit;
    textAtoms(source->getText(), text);
    textAtoms(result->getResultToRevisit(), revisit);
    if (text.size() < 2) return false;
    CharSetAnalysis sets;
    std::vector<UCD::UnicodeSet> items(text.size());
    std::vector<std::pair<int, int>> bounds(text.size());
    std::vector<std::string> names(text.size());
    for (size_t j = 0; j < text.size(); ++j) {
        const Capture * cap = dyn_cast<Capture>(text[j]);
        if (cap == nullptr) return false;
        names[j] = cap->getName();
        RE * re = cap->getCapturedRE();
        bounds[j] = {1, 1};
        if (const Rep * r = dyn_cast<Rep>(re)) {
            if (r->getLB() < 1) return false;
            bounds[j] = {r->getLB(), r->getUB()};
            re = r->getRE();
        }
        if (!CharSetAnalysis::isSet(re) || hasStrings(re) || hasBoundary(re)) return false;
        items[j] = sets.setOf(re, false);
        if (filter) items[j] = items[j] & *filter;
        if (items[j].empty()) return false;
    }
    //  The result: the references in order, with M between two of them.
    size_t next = 0, at = 0;
    std::u32string inserted;
    for (RE * e : revisit) {
        if (const Reference * ref = dyn_cast<Reference>(e)) {
            if (next == text.size() || ref->getName() != names[next]) return false;
            ++next;
        } else if (CharSetAnalysis::isSet(e) && !hasStrings(e)) {
            const UCD::UnicodeSet s = sets.setOf(e, false);
            if (s.count() != 1 || next == 0 || next == text.size() || (!inserted.empty() && at != next)) return false;
            at = next;
            inserted.push_back(static_cast<char32_t>(lo_codepoint(*s.begin())));
        } else {
            return false;
        }
    }
    if (next != text.size() || inserted.empty()) return false;
    UCD::UnicodeSet m;
    for (char32_t ch : inserted) m.insert(ch);
    if (m.intersects(items[0]) || m.intersects(items[1])) return false;
    for (const Rule * r : group) {
        const ConversionRule * other = dyn_cast<ConversionRule>(r);
        if (other == nullptr || other == c) continue;
        const RuleSide * os = other->getSourceSide(Direction::Forward);
        const std::vector<PatternItem> otherText = patternItems(os->getText());
        for (size_t i = 0; i < otherText.size(); ++i) {
            if ((i > 0 || otherText[i].repeated) && otherText[i].chars.intersects(items[0])) return false;
        }
        if (os->hasAfterContext()) {
            for (const PatternItem & item : patternItems(os->getAfterContext())) {
                if (item.chars.intersects(items[0])) return false;
            }
        }
        if (RE * rv = other->getResultSide(Direction::Forward)->getResultToRevisit()) {
            const std::vector<PatternItem> y = patternItems(rv);
            for (size_t i = 1; i < y.size(); ++i) {
                if (y[i].repeated || y[i].chars.intersects(items[1])) return false;
            }
            if (!y.empty() && (y.back().repeated || y.back().chars.intersects(items[0]))) {
                if (otherText.size() != 1 || otherText[0].repeated || !otherText[0].chars.subset(items[0])) return false;
            }
        }
    }
    auto item = [&](size_t j) {
        RE * const s = makeCC(items[j]);
        return bounds[j] == std::make_pair(1, 1) ? s : makeRep(s, bounds[j].first, bounds[j].second);
    };
    std::vector<RE *> before, after;
    for (size_t j = 0; j < at; ++j) before.push_back(item(j));
    for (size_t j = at; j < text.size(); ++j) after.push_back(item(j));
    insertion = makeConversionRule(makeRuleSide(makeSeq(before.begin(), before.end()), makeSeq(), makeSeq(after.begin(), after.end())),
                                   Direction::Forward, makeRuleSide(literal(inserted)));
    return true;
}

//  A trigger of the prefix runs (see prefixRunConflict): C → B | p ; with p
//  in first.
bool prefixRunTrigger(const ConversionRule * c, const UCD::UnicodeSet & first, PrefixRunTrigger & tr) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->hasBeforeContext() || source->hasAfterContext()) return false;
    if (!result->hasCursor() || result->getCursorOffset() != 0 || result->getResultToRevisit() == nullptr) return false;
    std::vector<RE *> text, revisit;
    textAtoms(source->getText(), text);
    textAtoms(result->getResultToRevisit(), revisit);
    if (text.size() != 1 || revisit.size() != 1) return false;
    if (!CharSetAnalysis::isSet(text[0]) || hasStrings(text[0]) || hasBoundary(text[0])) return false;
    if (!CharSetAnalysis::isSet(revisit[0]) || hasStrings(revisit[0])) return false;
    CharSetAnalysis sets;
    tr.text = sets.setOf(text[0], false);
    const UCD::UnicodeSet p = sets.setOf(revisit[0], false);
    if (tr.text.empty() || p.count() != 1 || !p.subset(first)) return false;
    tr.revisit = static_cast<char32_t>(lo_codepoint(*p.begin()));
    tr.replacement.clear();
    return resultString(result->getCompletedResult(), {}, tr.replacement);
}

//  A prefix run with A empty, P S → | P ; (S of one character), where every
//  other rule of the group that may match a character of P matches it alone
//  (a single character, no after context, a fixed result without a cursor).
//  ICU deletes the run S^n and applies to p the first of these rules whose
//  before context matches (or leaves it), as the run rules
//      before { p S+ → f(p) ;
//  do, one for each such rule (in order) and character p of P, until one
//  without a before context (or then p S+ → p ;).
bool prefixRunAsRun(const ConversionRule * c, const std::vector<Rule *> & group, std::vector<Rule *> & out) {
    PrefixRun pr;
    if (!prefixRun(c, pr) || !pr.replacement.empty() || pr.run.size() != 1 || pr.first.count() > 64) return false;
    struct AtP {
        const ConversionRule * rule;
        UCD::UnicodeSet text;
        std::string capture;        // the name of the capture of the text, if any
    };
    std::vector<AtP> atP;
    CharSetAnalysis sets;
    for (const Rule * r : group) {
        const ConversionRule * other = dyn_cast<ConversionRule>(r);
        if (other == nullptr || other == c) continue;
        const RuleSide * source = other->getSourceSide(Direction::Forward);
        const std::vector<PatternItem> items = patternItems(source->getText());
        if (items.empty()) return false;
        bool atFirst = items[0].chars.intersects(pr.first);
        for (const PatternItem & item : items) atFirst |= item.repeated && item.chars.intersects(pr.first);
        if (!atFirst) continue;
        std::vector<RE *> text;
        textAtoms(source->getText(), text);
        if (text.size() != 1 || items.size() != 1 || items[0].repeated || source->hasAfterContext()) return false;
        const RuleSide * result = other->getResultSide(Direction::Forward);
        if (result->hasCursor()) return false;
        AtP a{other, UCD::UnicodeSet(), ""};
        RE * set = text[0];
        if (const Capture * cap = dyn_cast<Capture>(set)) {
            a.capture = cap->getName();
            set = cap->getCapturedRE();
        }
        if (!CharSetAnalysis::isSet(set) || hasStrings(set) || hasBoundary(set)) return false;
        a.text = sets.setOf(set, false);
        atP.push_back(a);
    }
    RE * const run = makeRep(makeCC(pr.run.front()), 1, Rep::UNBOUNDED_REP);
    for (const auto & range : pr.first) {
        for (UCD::codepoint_t ch = lo_codepoint(range); ch <= hi_codepoint(range); ++ch) {
            RE * const text = makeSeq({makeCC(ch), run});
            bool unconditional = false;
            for (const AtP & a : atP) {
                if (!a.text.contains(ch)) continue;
                std::map<std::string, std::u32string> refs;
                if (!a.capture.empty()) refs[a.capture] = std::u32string(1, static_cast<char32_t>(ch));
                std::u32string result;
                if (!resultString(a.rule->getResultSide(Direction::Forward)->getCompletedResult(), refs, result)) return false;
                RE * const before = a.rule->getSourceSide(Direction::Forward)->getBeforeContext();
                out.push_back(makeConversionRule(makeRuleSide(before, text, nullptr), Direction::Forward, makeRuleSide(literal(result))));
                if (before == nullptr) {
                    unconditional = true;
                    break;
                }
            }
            if (!unconditional) {
                out.push_back(makeConversionRule(makeRuleSide(nullptr, text, nullptr), Direction::Forward,
                                                 makeRuleSide(makeCC(ch))));
            }
        }
    }
    return true;
}

//  Whether a rule of a group cannot see the difference made by applying the
//  chain revisits of the group (with first, E and the characters c that the
//  group revisits as one of E) before the group: where, in a match of the
//  rule (its text and after context, from a position before E), E is
//  followed by M1 ... Mk and A*, its items do not distinguish X from Y
//  (ICU replaces X only when it reaches E).  The items are matched
//  possessively: an optional item or a repetition is left only on a
//  character it does not match.  A rule whose text consumes E (after its
//  first character) must revisit, with ICU then applying the chain revisits
//  at E: its text to revisit is that from E on (a character of E, or the
//  characters of E as captured) followed by characters of A not in M,
//  inserted only after M1 ... Mk.  A rule whose text to revisit has a
//  character of E must be such a rule.
bool Composer::chainUnseen(const ConversionRule * r, const ChainRevisit & chain, const UCD::UnicodeSet & first,
                           std::string & reason) {
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    const RuleSide * result = r->getResultSide(Direction::Forward);
    std::vector<Item> items;
    if (!collectItems(source->getText(), items)) {
        reason = "the text of " + printRule(r) + " is not a sequence of sets";
        return false;
    }
    const size_t textItems = items.size();
    if (source->hasAfterContext() && !collectItems(source->getAfterContext(), items)) {
        reason = "the after context of " + printRule(r) + " is not a sequence of sets";
        return false;
    }
    //  E after the first character of the text (at the first, the chain
    //  revisits apply first, or, for c, as c → | e ;).
    bool consumes = false;
    for (size_t i = 0; i < textItems; ++i) consumes |= (i > 0 || items[i].star) && items[i].chars.intersects(first);
    const bool revisits = hasRevisit(r);
    bool inserts = false;       // characters of A after the text to revisit
    std::vector<RE *> revisit;
    bool revisitsFirst = false;
    if (revisits) {
        textAtoms(result->getResultToRevisit(), revisit);
        for (RE * e : revisit) {
            revisitsFirst |= CharSetAnalysis::isSet(e) && !hasStrings(e) && mSets.setOf(e, false).intersects(first);
        }
    }
    if (revisits && (consumes || revisitsFirst)) {
        //  The text to revisit, from the first atom of the text with E.
        std::vector<RE *> text;
        textAtoms(source->getText(), text);
        std::u32string completed;
        const bool all = result->getCursorOffset() == 0 && resultString(result->getCompletedResult(), {}, completed) && completed.empty();
        bool ok = all;
        size_t a = 0;
        for (; ok && a < text.size(); ++a) {
            std::vector<Item> atomItems;
            if (!collectItems(text[a], atomItems)) ok = false;
            bool hasFirst = false;
            for (const Item & item : atomItems) hasFirst |= item.chars.intersects(first);
            if (hasFirst) break;
        }
        size_t k = 0;
        auto literal = [&](RE * e, const UCD::UnicodeSet & within) {
            if (!CharSetAnalysis::isSet(e) || hasStrings(e)) return false;
            const UCD::UnicodeSet c = mSets.setOf(e, false);
            return c.count() == 1 && c.subset(within);
        };
        if (ok && a < text.size() && !isa<Capture>(text[a])) {
            //  A character of E replacing the atom (a set within E and c).
            ok = CharSetAnalysis::isSet(text[a]) && !hasStrings(text[a]) && mSets.setOf(text[a], false).subset(first)
                 && !revisit.empty() && literal(revisit[0], chain.first);
            k = 1;
            ++a;
        }
        for (; ok && a < text.size(); ++a, ++k) {
            const Capture * c = dyn_cast<Capture>(text[a]);
            const Reference * ref = k < revisit.size() ? dyn_cast<Reference>(revisit[k]) : nullptr;
            ok = c && ref && ref->getName() == c->getName();
        }
        UCD::UnicodeSet m;
        for (const UCD::UnicodeSet & f : chain.fixed) m = m + f;
        for (; ok && k < revisit.size(); ++k) {
            ok = literal(revisit[k], chain.star - m);
            inserts = true;
        }
        //  Or, at its first character, a character replaced by one revisited
        //  (as c → | e ; where not followed by M1, or else c is in E).
        if (!ok && !consumes) {
            ok = all && text.size() == 1 && CharSetAnalysis::isSet(text[0]) && !hasStrings(text[0]) && revisit.size() == 1
                 && literal(revisit[0], ~UCD::UnicodeSet());
        }
        if (!ok) {
            reason = printRule(r) + (consumes ? " matches a character of E and does not revisit it as the chain revisits require"
                                              : " revisits a character of E");
            return false;
        }
    }
    //  The matches from a position before E: states (item, chain position,
    //  characters excluded by the optional items and repetitions left, and
    //  whether E was matched by the text).
    const size_t k = chain.fixed.size();
    struct State {
        size_t item;
        size_t at;              // M1 ... Mk, then k: within [A X]*
        UCD::UnicodeSet excluded;
        bool fromText;
    };
    std::vector<State> pending, seen;
    auto push = [&](State st) {
        //  Leaving optional items and repetitions.
        while (true) {
            for (const State & o : seen) {
                if (o.item == st.item && o.at == st.at && o.excluded == st.excluded && o.fromText == st.fromText) return;
            }
            seen.push_back(st);
            pending.push_back(st);
            if (st.item < items.size() && (items[st.item].optional || items[st.item].star)) {
                st.excluded = st.excluded + items[st.item].chars;
                st.item++;
            } else {
                return;
            }
        }
    };
    auto next = [&](size_t i) {return items[i].star ? i : i + 1;};
    for (size_t i = 0; i < items.size(); ++i) {
        if ((i > 0 || items[i].star) && items[i].chars.intersects(first)) push(State{next(i), 0, UCD::UnicodeSet(), i < textItems});
    }
    while (!pending.empty()) {
        const State st = pending.back();
        pending.pop_back();
        //  The characters that may come next (in the chain).
        const UCD::UnicodeSet following = (st.at < k ? chain.fixed[st.at] : chain.star + chain.last) - st.excluded;
        if (following.empty()) continue;
        if (st.fromText && st.item == textItems) {
            //  The end of the text, with E consumed: ICU reaches E again only
            //  by revisiting it, and then E M must not be separated.
            if (!revisits) {
                reason = printRule(r) + " matches a character of E (followed by M) and does not revisit it";
                return false;
            }
            if (inserts && st.at < k) {
                reason = printRule(r) + " inserts characters between E and M";
                return false;
            }
        }
        if (st.item >= items.size()) continue;
        const Item & item = items[st.item];
        if (st.at < k) {
            if (following.intersects(item.chars)) push(State{next(st.item), st.at + 1, UCD::UnicodeSet(), st.fromText});
            continue;
        }
        if ((chain.star - st.excluded).intersects(item.chars)) push(State{next(st.item), k, UCD::UnicodeSet(), st.fromText});
        //  A character of X (Y once the chain revisits are applied first).
        const UCD::UnicodeSet x = chain.last - st.excluded;
        if (x.empty()) continue;
        const bool yIn = item.chars.contains(chain.replacement);
        if (st.excluded.contains(chain.replacement) || (x.intersects(item.chars) && !(yIn && x.subset(item.chars)))
                || (yIn && !x.intersects(item.chars))) {
            reason = printRule(r) + " may see a character of X that a chain revisit replaces";
            return false;
        }
        if (yIn) push(State{next(st.item), k, UCD::UnicodeSet(), st.fromText});
    }
    return true;
}

//  The chain revisits of a group (see ComposeRevisits; isChain), if they may
//  be applied before the rest of the group, in a group of their own:
//    - all with the same E, M, A and Y (chain, with X the union), within the
//      filter (Y as well, which ICU revisits),
//    - no rule before them may begin at a character of E (ICU would apply it
//      at E instead),
//    - the characters c that the first rule applying at c followed by M1
//      replaces by a character of E, revisited, c → | e ; (without contexts),
//      are added to E (ICU revisits e M1 ...), where no other rule may apply
//      at c followed by M1 before it,
//    - no other rule can see the difference (chainUnseen).
bool Composer::chainRevisitSplit(const std::vector<Rule *> & group, const UCD::UnicodeSet * filter,
                                 std::vector<bool> & isChain, ChainRevisit & chain, std::string & reason) {
    isChain.assign(group.size(), false);
    bool any = false;
    size_t last = 0;
    for (size_t k = 0; k < group.size(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
        ChainRevisit ch;
        if (c == nullptr || !hasRevisit(c) || !chainRevisit(c, ch)) continue;
        if (any) {
            if (!(ch.first == chain.first) || ch.fixed != chain.fixed || !(ch.star == chain.star) || ch.replacement != chain.replacement) {
                reason = "chain revisits with different sets";
                return false;
            }
            chain.last = chain.last + ch.last;
        } else {
            chain = ch;
            any = true;
        }
        isChain[k] = true;
        last = k;
    }
    if (!any) return false;
    if (filter) {
        chain.first = chain.first & *filter;
        for (UCD::UnicodeSet & m : chain.fixed) m = m & *filter;
        chain.star = chain.star & *filter;
        chain.last = chain.last & *filter;
        bool empty = chain.first.empty() || chain.last.empty() || !filter->contains(chain.replacement);
        for (const UCD::UnicodeSet & m : chain.fixed) empty |= m.empty();
        if (empty) {
            reason = "chain revisits outside the filter";
            return false;
        }
    }
    //  The characters with which a rule may begin.
    auto firstOf = [&](const ConversionRule * r, UCD::UnicodeSet & f) {
        const RuleSide * source = r->getSourceSide(Direction::Forward);
        bool nullable = false;
        f = UCD::UnicodeSet();
        if (!firstChars(source->getText(), f, nullable)) return false;
        if (nullable && (!source->hasAfterContext() || !firstChars(source->getAfterContext(), f, nullable) || nullable)) return false;
        return true;
    };
    for (size_t k = 0; k < last; ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
        if (c == nullptr || isChain[k]) continue;
        UCD::UnicodeSet f;
        if (!firstOf(c, f) || f.intersects(chain.first)) {
            reason = printRule(c) + " may apply at E before the chain revisits";
            return false;
        }
    }
    //  c → | e ; without contexts, e in E.
    auto revisitsAsFirst = [&](const ConversionRule * r) {
        const RuleSide * source = r->getSourceSide(Direction::Forward);
        const RuleSide * result = r->getResultSide(Direction::Forward);
        if (!hasRevisit(r) || source->hasBeforeContext() || source->hasAfterContext() || result->getCursorOffset() != 0) return false;
        std::u32string completed;
        if (!resultString(result->getCompletedResult(), {}, completed) || !completed.empty()) return false;
        std::vector<RE *> text, revisit;
        textAtoms(source->getText(), text);
        textAtoms(result->getResultToRevisit(), revisit);
        if (text.size() != 1 || revisit.size() != 1 || !CharSetAnalysis::isSet(text[0]) || hasStrings(text[0]) || hasBoundary(text[0])) return false;
        if (!CharSetAnalysis::isSet(revisit[0]) || hasStrings(revisit[0])) return false;
        const UCD::UnicodeSet e = mSets.setOf(revisit[0], false);
        return e.count() == 1 && e.subset(chain.first);
    };
    UCD::UnicodeSet candidates;
    for (size_t k = 0; k < group.size(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
        if (c == nullptr || isChain[k] || !revisitsAsFirst(c)) continue;
        std::vector<RE *> text;
        textAtoms(c->getSourceSide(Direction::Forward)->getText(), text);
        candidates = candidates + (mSets.setOf(text[0], false) - chain.first);
    }
    if (filter) candidates = candidates & *filter;
    if (candidates.count() > 256) {
        reason = "too many characters revisited as E";
        return false;
    }
    const UCD::UnicodeSet m1 = chain.fixed.empty() ? chain.star + chain.last : chain.fixed.front();
    UCD::UnicodeSet first = chain.first;
    for (const auto & range : candidates) {
        for (UCD::codepoint_t ch = lo_codepoint(range); ch <= hi_codepoint(range); ++ch) {
            for (size_t k = 0; k < group.size(); ++k) {
                const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
                if (c == nullptr || isChain[k]) continue;
                UCD::UnicodeSet f;
                if (!firstOf(c, f)) {
                    reason = "the first characters of " + printRule(c) + " are not known";
                    return false;
                }
                if (!f.contains(ch)) continue;
                const RuleSide * source = c->getSourceSide(Direction::Forward);
                std::vector<Item> items;
                bool ok = collectItems(source->getText(), items) && (!source->hasAfterContext() || collectItems(source->getAfterContext(), items));
                ok = ok && !items.empty() && !items[0].optional && !items[0].star && items[0].chars.contains(ch);
                if (ok && items.size() > 1) {
                    if (!items[1].optional && !items[1].star && !items[1].chars.intersects(m1)) continue;
                    ok = false;
                }
                if (!ok || source->hasBeforeContext()) {
                    reason = printRule(c) + " may apply at a character revisited as E";
                    return false;
                }
                if (revisitsAsFirst(c)) first.insert(ch);
                break;
            }
        }
    }
    for (size_t k = 0; k < group.size(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
        if (c && !isChain[k] && !chainUnseen(c, chain, first, reason)) return false;
    }
    chain.first = first;
    return true;
}

//  A rule that the mark run edits keep before them (see markRunSplit): its
//  text and after context sequences of sets, without a before context.
bool Composer::keptCandidate(const ConversionRule * r) {
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    if (source->hasBeforeContext()) return false;
    std::vector<Item> items;
    if (!collectItems(source->getText(), items) || items.empty()) return false;
    if (source->hasAfterContext() && !collectItems(source->getAfterContext(), items)) return false;
    for (const Item & item : items) {
        if (item.star || item.optional || item.boundary || item.chars.empty()) return false;
    }
    return true;
}

//  Whether a rule of a group cannot see the difference made by applying a
//  mark run edit e (P R* T → C, the run revisited) before the group, where
//  it matches from a position before P (matched possessively, as in
//  chainUnseen): its text may not match P (ICU would not reach P), and its
//  after context, over P, must treat the characters of P and C alike (C
//  as long as the part of P it sees), and may not reach T (deleted).
bool Composer::markRunUnseen(const ConversionRule * r, const MarkRunEdit & e, std::string & reason) {
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    std::vector<Item> items;
    if (!collectItems(source->getText(), items)) {
        reason = "the text of " + printRule(r) + " is not a sequence of sets";
        return false;
    }
    const size_t textItems = items.size();
    if (source->hasAfterContext() && !collectItems(source->getAfterContext(), items)) {
        reason = "the after context of " + printRule(r) + " is not a sequence of sets";
        return false;
    }
    //  The pattern: P, R (if R+), R*, T.
    struct Element {
        UCD::UnicodeSet chars;
        bool star;
        int prefix;             // its position in P, or -1
        bool suffix;
    };
    std::vector<Element> pattern;
    for (size_t j = 0; j < e.prefix.size(); ++j) pattern.push_back(Element{e.prefix[j], false, static_cast<int>(j), false});
    if (e.nonEmpty) pattern.push_back(Element{e.run, false, -1, false});
    pattern.push_back(Element{e.run, true, -1, false});
    for (const UCD::UnicodeSet & t : e.suffix) pattern.push_back(Element{t, false, -1, true});
    struct State {
        size_t item;
        size_t at;
        UCD::UnicodeSet excluded;
        bool fromText;
    };
    std::vector<State> pending, seen;
    auto push = [&](State st) {
        while (true) {
            //  The run may end (T, after it, has no character of R).
            if (st.at < pattern.size() && pattern[st.at].star) {
                State past = st;
                past.at++;
                bool known = false;
                for (const State & o : seen) known |= o.item == past.item && o.at == past.at && o.excluded == past.excluded && o.fromText == past.fromText;
                if (!known) {
                    seen.push_back(past);
                    pending.push_back(past);
                }
            }
            for (const State & o : seen) {
                if (o.item == st.item && o.at == st.at && o.excluded == st.excluded && o.fromText == st.fromText) return;
            }
            seen.push_back(st);
            pending.push_back(st);
            if (st.item < items.size() && (items[st.item].optional || items[st.item].star)) {
                st.excluded = st.excluded + items[st.item].chars;
                st.item++;
            } else {
                return;
            }
        }
    };
    auto next = [&](size_t i) {return items[i].star ? i : i + 1;};
    for (size_t i = 0; i < items.size(); ++i) {
        //  From the second character of the text (or within a repetition), or
        //  in the after context (from its first, for an insertion).
        if (!((i > 0 || items[i].star || i >= textItems) && items[i].chars.intersects(e.prefix.front()))) continue;
        push(State{i, 0, UCD::UnicodeSet(), i < textItems});
    }
    while (!pending.empty()) {
        const State st = pending.back();
        pending.pop_back();
        if (st.fromText && (st.item >= textItems || st.at == pattern.size())) {
            reason = printRule(r) + " may match the text of a mark run edit after the first character of its text";
            return false;
        }
        if (st.item >= items.size() || st.at == pattern.size()) continue;
        const Item & item = items[st.item];
        const Element & el = pattern[st.at];
        const UCD::UnicodeSet chars = (el.chars - st.excluded) & item.chars;
        if (el.suffix && !chars.empty()) {
            reason = printRule(r) + " may see the text deleted by a mark run edit";
            return false;
        }
        if (el.prefix >= 0 && !st.fromText) {
            //  ICU sees P, the rest of the group C there.
            const size_t j = static_cast<size_t>(el.prefix);
            const UCD::UnicodeSet p = el.chars - st.excluded;
            if (p.empty()) continue;
            const bool pIn = p.intersects(item.chars);
            if (!pIn && (j >= e.replacement.size() || !item.chars.contains(e.replacement[j]))) continue;
            if (j >= e.replacement.size() || !p.subset(item.chars) || !item.chars.contains(e.replacement[j])
                    || st.excluded.contains(e.replacement[j])) {
                reason = printRule(r) + " may see the text replaced by a mark run edit";
                return false;
            }
        }
        if (!chars.empty()) push(State{next(st.item), el.star ? st.at : st.at + 1, UCD::UnicodeSet(), st.fromText});
    }
    return true;
}

//  The mark run edits of a group (see ComposeRevisits; isEdit), if they may
//  be applied before the rest of the group, in a group of their own:
//    - within the filter;
//    - each rule before a mark run edit that may begin at its P either
//      cannot match where it does, or is kept before the edits (a text and
//      after context of sets, no before context; isKept);
//    - no other rule can see the difference (markRunUnseen), nor match a
//      character of C (which ICU does not revisit).
//  The edits that do not pass are left in the group (and are then among the
//  other rules for those that do).
bool Composer::markRunSplit(const std::vector<Rule *> & group, const UCD::UnicodeSet * filter,
                            std::vector<bool> & isEdit, std::vector<bool> & isKept, std::string & reason) {
    isEdit.assign(group.size(), false);
    isKept.assign(group.size(), false);
    std::vector<MarkRunEdit> edits;
    std::vector<size_t> at;
    for (size_t k = 0; k < group.size(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
        MarkRunEdit e;
        if (c == nullptr || !hasRevisit(c) || !markRunEdit(c, e)) continue;
        if (filter) {
            for (UCD::UnicodeSet & p : e.prefix) p = p & *filter;
            e.run = e.run & *filter;
            e.firstRun = e.firstRun & *filter;
            for (UCD::UnicodeSet & t : e.suffix) t = t & *filter;
            bool empty = false;
            for (const UCD::UnicodeSet & p : e.prefix) empty |= p.empty();
            for (const UCD::UnicodeSet & t : e.suffix) empty |= t.empty();
            if (empty) {
                reason = "a mark run edit outside the filter";
                return false;
            }
        }
        edits.push_back(e);
        at.push_back(k);
        isEdit[k] = true;
    }
    //  Each edit against the other rules (and the edits left), until those
    //  left pass.
    auto passes = [&](size_t n) {
        for (size_t k = 0; k < group.size(); ++k) {
            const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
            if (c == nullptr || isEdit[k]) continue;
            const RuleSide * source = c->getSourceSide(Direction::Forward);
            std::vector<Item> text;
            if (!collectItems(source->getText(), text)) {
                reason = "the text of " + printRule(c) + " is not a sequence of sets";
                return false;
            }
            for (const Item & item : text) {
                for (char32_t ch : edits[n].replacement) {
                    if (item.chars.contains(ch)) {
                        reason = printRule(c) + " may match a character that a mark run edit leaves";
                        return false;
                    }
                }
            }
            if (k < at[n]) {
                //  Before the edit: may it apply at its P instead?
                UCD::UnicodeSet first;
                bool nullable = false;
                if (!firstChars(source->getText(), first, nullable)
                        || (nullable && (!source->hasAfterContext() || !firstChars(source->getAfterContext(), first, nullable)))) {
                    reason = "the first characters of " + printRule(c) + " are not known";
                    return false;
                }
                if (first.intersects(edits[n].prefix.front()) && !keptCandidate(c)) {
                    reason = printRule(c) + " may apply at the text of a mark run edit before it";
                    return false;
                }
            }
            if (!markRunUnseen(c, edits[n], reason)) return false;
        }
        return true;
    };
    for (size_t n = 0; n < edits.size(); ) {
        if (passes(n)) {
            ++n;
            continue;
        }
        isEdit[at[n]] = false;
        edits.erase(edits.begin() + n);
        at.erase(at.begin() + n);
        n = 0;
    }
    if (edits.empty()) return false;
    //  The rules kept before the edits.
    for (size_t k = 0; k < group.size(); ++k) {
        const ConversionRule * c = dyn_cast<ConversionRule>(group[k]);
        if (c == nullptr || isEdit[k]) continue;
        for (size_t n = 0; n < edits.size(); ++n) {
            UCD::UnicodeSet first;
            bool nullable = false;
            const RuleSide * source = c->getSourceSide(Direction::Forward);
            firstChars(source->getText(), first, nullable);
            if (nullable && source->hasAfterContext()) firstChars(source->getAfterContext(), first, nullable);
            if (k < at[n] && first.intersects(edits[n].prefix.front())) isKept[k] = true;
        }
    }
    return true;
}

std::vector<Rule *> Composer::compose(const std::vector<Rule *> & rules) {
    std::vector<Rule *> out;
    //  The global filter (contexts see the characters outside it).
    std::optional<UCD::UnicodeSet> filter;
    for (const Rule * r : rules) {
        if (const FilterRule * f = dyn_cast<FilterRule>(r); f && !f->isInverse()) {
            CharSetAnalysis sets;
            filter = sets.setOf(f->getFilterSet(), false);
            break;
        }
    }
    size_t start = 0;
    while (start < rules.size()) {
        //  A group: the conversion rules (and variable definitions) between
        //  filter and transform rules.
        size_t end = start;
        while (end < rules.size() && (isa<ConversionRule>(rules[end]) || isa<VariableDefinitionRule>(rules[end]))) ++end;
        if (end == start) {
            out.push_back(rules[start++]);
            continue;
        }
        //  First the rules whose text to revisit is the end of their text, which
        //  are rewritten (and applied as rewritten in the compositions).
        std::vector<Rule *> rewritten(rules.begin() + start, rules.begin() + end);
        //  A first rule inserting a string between its captures and revisiting
        //  all, as an insertion rule in a group before (see insertionRevisit).
        for (size_t k = 0; k < rewritten.size(); ++k) {
            ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k]);
            if (c == nullptr) continue;
            Rule * insertion = nullptr;
            if (hasRevisit(c) && insertionRevisit(c, rewritten, filter ? &*filter : nullptr, insertion)) {
                if (mStats) {
                    mStats->candidates++;
                    mStats->composed++;
                    mStats->composedRules++;
                    if (mStats->keepRecords) {
                        mStats->records.push_back(RevisitRecord{c, RevisitRecord::Outcome::Composed});
                        mStats->records.back().composedRules = 1;
                        mStats->records.back().composed.push_back(insertion);
                    }
                }
                out.push_back(insertion);
                out.push_back(makeTransformRule(TransformID("Null")));
                rewritten.erase(rewritten.begin() + k);
            }
            break;
        }
        //  The chain revisits, in a group of their own before (see
        //  chainRevisitSplit), with E extended.
        std::vector<bool> isChain;
        ChainRevisit chain;
        std::string chainReason;
        bool split = false;
        if (chainRevisitSplit(rewritten, filter ? &*filter : nullptr, isChain, chain, chainReason)) {
            split = true;
            std::vector<Rule *> rest;
            for (size_t k = 0; k < rewritten.size(); ++k) {
                if (!isChain[k]) {
                    rest.push_back(rewritten[k]);
                    continue;
                }
                ConversionRule * c = cast<ConversionRule>(rewritten[k]);
                ChainRevisit own;
                chainRevisit(c, own);
                std::vector<RE *> text;
                textAtoms(c->getSourceSide(Direction::Forward)->getText(), text);
                std::vector<RE *> captured{makeCC(chain.first)};
                for (const UCD::UnicodeSet & m : chain.fixed) captured.push_back(makeCC(m));
                captured.push_back(makeRep(makeCC(chain.star), 0, Rep::UNBOUNDED_REP));
                const std::string name = cast<Capture>(text[0])->getName();
                Capture * const capture = makeCapture(name, makeSeq(captured.begin(), captured.end()));
                RE * const last = makeCC(filter ? own.last & *filter : own.last);
                Rule * const r = makeConversionRule(makeRuleSide(nullptr, makeSeq({capture, last}), nullptr), Direction::Forward,
                                                    makeRuleSide(nullptr, makeSeq(), makeSeq({makeReference(name, capture, 0),
                                                                                              makeCC(chain.replacement)}), 0, nullptr));
                out.push_back(r);
                if (mStats) {
                    mStats->candidates++;
                    mStats->chainRevisits++;
                    if (mStats->keepRecords) {
                        mStats->records.push_back(RevisitRecord{c, RevisitRecord::Outcome::ChainRevisit});
                        mStats->records.back().composedRules = 1;
                        mStats->records.back().composed.push_back(r);
                    }
                }
            }
            out.push_back(makeTransformRule(TransformID("Null")));
            rewritten = std::move(rest);
        }
        //  The mark run edits, in a group of their own before (see
        //  markRunSplit), after the rules kept there (leaving their text).
        std::vector<bool> isEdit, isKept;
        std::string editReason;
        if (!split && markRunSplit(rewritten, filter ? &*filter : nullptr, isEdit, isKept, editReason)) {
            std::vector<Rule *> rest;
            for (size_t k = 0; k < rewritten.size(); ++k) {
                ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k]);
                if (isKept[k]) {
                    const RuleSide * source = c->getSourceSide(Direction::Forward);
                    std::vector<Item> text, after;
                    collectItems(source->getText(), text);
                    if (source->hasAfterContext()) collectItems(source->getAfterContext(), after);
                    std::vector<RE *> t, a;
                    for (const Item & item : text) t.push_back(makeCC(item.chars));
                    for (const Item & item : after) a.push_back(makeCC(item.chars));
                    Capture * const capture = makeCapture("kept", makeSeq(t.begin(), t.end()));
                    out.push_back(makeConversionRule(makeRuleSide(nullptr, capture, a.empty() ? nullptr : makeSeq(a.begin(), a.end())),
                                                     Direction::Forward, makeRuleSide(makeReference("kept", capture, 0))));
                }
                if (!isEdit[k]) {
                    rest.push_back(rewritten[k]);
                    continue;
                }
                out.push_back(c);
                if (mStats) {
                    mStats->candidates++;
                    mStats->markRunEdits++;
                    if (mStats->keepRecords) {
                        mStats->records.push_back(RevisitRecord{c, RevisitRecord::Outcome::MarkRunEdit});
                        mStats->records.back().composedRules = 1;
                        mStats->records.back().composed.push_back(c);
                    }
                }
            }
            out.push_back(makeTransformRule(TransformID("Null")));
            rewritten = std::move(rest);
        }
        std::vector<bool> isRewritten(rewritten.size(), false);
        for (Rule *& r : rewritten) {
            ConversionRule * c = dyn_cast<ConversionRule>(r);
            if (c == nullptr || !hasRevisit(c)) continue;
            if (mStats) mStats->candidates++;
            if (Rule * s = suffixRevisit(c)) {
                if (mStats) {
                    mStats->composed++;
                    mStats->composedRules++;
                    if (mStats->keepRecords) {
                        mStats->records.push_back(RevisitRecord{c, RevisitRecord::Outcome::Suffix});
                        mStats->records.back().composedRules = 1;
                        mStats->records.back().composed.push_back(s);
                    }
                }
                isRewritten[&r - rewritten.data()] = true;
                r = s;
            }
        }
        //  Then the prefix runs, moved to a group of their own if the rest of the
        //  group cannot see the difference.
        std::vector<bool> isRun(rewritten.size(), false);
        std::vector<PrefixRun> runs;
        for (size_t k = 0; k < rewritten.size(); ++k) {
            ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k]);
            PrefixRun pr;
            if (c && !isRewritten[k] && hasRevisit(c) && prefixRun(c, pr)) {
                isRun[k] = true;
                runs.push_back(pr);
            }
        }
        //  And the triggers of the prefix runs, if all such rules are.
        std::vector<bool> isTrigger(rewritten.size(), false);
        std::vector<PrefixRunTrigger> triggers;
        UCD::UnicodeSet runFirst;
        for (const PrefixRun & pr : runs) runFirst = runFirst + pr.first;
        for (size_t k = 0; k < rewritten.size() && !runs.empty(); ++k) {
            ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k]);
            PrefixRunTrigger tr;
            if (c && !isRun[k] && !isRewritten[k] && hasRevisit(c) && prefixRunTrigger(c, runFirst, tr)) {
                isTrigger[k] = true;
                triggers.push_back(tr);
            }
        }
        std::vector<std::pair<UCD::UnicodeSet, UCD::UnicodeSet>> covered;
        std::string runConflict = runs.empty() ? "" : prefixRunConflict(rewritten, isRun, runs, isTrigger, triggers, covered);
        if (!triggers.empty() && !runConflict.empty()) {
            //  Without the triggers (whose P are then revisited by ICU only in the rest of the group).
            const std::string withTriggers = runConflict;
            std::vector<bool> none(rewritten.size(), false);
            runConflict = prefixRunConflict(rewritten, isRun, runs, none, {}, covered);
            if (!runConflict.empty()) runConflict = withTriggers;
            isTrigger = none;
            triggers.clear();
        }
        if (!runs.empty() && runConflict.empty()) {
            //  The triggers first, C → B p ; except after the characters k of
            //  the covering rules k c (k { c → c ;).
            if (!triggers.empty()) {
                for (const auto & kc : covered) {
                    for (const auto & range : kc.second) {
                        for (UCD::codepoint_t ch = lo_codepoint(range); ch <= hi_codepoint(range); ++ch) {
                            out.push_back(makeConversionRule(makeRuleSide(makeCC(kc.first), makeCC(ch), nullptr), Direction::Forward,
                                                             makeRuleSide(makeCC(ch))));
                        }
                    }
                }
                size_t tr = 0;
                for (size_t k = 0; k < rewritten.size(); ++k) {
                    if (!isTrigger[k]) continue;
                    const PrefixRunTrigger & trigger = triggers[tr++];
                    out.push_back(makeConversionRule(makeRuleSide(makeCC(trigger.text)), Direction::Forward,
                                                     makeRuleSide(literal(trigger.replacement + std::u32string(1, trigger.revisit)))));
                    if (mStats) {
                        mStats->prefixRuns++;
                        if (mStats->keepRecords) mStats->records.push_back(RevisitRecord{rewritten[k], RevisitRecord::Outcome::PrefixRunTrigger});
                    }
                }
                out.push_back(makeTransformRule(TransformID("Null")));
            }
            std::vector<Rule *> rest;
            std::vector<bool> restRewritten;
            for (size_t k = 0; k < rewritten.size(); ++k) {
                if (isTrigger[k]) continue;
                if (!isRun[k]) {
                    rest.push_back(rewritten[k]);
                    restRewritten.push_back(isRewritten[k]);
                    continue;
                }
                out.push_back(rewritten[k]);
                if (mStats) {
                    mStats->prefixRuns++;
                    if (mStats->keepRecords) mStats->records.push_back(RevisitRecord{rewritten[k], RevisitRecord::Outcome::PrefixRun});
                }
            }
            out.push_back(makeTransformRule(TransformID("Null")));
            rewritten = std::move(rest);
            isRewritten = std::move(restRewritten);
            isRun.assign(rewritten.size(), false);
        }
        std::vector<GroupRule> group;
        for (size_t k = 0; k < rewritten.size(); ++k) {
            if (ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k])) {
                group.push_back(groupRule(c));
                group.back().revisits = isRewritten[k] || hasRevisit(c);
            }
        }
        for (size_t k = 0; k < rewritten.size(); ++k) {
            ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k]);
            if (c == nullptr || isRewritten[k] || !hasRevisit(c)) {
                out.push_back(rewritten[k]);
                continue;
            }
            RevisitRecord record{c, RevisitRecord::Outcome::Composed};
            mRecord = mStats && mStats->keepRecords ? &record : nullptr;
            try {
                std::vector<Rule *> composed = composeRule(c, group);
                if (mStats) {
                    mStats->composed++;
                    mStats->composedRules += composed.size();
                }
                record.composedRules = composed.size();
                record.composed.assign(composed.begin(), composed.end());
                out.insert(out.end(), composed.begin(), composed.end());
            } catch (const NotComposed & e) {
                //  A prefix run with A empty, as a run rule.
                if (isRun[k]) {
                    std::vector<Rule *> runRules;
                    if (prefixRunAsRun(c, rewritten, runRules)) {
                        if (mStats) {
                            mStats->composed++;
                            mStats->composedRules += runRules.size();
                        }
                        record.composedRules = runRules.size();
                        record.composed.assign(runRules.begin(), runRules.end());
                        out.insert(out.end(), runRules.begin(), runRules.end());
                        if (mRecord) mStats->records.push_back(std::move(record));
                        mRecord = nullptr;
                        continue;
                    }
                }
                const std::string reason = isRun[k] ? "a prefix run, but " + runConflict : e.reason;
                if (mStats) mStats->notComposed.emplace_back(c, reason);
                record.outcome = RevisitRecord::Outcome::NotComposed;
                record.reason = reason;
                out.push_back(c);
            }
            if (mRecord) mStats->records.push_back(std::move(record));
            mRecord = nullptr;
        }
        start = end;
    }
    return out;
}

}

//  The prefix runs with the same set P form a family, whose texts S, of the
//  same length, may follow each other in a run.  The sets P of two families
//  must be disjoint, and contain no character of a set of S.  Within a family,
//  no two matches of its texts S may overlap (other than a match with itself):
//  a run is then a sequence of matches, each beginning where the one before
//  ends, as ICU applies them.
std::string prefixRunFamilies(const std::vector<PrefixRun> & runs) {
    UCD::UnicodeSet first, run;
    for (size_t i = 0; i < runs.size(); ++i) {
        const PrefixRun & a = runs[i];
        for (size_t j = 0; j < i; ++j) {
            const PrefixRun & b = runs[j];
            if (!a.first.intersects(b.first)) continue;
            if (!(a.first == b.first) || a.run.size() != b.run.size()) {
                return "the sets of the characters to revisit of two prefix runs intersect";
            }
        }
        first = first + a.first;
        for (const UCD::UnicodeSet & s : a.run) run = run + s;
    }
    if (first.intersects(run)) return "a character to revisit may be repeated";
    for (const PrefixRun & a : runs) {
        for (const PrefixRun & b : runs) {
            if (!(a.first == b.first)) continue;
            const size_t m = a.run.size();
            //  A match of S of a followed, d characters on, by one of S of b.
            for (size_t d = &a == &b ? 1 : 0; d < m; ++d) {
                bool overlap = true;
                for (size_t i = 0; i + d < m && overlap; ++i) overlap = a.run[d + i].intersects(b.run[i]);
                if (overlap) return "the texts of two prefix runs (or of one) may overlap";
            }
        }
    }
    return "";
}

bool prefixRun(const ConversionRule * c, PrefixRun & pr) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->hasBeforeContext() || source->hasAfterContext()) return false;
    if (!result->hasCursor() || result->getCursorOffset() != 0 || result->getResultToRevisit() == nullptr) return false;
    std::vector<RE *> text, revisit;
    textAtoms(source->getText(), text);
    textAtoms(result->getResultToRevisit(), revisit);
    if (text.size() < 2 || revisit.size() != 1) return false;
    CharSetAnalysis sets;
    auto setOf = [&sets](RE * re, UCD::UnicodeSet & s) {
        if (!CharSetAnalysis::isSet(re) || hasStrings(re) || hasBoundary(re)) return false;
        s = sets.setOf(re, false);
        return !s.empty();
    };
    const Capture * const capture = dyn_cast<Capture>(text[0]);
    std::vector<RE *> captured;
    if (capture) textAtoms(capture->getCapturedRE(), captured); else captured.push_back(text[0]);
    if (captured.size() != 1 || !setOf(captured[0], pr.first)) return false;
    pr.run.assign(text.size() - 1, UCD::UnicodeSet());
    for (size_t i = 1; i < text.size(); ++i) {
        if (!setOf(text[i], pr.run[i - 1])) return false;
    }
    if (const Reference * ref = dyn_cast<Reference>(revisit[0])) {
        if (capture == nullptr || ref->getName() != capture->getName()) return false;
        pr.revisitsReplacement = true;
    } else {
        UCD::UnicodeSet s;
        if (capture || !setOf(revisit[0], s) || s.count() != 1 || !(s == pr.first)) return false;
        pr.revisitsReplacement = false;
    }
    pr.replacement.clear();
    if (!resultString(result->getCompletedResult(), {}, pr.replacement)) return false;
    return true;
}

bool chainRevisit(const ConversionRule * c, ChainRevisit & ch) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->hasBeforeContext() || source->hasAfterContext()) return false;
    if (!result->hasCursor() || result->getCursorOffset() != 0 || result->getResultToRevisit() == nullptr) return false;
    std::u32string completed;
    if (!resultString(result->getCompletedResult(), {}, completed) || !completed.empty()) return false;
    std::vector<RE *> text, revisit, captured;
    textAtoms(source->getText(), text);
    textAtoms(result->getResultToRevisit(), revisit);
    if (text.size() != 2 || revisit.size() != 2) return false;
    const Capture * cap = dyn_cast<Capture>(text[0]);
    const Reference * ref = dyn_cast<Reference>(revisit[0]);
    if (cap == nullptr || ref == nullptr || ref->getName() != cap->getName()) return false;
    CharSetAnalysis sets;
    auto set = [&](RE * e, UCD::UnicodeSet & s) {
        if (!CharSetAnalysis::isSet(e) || hasStrings(e) || hasBoundary(e)) return false;
        s = sets.setOf(e, false);
        return !s.empty();
    };
    UCD::UnicodeSet y;
    if (!set(text[1], ch.last) || !set(revisit[1], y) || y.count() != 1) return false;
    ch.replacement = static_cast<char32_t>(lo_codepoint(*y.begin()));
    textAtoms(cap->getCapturedRE(), captured);
    if (captured.size() < 2) return false;
    const Rep * rep = dyn_cast<Rep>(captured.back());
    if (rep == nullptr || rep->getLB() != 0 || rep->getUB() != Rep::UNBOUNDED_REP || !set(rep->getRE(), ch.star)) return false;
    if (!set(captured.front(), ch.first)) return false;
    ch.fixed.clear();
    bool fixedInStar = true;
    for (size_t k = 1; k + 1 < captured.size(); ++k) {
        UCD::UnicodeSet m;
        if (!set(captured[k], m) || m.intersects(ch.last) || m.contains(ch.replacement)) return false;
        fixedInStar &= m.subset(ch.star);
        ch.fixed.push_back(m);
    }
    if (!ch.star.contains(ch.replacement) || ch.star.intersects(ch.last) || ch.first.contains(ch.replacement)) return false;
    return !ch.first.intersects(ch.last) || fixedInStar;
}

//  The sets of a sequence of sets (through variables and captures).
static bool plainItems(RE * re, CharSetAnalysis & sets, std::vector<UCD::UnicodeSet> & items) {
    if (CharSetAnalysis::isSet(re)) {
        if (hasStrings(re) || hasBoundary(re)) return false;
        items.push_back(sets.setOf(re, false));
        return !items.back().empty();
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) {
            if (!plainItems(e, sets, items)) return false;
        }
        return true;
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        return plainItems(alt->front(), sets, items);
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return plainItems(c->getCapturedRE(), sets, items);
    } else if (Name * name = dyn_cast<Name>(re)) {
        return !isFunctionCall(name) && name->getDefinition() && plainItems(name->getDefinition(), sets, items);
    }
    return false;
}

bool markRunEdit(const ConversionRule * c, MarkRunEdit & e) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->hasBeforeContext() || source->hasAfterContext()) return false;
    if (!result->hasCursor() || result->getCursorOffset() != 0 || result->getResultToRevisit() == nullptr) return false;
    e.replacement.clear();
    if (!resultString(result->getCompletedResult(), {}, e.replacement)) return false;
    std::vector<RE *> text, revisit, captured;
    textAtoms(source->getText(), text);
    textAtoms(result->getResultToRevisit(), revisit);
    if (revisit.size() != 1) return false;
    const Reference * ref = dyn_cast<Reference>(revisit[0]);
    if (ref == nullptr) return false;
    size_t k = 0;
    while (k < text.size() && !isa<Capture>(text[k])) ++k;
    if (k == 0 || k + 1 >= text.size()) return false;
    const Capture * cap = cast<Capture>(text[k]);
    if (cap->getName() != ref->getName()) return false;
    CharSetAnalysis sets;
    e.prefix.clear();
    e.suffix.clear();
    for (size_t j = 0; j < k; ++j) {
        if (!plainItems(text[j], sets, e.prefix)) return false;
    }
    for (size_t j = k + 1; j < text.size(); ++j) {
        if (isa<Capture>(text[j]) || !plainItems(text[j], sets, e.suffix)) return false;
    }
    textAtoms(cap->getCapturedRE(), captured);
    if (captured.size() != 1 && captured.size() != 2) return false;
    const Rep * rep = dyn_cast<Rep>(captured.back());
    if (rep == nullptr || rep->getLB() > (captured.size() == 1 ? 1 : 0) || rep->getUB() != Rep::UNBOUNDED_REP) return false;
    std::vector<UCD::UnicodeSet> run, firstRun;
    if (!plainItems(rep->getRE(), sets, run) || run.size() != 1) return false;
    e.run = run[0];
    e.nonEmpty = rep->getLB() == 1 || captured.size() == 2;
    e.firstRun = e.run;
    if (captured.size() == 2) {
        //  F R*, as the rules are made exclusive (F within R).
        if (!plainItems(captured[0], sets, firstRun) || firstRun.size() != 1 || !firstRun[0].subset(e.run)) return false;
        e.firstRun = firstRun[0];
    }
    if (e.replacement.size() > e.prefix.size()) return false;
    if (e.prefix.front().intersects(e.run) || e.suffix.front().intersects(e.run)) return false;
    for (size_t j = 1; j < e.prefix.size(); ++j) {
        if (!e.prefix[j].subset(e.run)) return false;
    }
    for (const UCD::UnicodeSet & t : e.suffix) {
        if (e.prefix.front().intersects(t)) return false;
    }
    return true;
}

bool keptText(const ConversionRule * c, std::vector<UCD::UnicodeSet> & items) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->hasBeforeContext() || result->hasCursor()) return false;
    std::vector<RE *> text, res;
    textAtoms(source->getText(), text);
    textAtoms(result->getText(), res);
    if (text.size() != 1 || res.size() != 1) return false;
    const Capture * cap = dyn_cast<Capture>(text[0]);
    const Reference * ref = dyn_cast<Reference>(res[0]);
    if (cap == nullptr || ref == nullptr || ref->getName() != cap->getName()) return false;
    CharSetAnalysis sets;
    items.clear();
    if (!plainItems(cap->getCapturedRE(), sets, items)) return false;
    return !source->hasAfterContext() || plainItems(source->getAfterContext(), sets, items);
}

MarkRunGroups SetAsideMarkRunGroups(std::vector<Rule *> & rules) {
    MarkRunGroups groups;
    std::vector<Rule *> out;
    size_t transforms = 0;
    for (size_t start = 0; start < rules.size(); ) {
        if (!isa<ConversionRule>(rules[start])) {
            if (isa<TransformRule>(rules[start])) transforms++;
            out.push_back(rules[start++]);
            continue;
        }
        size_t end = start;
        bool all = true, edit = false;
        for (; end < rules.size() && isa<ConversionRule>(rules[end]); ++end) {
            const ConversionRule * c = cast<ConversionRule>(rules[end]);
            MarkRunEdit e;
            std::vector<UCD::UnicodeSet> items;
            if (markRunEdit(c, e)) {
                edit = true;
            } else {
                all &= keptText(c, items);
            }
        }
        if (all && edit) {
            groups.emplace_back(transforms, std::vector<Rule *>(rules.begin() + start, rules.begin() + end));
        } else {
            out.insert(out.end(), rules.begin() + start, rules.begin() + end);
        }
        start = end;
    }
    rules = std::move(out);
    return groups;
}

void RestoreMarkRunGroups(std::vector<Rule *> & rules, const MarkRunGroups & groups) {
    std::vector<Rule *> out;
    size_t transforms = 0, g = 0;
    auto insert = [&]() {
        while (g < groups.size() && groups[g].first == transforms) {
            out.insert(out.end(), groups[g].second.begin(), groups[g].second.end());
            g++;
        }
    };
    size_t k = 0;
    //  Before the first transform rule: after the filter rules.
    for (; k < rules.size() && isa<FilterRule>(rules[k]); ++k) out.push_back(rules[k]);
    insert();
    for (; k < rules.size(); ++k) {
        out.push_back(rules[k]);
        if (isa<TransformRule>(rules[k])) {
            transforms++;
            insert();
        }
    }
    insert();
    rules = std::move(out);
}

std::vector<Rule *> ComposeRevisits(const std::vector<Rule *> & rules, RevisitStats * stats) {
    return Composer(stats).compose(rules);
}

}

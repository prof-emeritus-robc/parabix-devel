/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Composition of the rules whose results have text to revisit with the rules
//  applying to that text (see ComposeRevisits in transform_rules.h).

#include <ldml/transform_rules.h>
#include <map>
#include <set>
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
//  boundary, or a (possessive) repetition of such a set.
struct Item {
    UCD::UnicodeSet chars;
    bool boundary = false;
    bool star = false;
    bool operator==(const Item & other) const {
        return chars == other.chars && boundary == other.boundary && star == other.star;
    }
};

//  A context: absent or empty (no condition), a sequence of items, or
//  (if not simple) another pattern, which can only be kept unchanged.
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
        if (!i.star) return false;
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
        if (x.star || y.star) return false;
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

//  Both contexts: before contexts are aligned at the end, after contexts at
//  the start.  Fail if they cannot both match.
Match conjoin(Context & a, const Context & b, bool front) {
    if (b.empty()) return Match::Ok;
    if (a.empty()) {
        a = b;
        return Match::Ok;
    }
    if (!a.simple || !b.simple) {
        if (a.re && a.re == b.re) return Match::Ok;
        throw NotComposed{"two contexts that are not sequences of sets apply"};
    }
    bool star = false;
    for (const Item & i : a.items) star |= i.star;
    for (const Item & i : b.items) star |= i.star;
    if (star) {
        if (a.items == b.items) return Match::Ok;
        throw NotComposed{"two contexts with repetitions apply"};
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
    std::vector<Rule *> composeRule(ConversionRule * r, const std::vector<GroupRule> & group);
    void explore(Path p, RE * text, std::vector<Rule *> & composed);
    void apply(const GroupRule & g, Path q, const std::vector<MatchedChar> & matched,
               std::vector<int> referenced, RE * text, std::vector<Rule *> & composed);
    void finish(Path p, RE * text, std::vector<Rule *> & composed);
    Rule * suffixRevisit(ConversionRule * r);
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
    } else if (Name * name = dyn_cast<Name>(re)) {
        return !isFunctionCall(name) && name->getDefinition() && collectItems(name->getDefinition(), items);
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        std::vector<Item> body;
        if (!collectItems(rep->getRE(), body) || body.size() != 1) return false;
        const int lb = rep->getLB();
        const int ub = rep->getUB();
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

GroupRule Composer::groupRule(ConversionRule * r) {
    GroupRule g;
    g.rule = r;
    const RuleSide * source = r->getSourceSide(Direction::Forward);
    textItems(source->getText(), g);
    if (g.text.empty()) g.fixed = false;
    g.before = context(source->getBeforeContext());
    g.after = context(source->getAfterContext());
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

std::vector<Rule *> Composer::compose(const std::vector<Rule *> & rules) {
    std::vector<Rule *> out;
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

std::vector<Rule *> ComposeRevisits(const std::vector<Rule *> & rules, RevisitStats * stats) {
    return Composer(stats).compose(rules);
}

}

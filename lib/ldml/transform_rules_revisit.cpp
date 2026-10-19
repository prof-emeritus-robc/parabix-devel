/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Composition of the rules whose results have text to revisit with the rules
//  applying to that text (see ComposeRevisits in transform_rules.h).

#include <ldml/transform_rules.h>
#include <map>
#include <set>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include "charset_analysis.h"

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
            throw NotComposed{"a rule whose text is not a fixed-length sequence of characters may apply "
                              "to the text to revisit: " + printRule(g.rule)};
        }
        if (q.beyond.size() > MaxExtension) {
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
                }
                isRewritten[&r - rewritten.data()] = true;
                r = s;
            }
        }
        std::vector<GroupRule> group;
        for (Rule * r : rewritten) {
            if (ConversionRule * c = dyn_cast<ConversionRule>(r)) group.push_back(groupRule(c));
        }
        for (size_t k = 0; k < rewritten.size(); ++k) {
            ConversionRule * c = dyn_cast<ConversionRule>(rewritten[k]);
            if (c == nullptr || isRewritten[k] || !hasRevisit(c)) {
                out.push_back(rewritten[k]);
                continue;
            }
            try {
                std::vector<Rule *> composed = composeRule(c, group);
                if (mStats) {
                    mStats->composed++;
                    mStats->composedRules += composed.size();
                }
                out.insert(out.end(), composed.begin(), composed.end());
            } catch (const NotComposed & e) {
                if (mStats) mStats->notComposed.emplace_back(c, e.reason);
                out.push_back(c);
            }
        }
        start = end;
    }
    return out;
}

}

std::vector<Rule *> ComposeRevisits(const std::vector<Rule *> & rules, RevisitStats * stats) {
    return Composer(stats).compose(rules);
}

}

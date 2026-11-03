/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Expansion of texts to replace of finitely many lengths into fixed-length
//  texts (see ExpandFiniteTexts in transform_rules.h).

#include <ldml/transform_rules.h>
#include <algorithm>
#include <map>
#include <set>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include "charset_analysis.h"

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

constexpr size_t MaxVariants = 64;

struct NotExpanded {
    std::string reason;
};

//  A partial expansion of a text: the sets of its items so far, and what may
//  not follow (as ICU matches possessively and
//  takes the longest string of a set): a character of exclude, or a string of
//  excludeStrings (of two or more characters).
struct Variant {
    std::vector<UCD::UnicodeSet> items;
    UCD::UnicodeSet exclude;
    std::set<std::u32string> excludeStrings;
};

class Expander {
public:
    std::vector<Variant> expand(RE * re, std::vector<Variant> in);
    bool isFixed(RE * re);
    bool alternatives(const RE * re, UCD::UnicodeSet & chars, std::set<std::u32string> & strings);
private:
    std::vector<Variant> append(const Variant & v, const UCD::UnicodeSet & set);
    std::vector<Variant> alternativesOf(const Variant & v, const UCD::UnicodeSet & chars, const std::set<std::u32string> & strings);
    std::vector<Variant> optional(RE * body, std::vector<Variant> in);
    bool isSetLike(const RE * re);
    CharSetAnalysis mSets;
};

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

//  The literal string of a sequence of single characters.
bool literalString(const RE * re, std::u32string & s) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        if (cc->count() != 1) return false;
        s.push_back(static_cast<char32_t>(cc->min_codepoint()));
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) {
            if (!literalString(e, s)) return false;
        }
        return true;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && literalString(n->getDefinition(), s);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        return alt->size() == 1 && literalString(alt->front(), s);
    }
    return false;
}

//  A set, possibly with strings, as its single characters and its strings
//  (of two or more characters).
bool Expander::alternatives(const RE * re, UCD::UnicodeSet & chars, std::set<std::u32string> & strings) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (isBoundary(a)) continue;    // (the text boundary, only in contexts)
            if (isa<Seq>(a)) {
                std::u32string s;
                if (!literalString(a, s)) return false;
                if (s.size() == 1) chars.insert(s[0]);
                else if (s.size() > 1) strings.insert(s);
                continue;
            }
            if (!alternatives(a, chars, strings)) return false;
        }
        return true;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && alternatives(n->getDefinition(), chars, strings);
    } else if (CharSetAnalysis::isSetExpression(re)) {
        //  Strings within differences or intersections are not supported.
        if (const Diff * d = dyn_cast<Diff>(re)) {
            UCD::UnicodeSet c;
            std::set<std::u32string> s;
            if (!alternatives(d->getLH(), c, s) || !s.empty()) return false;
        }
        chars = chars + mSets.setOf(re, false);
        return true;
    }
    return false;
}

bool Expander::isSetLike(const RE * re) {
    UCD::UnicodeSet chars;
    std::set<std::u32string> strings;
    return CharSetAnalysis::isSet(re) && alternatives(re, chars, strings);
}

//  A text that is already a fixed-length sequence of characters.
bool Expander::isFixed(RE * re) {
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) {
            if (!isFixed(e)) return false;
        }
        return true;
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return isFixed(c->getCapturedRE());
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        return isFixed(alt->front());
    } else if (Name * n = dyn_cast<Name>(re);
               n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
        return isFixed(n->getDefinition());
    }
    if (!CharSetAnalysis::isSet(re)) return false;
    UCD::UnicodeSet chars;
    std::set<std::u32string> strings;
    return alternatives(re, chars, strings) && strings.empty();
}

//  The variant followed by a character of a set: split by what may follow,
//  where the character begins a string that may not follow.
std::vector<Variant> Expander::append(const Variant & v, const UCD::UnicodeSet & set) {
    std::vector<Variant> out;
    const UCD::UnicodeSet allowed = set - v.exclude;
    if (allowed.empty()) return out;
    //  The characters beginning an excluded string, each with the rest of
    //  the strings it begins.
    std::map<char32_t, std::set<std::u32string>> rests;
    for (const std::u32string & s : v.excludeStrings) {
        if (allowed.contains(s[0])) rests[s[0]].insert(s.substr(1));
    }
    UCD::UnicodeSet free = allowed;
    for (const auto & r : rests) free = free - UCD::UnicodeSet(r.first);
    if (!free.empty()) {
        Variant w = v;
        w.items.push_back(free);
        w.exclude = UCD::UnicodeSet();
        w.excludeStrings.clear();
        out.push_back(std::move(w));
    }
    for (const auto & r : rests) {
        Variant w = v;
        w.items.push_back(UCD::UnicodeSet(r.first));
        w.exclude = UCD::UnicodeSet();
        w.excludeStrings.clear();
        for (const std::u32string & rest : r.second) {
            if (rest.size() == 1) w.exclude.insert(rest[0]);
            else w.excludeStrings.insert(rest);
        }
        out.push_back(std::move(w));
    }
    return out;
}

//  The variant followed by a set with strings: its longest string matching,
//  as ICU takes it, and none longer.
std::vector<Variant> Expander::alternativesOf(const Variant & v, const UCD::UnicodeSet & chars,
                                              const std::set<std::u32string> & strings) {
    std::vector<std::u32string> all(strings.begin(), strings.end());
    std::stable_sort(all.begin(), all.end(), [](const std::u32string & a, const std::u32string & b) {
        return a.size() > b.size();
    });
    //  The continuations of the longer strings of the set beginning with s.
    auto longer = [&](const std::u32string & s, Variant & w) {
        for (const std::u32string & t : strings) {
            if (t.size() > s.size() && t.compare(0, s.size(), s) == 0) {
                const std::u32string rest = t.substr(s.size());
                if (rest.size() == 1) w.exclude.insert(rest[0]);
                else w.excludeStrings.insert(rest);
            }
        }
    };
    std::vector<Variant> out;
    for (const std::u32string & s : all) {
        std::vector<Variant> vs{v};
        for (char32_t c : s) {
            std::vector<Variant> next;
            for (const Variant & x : vs) {
                for (Variant & y : append(x, UCD::UnicodeSet(c))) next.push_back(std::move(y));
            }
            vs = std::move(next);
        }
        for (Variant & x : vs) {
            longer(s, x);
            out.push_back(std::move(x));
        }
    }
    //  The single characters: those beginning strings of the set separately.
    UCD::UnicodeSet plain = chars;
    for (const std::u32string & t : strings) {
        if (chars.contains(t[0])) plain = plain - UCD::UnicodeSet(t[0]);
    }
    std::vector<UCD::UnicodeSet> groups;
    if (!plain.empty()) groups.push_back(plain);
    for (const auto & range : chars - plain) {
        for (codepoint_t c = range.first; c <= range.second; ++c) groups.push_back(UCD::UnicodeSet(c));
    }
    for (const UCD::UnicodeSet & g : groups) {
        for (Variant & x : append(v, g)) {
            if (g.count() == 1) longer(std::u32string(1, static_cast<char32_t>(g.front().first)), x);
            out.push_back(std::move(x));
        }
    }
    return out;
}

//  Possessive x? : x if it matches, and otherwise nothing, where the text does
//  not then continue with x.
std::vector<Variant> Expander::optional(RE * body, std::vector<Variant> in) {
    std::vector<Variant> out = expand(body, in);
    UCD::UnicodeSet chars;
    std::set<std::u32string> strings;
    std::u32string literal;
    if (isSetLike(body)) {
        alternatives(body, chars, strings);
    } else if (literalString(body, literal) && !literal.empty()) {
        if (literal.size() == 1) chars.insert(literal[0]); else strings.insert(literal);
    } else {
        throw NotExpanded{"an optional item is not a set or a string: " + printPattern(body)};
    }
    for (Variant & v : in) {
        v.exclude = v.exclude + chars;
        v.excludeStrings.insert(strings.begin(), strings.end());
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<Variant> Expander::expand(RE * re, std::vector<Variant> in) {
    if (in.empty()) return in;
    if (in.size() > MaxVariants) throw NotExpanded{"more than " + std::to_string(MaxVariants) + " alternatives"};
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) in = expand(e, std::move(in));
        return in;
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return expand(c->getCapturedRE(), std::move(in));
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        return expand(alt->front(), std::move(in));
    } else if (Name * n = dyn_cast<Name>(re);
               n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
        return expand(n->getDefinition(), std::move(in));
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        if (rep->getUB() == Rep::UNBOUNDED_REP) throw NotExpanded{"an unbounded repetition"};
        for (int k = 0; k < rep->getLB(); k++) in = expand(rep->getRE(), std::move(in));
        for (int k = rep->getLB(); k < rep->getUB(); k++) in = optional(rep->getRE(), std::move(in));
        return in;
    } else if (isSetLike(re)) {
        if (hasBoundary(re)) throw NotExpanded{"the text includes the text boundary"};
        UCD::UnicodeSet chars;
        std::set<std::u32string> strings;
        alternatives(re, chars, strings);
        std::vector<Variant> out;
        for (const Variant & v : in) {
            for (Variant & w : alternativesOf(v, chars, strings)) out.push_back(std::move(w));
        }
        return out;
    }
    throw NotExpanded{"the text has " + printPattern(re)};
}

//  An after context excluding what may not follow a text: a character of
//  exclude, or a string of strings.  As alternatives: a character other than
//  these (or the end of the text), or a character beginning only strings,
//  followed by what excludes their rest.
RE * exclusionContext(const UCD::UnicodeSet & exclude, const std::set<std::u32string> & strings) {
    std::map<char32_t, std::set<std::u32string>> rests;
    for (const std::u32string & x : strings) {
        if (!exclude.contains(x[0])) rests[x[0]].insert(x.substr(1));
    }
    UCD::UnicodeSet firsts = exclude;
    for (const auto & r : rests) firsts.insert(r.first);
    std::vector<RE *> alternatives{makeCC(AllCodepoints - firsts), makeEnd()};
    for (const auto & r : rests) {
        UCD::UnicodeSet next;
        std::set<std::u32string> longer;
        for (const std::u32string & rest : r.second) {
            if (rest.size() == 1) next.insert(rest[0]); else longer.insert(rest);
        }
        alternatives.push_back(makeSeq({makeCC(static_cast<codepoint_t>(r.first)), exclusionContext(next, longer)}));
    }
    return makeAlt(alternatives.begin(), alternatives.end());
}

//  The text of a variant (its captures, not referred to, are dropped).
RE * variantText(const Variant & v) {
    std::vector<RE *> seq;
    for (const UCD::UnicodeSet & item : v.items) seq.push_back(makeCC(item));
    return makeSeq(seq.begin(), seq.end());
}

bool referencesCaptures(const RE * re) {
    if (re == nullptr) return false;
    if (isa<Reference>(re)) return true;
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (referencesCaptures(e)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (referencesCaptures(e)) return true;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return isFunctionCall(n) && referencesCaptures(n->getDefinition());
    }
    return false;
}

}

//  The members of a set other than the end of the text (End), through its
//  alternations, differences, intersections and variables, with end set if it
//  has the end (a negated set [^X] is the difference of a set with it and X);
//  nullptr if it has no other member.
static RE * withoutEnd(RE * re, bool & end) {
    if (isa<End>(re)) {
        end = true;
        return nullptr;
    } else if (Name * n = dyn_cast<Name>(re); n && !isFunctionCall(n) && n->getDefinition()) {
        bool e = false;
        RE * const rest = withoutEnd(n->getDefinition(), e);
        if (!e) return re;
        end = true;
        return rest;
    } else if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> rest;
        bool e = false;
        for (RE * a : *alt) {
            RE * const x = withoutEnd(a, e);
            if (x && !(isa<CC>(x) && cast<CC>(x)->empty())) rest.push_back(x);
        }
        if (!e) return re;
        end = true;
        return rest.empty() ? nullptr : makeAlt(rest.begin(), rest.end());
    } else if (Diff * d = dyn_cast<Diff>(re)) {
        bool l = false, r = false;
        RE * const lh = withoutEnd(d->getLH(), l);
        RE * const rh = withoutEnd(d->getRH(), r);
        if (!l || r) return re;
        end = true;
        if (lh == nullptr) return nullptr;
        return rh ? makeDiff(lh, rh) : lh;
    } else if (Intersect * x = dyn_cast<Intersect>(re)) {
        bool l = false, r = false;
        RE * const lh = withoutEnd(x->getLH(), l);
        RE * const rh = withoutEnd(x->getRH(), r);
        if (!l || !r) return re;
        end = true;
        if (lh == nullptr || rh == nullptr) return nullptr;
        return makeIntersect(lh, rh);
    }
    return re;
}

//  Whether a pattern (of a result) refers to a capture, anywhere in it
//  (including the arguments of function calls).
static bool refersTo(const RE * re, const Capture * capture) {
    if (const Reference * r = dyn_cast<Reference>(re)) {
        return r->getCapture() == capture;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (refersTo(e, capture)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (refersTo(e, capture)) return true;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return refersTo(rep->getRE(), capture);
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() && refersTo(n->getDefinition(), capture);
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return refersTo(c->getCapturedRE(), capture);
    }
    return false;
}

//  A part of a result with the references to a capture (items of the part)
//  referring to another (by), or, with by nullptr, removed; false if a
//  reference to it is not such an item (within a function call).
static bool rebind(RE * part, const Capture * capture, Capture * by, RE *& out) {
    if (part == nullptr) {
        out = nullptr;
        return true;
    }
    std::vector<RE *> items;
    if (Seq * seq = dyn_cast<Seq>(part)) items.assign(seq->begin(), seq->end()); else items.push_back(part);
    std::vector<RE *> rebound;
    for (RE * item : items) {
        if (const Reference * r = dyn_cast<Reference>(item); r && r->getCapture() == capture) {
            if (by) rebound.push_back(Reference::Create(r->getName(), by, r->getInstance(), r->getReferencedProperty()));
        } else if (refersTo(item, capture)) {
            return false;
        } else {
            rebound.push_back(item);
        }
    }
    out = makeSeq(rebound.begin(), rebound.end());
    return true;
}

//  The result side of a rule with the references to a capture rebound (see
//  rebind), or nullptr.
static RuleSide * rebindResult(const RuleSide * result, const Capture * capture, Capture * by) {
    RE * completed = nullptr, * revisit = nullptr;
    if (!rebind(result->getCompletedResult(), capture, by, completed) || !rebind(result->getResultToRevisit(), capture, by, revisit)) {
        return nullptr;
    }
    return RuleSide::Create(result->getBeforeContext(), completed, result->hasCursor(), revisit, result->getCursorOffset(),
                            result->getAfterContext());
}

std::vector<Rule *> MoveTextAnchors(const std::vector<Rule *> & rules) {
    //  With a global filter and transform rules, ICU applies each rule set to
    //  each run of the characters of the filter (in the text given, which the
    //  rule sets before change): a set of the text that includes the text
    //  boundary also matches at the end of a run, which the text of a step does
    //  not tell (the characters a step before inserts are within the run of the
    //  character they follow), but the filter's mask does: the end of the text
    //  is then also a position outside the filter (FilterRunEndMark).
    //  (The characters of the set are then those of the filter, as the text of
    //  a rule matches within a run.)
    const FilterRule * filter = nullptr;
    bool transforms = false;
    for (const Rule * r : rules) {
        if (const FilterRule * f = dyn_cast<FilterRule>(r); f && !f->isInverse() && filter == nullptr) filter = f;
        transforms |= isa<TransformRule>(r);
    }
    const bool runs = filter && transforms;
    RE * const end = runs ? static_cast<RE *>(Alt::Create({makeCC(FilterRunEndMark), makeEnd()})) : makeEnd();
    CharSetAnalysis sets;
    static const UCD::UnicodeSet nonCharacters = UCD::UnicodeSet(0xFDD0, 0xFDEF);
    const UCD::UnicodeSet filterChars = runs ? sets.setOf(filter->getFilterSet(), false) - nonCharacters : UCD::UnicodeSet();
    auto withinFilter = [&](RE * set) -> RE * {
        if (!runs || set == nullptr) return set;
        const UCD::UnicodeSet chars = sets.setOf(set, false) & filterChars;
        return chars.empty() ? nullptr : makeCC(chars);
    };
    std::vector<Rule *> out;
    for (Rule * r : rules) {
        ConversionRule * c = dyn_cast<ConversionRule>(r);
        const RuleSide * source = c ? c->getSourceSide(Direction::Forward) : nullptr;
        if (source == nullptr || source->getText() == nullptr) {
            out.push_back(r);
            continue;
        }
        RE * const text = source->getText();
        std::vector<RE *> items;
        if (Seq * seq = dyn_cast<Seq>(text)) items.assign(seq->begin(), seq->end()); else items.push_back(text);
        RE * before = source->getBeforeContext();
        RE * const after = source->getAfterContext();
        RuleSide * const result = c->getResultSide(Direction::Forward);
        auto rule = [&](RE * b, const std::vector<RE *> & t, RE * a, RuleSide * r = nullptr) {
            return makeConversionRule(makeRuleSide(b, makeSeq(t.begin(), t.end()), a), Direction::Forward, r ? r : result);
        };
        bool changed = false;
        //  ^ at the start of the text: its before context.
        if (before == nullptr && items.size() > 1 && isa<Start>(items.front())) {
            before = makeStart();
            items.erase(items.begin());
            changed = true;
        }
        if (after == nullptr && items.size() > 1) {
            //  $ at the end of the text: its after context.
            if (isa<End>(items.back())) {
                items.pop_back();
                out.push_back(rule(before, items, makeEnd()));
                continue;
            }
            //  A last capture of such a set: of a character of the set, or
            //  (empty, its references removed) the end of the text.  Revisited
            //  unchanged (the last of the text to revisit, its only reference),
            //  it is its after context, the text then revisited from the same
            //  position (^ 4 ($nd) → | X $1 as ^ 4 } $nd → | X).
            if (Capture * const capture = dyn_cast<Capture>(items.back())) {
                bool boundary = false;
                RE * const chars = withinFilter(withoutEnd(capture->getCapturedRE(), boundary));
                RE * const revisit = result->getResultToRevisit();
                std::vector<RE *> revisited;
                if (revisit) {
                    if (Seq * seq = dyn_cast<Seq>(revisit)) revisited.assign(seq->begin(), seq->end()); else revisited.push_back(revisit);
                }
                const Reference * const last = revisited.empty() ? nullptr : dyn_cast<Reference>(revisited.back());
                if (boundary && result->hasCursor() && result->getCursorOffset() == 0 && last && last->getCapture() == capture
                        && !(result->getCompletedResult() && refersTo(result->getCompletedResult(), capture))) {
                    revisited.pop_back();
                    bool once = true;
                    for (const RE * e : revisited) once &= !refersTo(e, capture);
                    if (once) {
                        RuleSide * const context = RuleSide::Create(result->getBeforeContext(), result->getCompletedResult(), true,
                                                                    makeSeq(revisited.begin(), revisited.end()), 0,
                                                                    result->getAfterContext());
                        items.pop_back();
                        //  (Its end of the text also the end of a run, see FilterRunEndMark.)
                        RE * after = capture->getCapturedRE();
                        if (end != nullptr && !isa<End>(end)) {
                            std::vector<RE *> alts;
                            if (chars) alts.push_back(chars);
                            alts.push_back(end);
                            after = makeAlt(alts.begin(), alts.end());
                        }
                        out.push_back(rule(before, items, after, context));
                        continue;
                    }
                }
                if (boundary) {
                    Capture * const withChar = chars ? makeCapture(capture->getName(), chars) : nullptr;
                    RuleSide * const charResult = withChar ? rebindResult(result, capture, withChar) : nullptr;
                    RuleSide * const endResult = rebindResult(result, capture, nullptr);
                    if ((withChar == nullptr || charResult) && endResult) {
                        if (withChar) {
                            std::vector<RE *> text(items);
                            text.back() = withChar;
                            out.push_back(rule(before, text, nullptr, charResult));
                        }
                        items.pop_back();
                        out.push_back(rule(before, items, end, endResult));
                        continue;
                    }
                }
            }
            //  A last set including the end of the text: a character of the
            //  set, or (matching nothing) the end of the text.
            bool boundary = false;
            RE * const chars = withinFilter(withoutEnd(items.back(), boundary));
            if (boundary) {
                if (chars) {
                    std::vector<RE *> withChar(items);
                    withChar.back() = chars;
                    out.push_back(rule(before, withChar, nullptr));
                }
                items.pop_back();
                out.push_back(rule(before, items, end));
                continue;
            }
        }
        out.push_back(changed ? rule(before, items, after) : r);
    }
    return out;
}

//  Whether a set has only characters (no strings), through its alternations,
//  differences, intersections and variables.
static bool charactersOnly(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && charactersOnly(n->getDefinition());
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) if (!charactersOnly(a)) return false;
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        return seq->size() == 1 && charactersOnly(seq->front());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        return charactersOnly(d->getLH());
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        return charactersOnly(x->getLH()) || charactersOnly(x->getRH());
    }
    return !isBoundary(re);
}

//  The references to a capture in a pattern of a result (anywhere), counted;
//  false where one is not an item of the pattern (within a function call).
static bool countReferences(const RE * re, const Capture * capture, unsigned & count, bool top = true) {
    if (const Reference * r = dyn_cast<Reference>(re)) {
        if (r->getCapture() == capture) {
            if (!top) return false;
            count++;
        }
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (!countReferences(e, capture, count, top)) return false;
        return true;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() == nullptr || countReferences(n->getDefinition(), capture, count, false);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (!countReferences(e, capture, count, false)) return false;
        return true;
    }
    return true;
}

//  A part of a result with the references to the captures given replaced by
//  their characters.
static RE * replaceReferences(RE * part, const std::map<const Capture *, UCD::codepoint_t> & chars) {
    if (part == nullptr) return nullptr;
    std::vector<RE *> items;
    if (Seq * seq = dyn_cast<Seq>(part)) items.assign(seq->begin(), seq->end()); else items.push_back(part);
    for (RE *& item : items) {
        if (const Reference * r = dyn_cast<Reference>(item)) {
            const auto f = chars.find(r->getCapture());
            if (f != chars.end()) item = makeCC(f->second);
        }
    }
    return makeSeq(items.begin(), items.end());
}

std::vector<Rule *> EnumerateRepeatedCaptures(const std::vector<Rule *> & rules) {
    constexpr size_t MaxRules = 64;
    std::vector<Rule *> out;
    CharSetAnalysis sets;
    for (Rule * r : rules) {
        ConversionRule * c = dyn_cast<ConversionRule>(r);
        const RuleSide * source = c ? c->getSourceSide(Direction::Forward) : nullptr;
        if (source == nullptr || source->getText() == nullptr) {
            out.push_back(r);
            continue;
        }
        const RuleSide * result = c->getResultSide(Direction::Forward);
        std::vector<RE *> items;
        if (Seq * seq = dyn_cast<Seq>(source->getText())) items.assign(seq->begin(), seq->end()); else items.push_back(source->getText());
        //  The captures of single characters of the text referred to more than
        //  once (the references items of the result).
        std::vector<std::pair<size_t, UCD::UnicodeSet>> enumerated;
        bool repeated = false, ok = true;
        size_t combinations = 1;
        for (size_t i = 0; i < items.size() && ok; ++i) {
            const Capture * cap = dyn_cast<Capture>(items[i]);
            if (cap == nullptr || !CharSetAnalysis::isSet(cap->getCapturedRE()) || !charactersOnly(cap->getCapturedRE())) continue;
            unsigned count = 0;
            ok = countReferences(result->getCompletedResult() ? result->getCompletedResult() : makeSeq(), cap, count)
                 && (result->getResultToRevisit() == nullptr || countReferences(result->getResultToRevisit(), cap, count));
            if (!ok || count == 0) continue;
            const UCD::UnicodeSet chars = sets.setOf(cap->getCapturedRE(), false);
            if (chars.empty()) {
                ok = false;
                break;
            }
            combinations *= chars.count();
            repeated |= count > 1;
            enumerated.emplace_back(i, chars);
        }
        if (!ok || !repeated || combinations > MaxRules) {
            out.push_back(r);
            continue;
        }
        //  Each combination of the characters of the captures, in order.
        std::vector<std::map<const Capture *, UCD::codepoint_t>> combos(1);
        for (const auto & e : enumerated) {
            std::vector<std::map<const Capture *, UCD::codepoint_t>> next;
            for (const auto & m : combos) {
                for (const auto & range : e.second) {
                    for (UCD::codepoint_t ch = lo_codepoint(range); ch <= hi_codepoint(range); ++ch) {
                        auto n = m;
                        n[cast<Capture>(items[e.first])] = ch;
                        next.push_back(std::move(n));
                    }
                }
            }
            combos = std::move(next);
        }
        for (const auto & m : combos) {
            std::vector<RE *> text(items);
            for (const auto & e : enumerated) text[e.first] = makeCC(m.at(cast<Capture>(items[e.first])));
            RuleSide * const side = makeRuleSide(source->getBeforeContext(), makeSeq(text.begin(), text.end()), source->getAfterContext());
            RuleSide * const res = RuleSide::Create(result->getBeforeContext(), replaceReferences(result->getCompletedResult(), m),
                                                    result->hasCursor(), replaceReferences(result->getResultToRevisit(), m),
                                                    result->getCursorOffset(), result->getAfterContext());
            out.push_back(makeConversionRule(side, Direction::Forward, res));
        }
    }
    return out;
}

std::vector<Rule *> ExpandFiniteTexts(const std::vector<Rule *> & rules, FiniteTextStats * stats) {
    std::vector<Rule *> out;
    Expander expander;
    CharSetAnalysis sets;
    for (Rule * r : rules) {
        ConversionRule * c = dyn_cast<ConversionRule>(r);
        if (c == nullptr) {
            out.push_back(r);
            continue;
        }
        const RuleSide * source = c->getSourceSide(Direction::Forward);
        RE * const text = source->getText();
        if (text == nullptr || expander.isFixed(text)) {
            out.push_back(r);
            continue;
        }
        if (stats) stats->candidates++;
        try {
            const RuleSide * const result = c->getResultSide(Direction::Forward);
            if (referencesCaptures(result->getCompletedResult()) || referencesCaptures(result->getResultToRevisit())) {
                throw NotExpanded{"the result refers to captures"};
            }
            const std::vector<Variant> variants = expander.expand(text, {Variant{}});
            std::vector<Rule *> expanded;
            for (const Variant & v : variants) {
                if (v.items.empty()) throw NotExpanded{"an alternative matches the empty text"};
                //  (A string beginning with an excluded character is excluded with it.)
                bool strings = false;
                for (const std::u32string & x : v.excludeStrings) strings |= !v.exclude.contains(x[0]);
                RE * after = source->getAfterContext();
                if (strings) {
                    if (after) throw NotExpanded{"a string may not follow the text, which has an after context"};
                    after = exclusionContext(v.exclude, v.excludeStrings);
                } else if (!v.exclude.empty()) {
                    //  The character after the text may not be one of exclude.
                    if (after == nullptr) {
                        after = Alt::Create({makeCC(AllCodepoints - v.exclude), makeEnd()});
                    } else {
                        std::vector<RE *> items;
                        if (Seq * seq = dyn_cast<Seq>(after)) items.assign(seq->begin(), seq->end()); else items.push_back(after);
                        RE * const first = items.front();
                        UCD::UnicodeSet chars;
                        std::set<std::u32string> strings;
                        if (!CharSetAnalysis::isSet(first) || !expander.alternatives(first, chars, strings) || !strings.empty()) {
                            throw NotExpanded{"the after context does not begin with a set"};
                        }
                        RE * cc = makeCC(chars - v.exclude);
                        items.front() = hasBoundary(first) ? Alt::Create({cc, makeEnd()}) : cc;
                        after = makeSeq(items.begin(), items.end());
                    }
                }
                RuleSide * const side = makeRuleSide(source->getBeforeContext(), variantText(v), after);
                expanded.push_back(makeConversionRule(side, Direction::Forward, c->getResultSide(Direction::Forward)));
            }
            if (stats) {
                stats->expanded++;
                stats->rules += expanded.size();
            }
            out.insert(out.end(), expanded.begin(), expanded.end());
        } catch (const NotExpanded & e) {
            if (stats) stats->notExpanded.emplace_back(c, e.reason);
            out.push_back(r);
        }
    }
    return out;
}


namespace {

//  The characters with which a match of re may begin, and whether it may be
//  empty (as Composer::firstChars), or false.
bool textFirsts(RE * re, CharSetAnalysis & sets, UCD::UnicodeSet & first, bool & nullable) {
    if (CharSetAnalysis::isSet(re)) {
        first = first + sets.setOf(re, true);
        nullable = false;
        return true;
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        nullable = true;
        for (RE * e : *seq) {
            bool n = false;
            if (!textFirsts(e, sets, first, n)) return false;
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
            if (!textFirsts(e, sets, first, n)) return false;
            nullable |= n;
        }
        return true;
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        if (!textFirsts(rep->getRE(), sets, first, nullable)) return false;
        nullable |= rep->getLB() == 0;
        return true;
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return textFirsts(c->getCapturedRE(), sets, first, nullable);
    } else if (Name * name = dyn_cast<Name>(re)) {
        return !isFunctionCall(name) && name->getDefinition() && textFirsts(name->getDefinition(), sets, first, nullable);
    } else if (isBoundary(re) || isa<Start>(re) || isa<End>(re)) {
        nullable = true;
        return true;
    }
    return false;
}

}

//  A rule's text beginning with a run of at least n characters of a set R,
//  R{k} R{lb,} T (n = k + lb), T not empty (see SplitLeadingRuns).
struct LeadingRun {
    RE * R = nullptr;
    UCD::UnicodeSet chars;
    int n = 0;
    RE * rest = nullptr;
    UCD::UnicodeSet restFirst;
};

static bool leadingRun(const ConversionRule * c, Expander & expander, CharSetAnalysis & sets, LeadingRun & run) {
    const RuleSide * const source = c->getSourceSide(Direction::Forward);
    if (source->hasBeforeContext() || c->getResultSide(Direction::Forward)->hasCursor()) return false;
    RE * const text = source->getText();
    const Seq * const seq = text ? dyn_cast<Seq>(text) : nullptr;
    if (seq == nullptr || seq->size() < 2) return false;
    std::vector<RE *> items(seq->begin(), seq->end());
    size_t at = 0;
    while (at < items.size() && !isa<Rep>(items[at])) at++;
    if (at + 1 >= items.size()) return false;
    const Rep * const rep = cast<Rep>(items[at]);
    if (rep->getUB() != Rep::UNBOUNDED_REP || !CharSetAnalysis::isSet(rep->getRE())) return false;
    std::set<std::u32string> strings;
    if (!expander.alternatives(rep->getRE(), run.chars, strings) || !strings.empty() || run.chars.empty()) return false;
    for (size_t j = 0; j < at; j++) {
        UCD::UnicodeSet chars;
        std::set<std::u32string> st;
        if (!CharSetAnalysis::isSet(items[j]) || !expander.alternatives(items[j], chars, st) || !st.empty() || !(chars == run.chars)) {
            return false;
        }
    }
    run.R = rep->getRE();
    run.n = static_cast<int>(at) + rep->getLB();
    run.rest = makeSeq(items.begin() + at + 1, items.end());
    bool nullable = false;
    return textFirsts(run.rest, sets, run.restFirst, nullable) && !nullable && !run.restFirst.intersects(run.chars);
}

//  The RE without its captures (as a context).
static RE * withoutCaptures(RE * re) {
    if (Capture * c = dyn_cast<Capture>(re)) return withoutCaptures(c->getCapturedRE());
    if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> items;
        for (RE * e : *seq) items.push_back(withoutCaptures(e));
        return makeSeq(items.begin(), items.end());
    }
    if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> items;
        for (RE * e : *alt) items.push_back(withoutCaptures(e));
        return makeAlt(items.begin(), items.end());
    }
    if (Rep * rep = dyn_cast<Rep>(re)) return makeRep(withoutCaptures(rep->getRE()), rep->getLB(), rep->getUB());
    return re;
}

std::vector<Rule *> SplitLeadingRuns(const std::vector<Rule *> & rules, unsigned * split) {
    std::vector<Rule *> out;
    Expander expander;
    CharSetAnalysis sets;
    //  The conversion rules of the group so far.
    std::vector<const ConversionRule *> earlier;
    //  Where an earlier rule may match within a run of R followed by a text
    //  beginning with a character of next (its first item then a set meeting
    //  R): at a character of the run followed by another (Within), or by the
    //  text (AtLast: a character after it may be of next); or, with n = 0, at a
    //  character of next (Within).
    enum class Match {None, AtLast, Within};
    auto mayMatch = [&](const ConversionRule * e, const UCD::UnicodeSet & R, const UCD::UnicodeSet & next, bool atNext) {
        const RuleSide * const s = e->getSourceSide(Direction::Forward);
        RE * const text = s->getText() ? withoutCaptures(s->getText()) : nullptr;
        UCD::UnicodeSet first;
        bool nullable = false;
        if (text == nullptr || !textFirsts(text, sets, first, nullable) || nullable) return Match::Within;
        if (atNext && first.intersects(next)) return Match::Within;
        if (!first.intersects(R)) return Match::None;
        std::vector<RE *> items;
        if (const Seq * seq = dyn_cast<Seq>(text)) items.assign(seq->begin(), seq->end()); else items.push_back(text);
        if (!CharSetAnalysis::isSet(items.front())) return Match::Within;
        items.erase(items.begin());
        if (s->hasAfterContext()) items.push_back(s->getAfterContext());
        UCD::UnicodeSet second;
        nullable = false;
        if (!textFirsts(makeSeq(items.begin(), items.end()), sets, second, nullable) || nullable) return Match::Within;
        if (second.intersects(R)) return Match::Within;
        return second.intersects(next) ? Match::AtLast : Match::None;
    };
    //  The position in out of each earlier rule (as written there).
    std::map<const ConversionRule *, size_t> outAt;
    for (size_t i = 0; i < rules.size(); i++) {
        Rule * const r = rules[i];
        ConversionRule * const c = dyn_cast<ConversionRule>(r);
        if (c == nullptr) {
            if (!isa<VariableDefinitionRule>(r)) {
                earlier.clear();
                outAt.clear();
            }
            out.push_back(r);
            continue;
        }
        LeadingRun run;
        if (!leadingRun(c, expander, sets, run)) {
            earlier.push_back(c);
            outAt[c] = out.size();
            out.push_back(r);
            continue;
        }
        //  The block of the rules beginning with the same run (variable
        //  definitions between them kept in place).
        std::vector<std::pair<size_t, LeadingRun>> block{{i, run}};
        size_t j = i + 1;
        for (; j < rules.size(); j++) {
            if (isa<VariableDefinitionRule>(rules[j])) continue;
            const ConversionRule * const d = dyn_cast<ConversionRule>(rules[j]);
            LeadingRun next;
            if (d == nullptr || !leadingRun(d, expander, sets, next) || !(next.chars == run.chars) || next.n != run.n) break;
            block.emplace_back(j, next);
        }
        UCD::UnicodeSet next;
        for (const auto & b : block) next = next + b.second.restFirst;
        //  An earlier rule that may match at the last character of the run
        //  (before Ti), and not within it, does not where the run has at least
        //  two characters, ICU's rule then applying at its first: the rules
        //  R{k} Ti → Ci (k = max(n, 2)), placed before the first such earlier
        //  rule (which they then precede where the run has exactly k), take
        //  the last k characters of the run (and the deletion the others).
        bool blocked = false;
        size_t moveTo = SIZE_MAX;
        for (const ConversionRule * e : earlier) {
            const Match m = mayMatch(e, run.chars, next, run.n == 0);
            if (m == Match::Within) blocked = true;
            if (m == Match::AtLast && run.n > 0) {
                //  (Not before a rule split here.)
                const size_t at = outAt.count(e) ? outAt.at(e) : SIZE_MAX;
                if (at == SIZE_MAX) blocked = true; else moveTo = std::min(moveTo, at);
            }
            if (m == Match::AtLast && run.n == 0) blocked = true;
        }
        if (blocked) {
            earlier.push_back(c);
            outAt[c] = out.size();
            out.push_back(r);
            continue;
        }
        const int k = moveTo == SIZE_MAX ? run.n : std::max(run.n, 2);
        //  R } R{n,} (T1 | T2 | ...) → ; then R{n} Ti → Ci in order.
        std::vector<RE *> alternatives;
        for (const auto & b : block) {
            const RuleSide * const s = cast<ConversionRule>(rules[b.first])->getSourceSide(Direction::Forward);
            std::vector<RE *> items{withoutCaptures(b.second.rest)};
            if (s->hasAfterContext()) items.push_back(s->getAfterContext());
            alternatives.push_back(makeSeq(items.begin(), items.end()));
        }
        RE * const after = makeSeq({makeRep(run.R, k, Rep::UNBOUNDED_REP), makeAlt(alternatives.begin(), alternatives.end())});
        out.push_back(makeConversionRule(makeRuleSide(nullptr, run.R, after), Direction::Forward, makeRuleSide(makeSeq())));
        std::vector<Rule *> fixed;
        size_t b = 0;
        for (size_t x = i; x < j; x++) {
            if (b < block.size() && block[b].first == x) {
                ConversionRule * const d = cast<ConversionRule>(rules[x]);
                const RuleSide * const s = d->getSourceSide(Direction::Forward);
                std::vector<RE *> last(static_cast<size_t>(k), run.R);
                last.push_back(block[b].second.rest);
                fixed.push_back(makeConversionRule(makeRuleSide(nullptr, makeSeq(last.begin(), last.end()), s->getAfterContext()),
                                                   Direction::Forward, d->getResultSide(Direction::Forward)));
                //  (With the rules moved, those of the shorter runs, R{j} Ti → Ci for
                //  n <= j < k, stay in place, after the earlier rule.)
                for (int len = run.n; moveTo != SIZE_MAX && len < k; len++) {
                    std::vector<RE *> shorter(static_cast<size_t>(len), run.R);
                    shorter.push_back(block[b].second.rest);
                    out.push_back(makeConversionRule(makeRuleSide(nullptr, makeSeq(shorter.begin(), shorter.end()), s->getAfterContext()),
                                                     Direction::Forward, d->getResultSide(Direction::Forward)));
                }
                earlier.push_back(d);
                b++;
            } else if (moveTo == SIZE_MAX) {
                out.insert(out.end(), fixed.begin(), fixed.end());
                fixed.clear();
                out.push_back(rules[x]);
            } else {
                out.push_back(rules[x]);
            }
        }
        if (moveTo == SIZE_MAX) {
            out.insert(out.end(), fixed.begin(), fixed.end());
        } else {
            out.insert(out.begin() + moveTo, fixed.begin(), fixed.end());
            for (auto & at : outAt) if (at.second >= moveTo) at.second += fixed.size();
        }
        for (size_t x = i; x < j; x++) {
            if (const ConversionRule * d = dyn_cast<ConversionRule>(rules[x])) outAt[d] = SIZE_MAX;
        }
        if (split) *split += block.size();
        i = j - 1;
    }
    return out;
}

//  The marker of a rule split by SplitMiddleRuns (counting down from U+FDEF,
//  as the markers of the revisits count up from U+FDE0).
static constexpr UCD::codepoint_t MiddleRunMarkLast = 0xFDEF;

std::vector<Rule *> SplitMiddleRuns(const std::vector<Rule *> & rules, unsigned * split) {
    //  (Not with a global filter, which the markers would have to pass.)
    for (const Rule * r : rules) {
        if (isa<FilterRule>(r)) return rules;
    }
    Expander expander;
    CharSetAnalysis sets;
    std::vector<Rule *> out;
    unsigned markers = 0;
    size_t start = 0;
    while (start < rules.size()) {
        //  A group: the conversion rules and variable definitions up to the next
        //  other rule.
        size_t end = start;
        while (end < rules.size() && (isa<ConversionRule>(rules[end]) || isa<VariableDefinitionRule>(rules[end]))) end++;
        if (end == start) {
            out.push_back(rules[start++]);
            continue;
        }
        bool contexts = false;
        for (size_t i = start; i < end; i++) {
            if (const ConversionRule * c = dyn_cast<ConversionRule>(rules[i])) contexts |= c->getSourceSide(Direction::Forward)->hasBeforeContext();
        }
        std::vector<Rule *> first, body, after;
        for (size_t i = start; i < end; i++) {
            ConversionRule * const c = dyn_cast<ConversionRule>(rules[i]);
            std::vector<Rule *> replaced;
            [&] {
                if (c == nullptr || contexts || markers > 15) return;
                const RuleSide * const source = c->getSourceSide(Direction::Forward);
                const RuleSide * const result = c->getResultSide(Direction::Forward);
                if (result->hasCursor()) return;
                RE * const text = source->getText();
                const Seq * const seq = text ? dyn_cast<Seq>(text) : nullptr;
                if (seq == nullptr) return;
                std::vector<RE *> items(seq->begin(), seq->end());
                size_t at = 0;
                while (at < items.size() && !isa<Rep>(items[at])) at++;
                if (at == 0 || at + 1 >= items.size()) return;
                const Rep * const rep = cast<Rep>(items[at]);
                if (rep->getLB() < 1 || rep->getUB() != Rep::UNBOUNDED_REP || !CharSetAnalysis::isSet(rep->getRE())) return;
                UCD::UnicodeSet chars;
                std::set<std::u32string> strings;
                if (!expander.alternatives(rep->getRE(), chars, strings) || !strings.empty()) return;
                RE * const X = makeSeq(items.begin(), items.begin() + at);
                RE * const T = makeSeq(items.begin() + at + 1, items.end());
                if (!expander.isFixed(X) || !expander.isFixed(T) || referencesCaptures(X)) return;
                //  (The result may refer to the captures of T only.)
                for (size_t j = 0; j < at; j++) {
                    if (isa<Capture>(items[j])) return;
                }
                UCD::UnicodeSet tFirst, xLast;
                bool nullable = false;
                if (!textFirsts(T, sets, tFirst, nullable) || nullable || tFirst.intersects(chars)) return;
                RE * const R = rep->getRE();
                RE * const mark = makeCC(MiddleRunMarkLast - markers);
                std::vector<RE *> ahead{makeRep(R, rep->getLB(), Rep::UNBOUNDED_REP), withoutCaptures(T)};
                if (source->hasAfterContext()) ahead.push_back(source->getAfterContext());
                //  X } R{lb,} T → M, in place.
                replaced.push_back(makeConversionRule(makeRuleSide(nullptr, X, makeSeq(ahead.begin(), ahead.end())),
                                                      Direction::Forward, makeRuleSide(mark)));
                //  M { R → ; and M { T → C, first in the group (M is followed by the
                //  run and T).  A T of one captured set, with a result of literals
                //  and the capture, as rules of single characters (whose before
                //  contexts are matched against the converted text).
                first.push_back(makeConversionRule(makeRuleSide(mark, R, nullptr), Direction::Forward, makeRuleSide(makeSeq())));
                const Capture * const capture = (at + 2 == items.size()) ? dyn_cast<Capture>(items[at + 1]) : nullptr;
                UCD::UnicodeSet tChars;
                std::set<std::u32string> tStrings;
                if (capture && CharSetAnalysis::isSet(capture->getCapturedRE())
                        && expander.alternatives(capture->getCapturedRE(), tChars, tStrings) && tStrings.empty() && tChars.count() <= 128) {
                    for (const auto & range : tChars) {
                        for (UCD::codepoint_t ch = lo_codepoint(range); ch <= hi_codepoint(range); ch++) {
                            RE * const out = replaceReferences(result->getText(), {{capture, ch}});
                            first.push_back(makeConversionRule(makeRuleSide(mark, makeCC(ch), source->getAfterContext()),
                                                               Direction::Forward, makeRuleSide(out)));
                        }
                    }
                } else {
                    first.push_back(makeConversionRule(makeRuleSide(mark, T, source->getAfterContext()),
                                                       Direction::Forward, c->getResultSide(Direction::Forward)));
                }
                //  M → ; in a group after.
                after.push_back(makeConversionRule(makeRuleSide(mark), Direction::Forward, makeRuleSide(makeSeq())));
                markers++;
            }();
            if (replaced.empty()) {
                body.push_back(rules[i]);
            } else {
                if (split) (*split)++;
                body.insert(body.end(), replaced.begin(), replaced.end());
            }
        }
        //  (Variable definitions before the rules that may refer to them.)
        size_t defs = 0;
        while (defs < body.size() && isa<VariableDefinitionRule>(body[defs])) defs++;
        out.insert(out.end(), body.begin(), body.begin() + defs);
        out.insert(out.end(), first.begin(), first.end());
        out.insert(out.end(), body.begin() + defs, body.end());
        if (!after.empty()) {
            out.push_back(makeTransformRule(TransformID("Null")));
            out.insert(out.end(), after.begin(), after.end());
            out.push_back(makeTransformRule(TransformID("Null")));
        }
        start = end;
    }
    return out;
}

}

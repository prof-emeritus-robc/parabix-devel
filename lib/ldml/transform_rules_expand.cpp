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

}

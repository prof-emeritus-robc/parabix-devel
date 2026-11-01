#include <re/transforms/resolve_possessive.h>
#include <re/transforms/re_transformer.h>
#include <re/adt/adt.h>
#include <re/analysis/re_analysis.h>
#include <re/alphabet/alphabet.h>
#include <functional>

using namespace llvm;

namespace re {

//
//  A possessive repetition e{lb,ub}+ takes as many repetitions as it can (up
//  to ub), without backtracking.  In general, it is resolved with a negative
//  lookahead: e{lb,}+ is e{lb,} (?!e), and e{lb,ub}+ is e{lb,ub-1} (?!e) | e{ub}.
//
//  Where possible, the repetitions of a character class S are resolved without
//  assertions, which the lookahead implementation does not support everywhere:
//  - at the end of a lookahead body, S{lb,ub}+ matches where S{lb,ub} does (only
//    whether the lookahead holds matters);
//  - followed by $, or by items whose first characters are not in S, S{lb,ub}+
//    is S{lb,ub};
//  - followed by a character class B, S{lb,}+ B is S{lb,} (B - S), and
//    S{lb,ub}+ B is S{lb,ub-1} (B - S) | S{ub} B.
//  The repetitions of a class of strings X (sequences of character classes) are
//  likewise resolved without assertions at the end of a lookahead body, before
//  $, and before items whose first characters are not among the characters of
//  the strings (at any position, not only the first): X{lb,ub}+ is then
//  X{lb,ub}.  (Wherever the following items can begin, no string of X begins,
//  so (?!X) holds; and no occurrence of a string of X can extend over that
//  position, as ICU's matching of the longest string of a set might.)
//  The end of a top-level RE or of a lookbehind body is not treated as such:
//  there, a possessive repetition determines the match end or must reach the
//  position of the lookbehind.
//
//  The engine accepts a text where any choice of the alternatives of the
//  repeated item matches; a possessive repetition chooses one at each
//  repetition (the longest that matches, as ICU does, or the first, as PCRE
//  does; see PossessiveChoice).  A repeated item that is a class of literal
//  strings (with sets of single characters) is therefore first made
//  unambiguous (see unambiguousStrings): at each position, at most one of its
//  alternatives matches, the one the possessive repetition chooses, so that
//  the repetitions reach the same positions whatever the engine chooses, and
//  the forms above are exact (e{n}+ as e{n}, before $ or at the end of a
//  lookahead as e{lb,ub}, before items that cannot begin with a first character
//  of e as e{lb,ub}, and otherwise with the assertion).  Other repeated items,
//  of several characters, are resolved as before, which is exact only where
//  they match unambiguously.
//
class ResolvePossessive final : public RE_Transformer {
public:
    ResolvePossessive(PossessiveChoice choice) : RE_Transformer("ResolvePossessive"), mChoice(choice) {}
protected:
    RE * transformRep(Rep * rep) override;
    RE * transformSeq(Seq * seq) override;
    RE * transformAssertion(Assertion * a) override;
private:
    RE * resolve(Rep * rep, const std::vector<RE *> & rest, bool atLookaheadEnd);
    // Is the RE being transformed at the end of a lookahead body?
    bool mAtLookaheadEnd = false;
    const PossessiveChoice mChoice;
};

//  The alternatives of a class of strings, in order (through alternations and
//  names), each a sequence of character classes, or false.
static bool orderedStrings(RE * r, std::vector<std::vector<CC *>> & strings) {
    if (Name * n = dyn_cast<Name>(r)) {
        return n->getDefinition() && orderedStrings(n->getDefinition(), strings);
    } else if (Alt * alt = dyn_cast<Alt>(r)) {
        for (RE * a : *alt) {
            if (!orderedStrings(a, strings)) return false;
        }
        return true;
    }
    std::vector<RE *> items;
    //  (A sequence of character classes, strings or names of them.)
    std::function<bool (RE *)> flatten = [&](RE * e) -> bool {
        if (Seq * seq = dyn_cast<Seq>(e)) {
            for (RE * x : *seq) {
                if (!flatten(x)) return false;
            }
            return true;
        } else if (String * str = dyn_cast<String>(e)) {
            for (const char32_t c : *str) items.push_back(makeCC(c));
            return true;
        }
        items.push_back(e);
        return true;
    };
    flatten(r);
    std::vector<CC *> str;
    for (RE * item : items) {
        CC * const cc = resolveCharClass(item);
        if (cc == nullptr || cc->empty()) return false;
        str.push_back(cc);
    }
    if (str.empty()) return false;
    strings.push_back(std::move(str));
    return true;
}

//  A class of strings made unambiguous for a possessive repetition, or nullptr
//  (with firsts, the characters with which its matches begin).  The strings
//  of several characters must be literal; the alternatives of one character
//  may be sets.  The alternative chosen where several match (the preferred:
//  the longer one, or the earlier one) is the only one left to match there:
//    - a string w with a preferred string t as a proper prefix is never chosen,
//      and is dropped (with First, as (a|ab): ab is never taken);
//    - a string w that is a proper prefix of preferred strings w t' is taken
//      only where none of them matches: w (?!t'...);
//    - the characters of a set of one character that begin longer strings are
//      such strings w (the others have no conflict).
//  At each position, at most one alternative then matches.
static RE * unambiguousStrings(RE * e, PossessiveChoice choice, CC *& firsts) {
    std::vector<std::vector<CC *>> alternatives;
    if (!orderedStrings(e, alternatives)) return nullptr;
    const cc::Alphabet * alphabet = nullptr;
    bool multi = false;
    UCD::UnicodeSet starts;                     // the first characters of the strings of several characters
    for (const auto & a : alternatives) {
        for (CC * cc : a) {
            if (alphabet && cc->getAlphabet() != alphabet) return nullptr;
            alphabet = cc->getAlphabet();
        }
        if (a.size() > 1) {
            multi = true;
            for (CC * cc : a) {
                if (cc->count() != 1) return nullptr;
            }
            starts.insert(lo_codepoint(a.front()->front()));
        }
    }
    if (!multi) return nullptr;
    struct Literal {
        std::u32string text;
        size_t order;
    };
    std::vector<Literal> literals;
    UCD::UnicodeSet others;                     // the characters of sets of one character, without conflicts
    for (size_t i = 0; i < alternatives.size(); i++) {
        const auto & a = alternatives[i];
        if (a.size() == 1) {
            const UCD::UnicodeSet chars = *a.front();
            others = others + (chars - starts);
            const UCD::UnicodeSet involved = chars & starts;
            if (involved.count() > 64) return nullptr;
            for (const auto & range : involved) {
                for (UCD::codepoint_t c = lo_codepoint(range); c <= hi_codepoint(range); c++) {
                    literals.push_back(Literal{std::u32string(1, static_cast<char32_t>(c)), i});
                }
            }
        } else {
            std::u32string text;
            for (CC * cc : a) text.push_back(static_cast<char32_t>(lo_codepoint(cc->front())));
            literals.push_back(Literal{text, i});
        }
    }
    //  (The same string twice: the earlier.)
    std::vector<Literal> distinct;
    for (const Literal & l : literals) {
        bool seen = false;
        for (const Literal & d : distinct) seen |= d.text == l.text;
        if (!seen) distinct.push_back(l);
    }
    auto preferred = [&](const Literal & t, const Literal & w) {
        return choice == PossessiveChoice::Longest ? t.text.size() > w.text.size() : t.order < w.order;
    };
    std::vector<RE *> alts;
    UCD::UnicodeSet first = others;
    if (!others.empty()) alts.push_back(makeCC(std::move(others), alphabet));
    for (const Literal & w : distinct) {
        bool dropped = false;
        std::vector<RE *> guards;
        for (const Literal & t : distinct) {
            if (&t == &w || !preferred(t, w)) continue;
            if (t.text.size() < w.text.size() && w.text.compare(0, t.text.size(), t.text) == 0) {
                dropped = true;
            } else if (t.text.size() > w.text.size() && t.text.compare(0, w.text.size(), w.text) == 0) {
                std::vector<RE *> suffix;
                for (size_t k = w.text.size(); k < t.text.size(); k++) suffix.push_back(makeCC(static_cast<UCD::codepoint_t>(t.text[k]), alphabet));
                guards.push_back(makeSeq(suffix.begin(), suffix.end()));
            }
        }
        if (dropped) continue;
        std::vector<RE *> items;
        for (char32_t c : w.text) items.push_back(makeCC(static_cast<UCD::codepoint_t>(c), alphabet));
        if (!guards.empty()) items.push_back(makeNegativeLookAheadAssertion(makeAlt(guards.begin(), guards.end())));
        alts.push_back(makeSeq(items.begin(), items.end()));
        first.insert(static_cast<UCD::codepoint_t>(w.text.front()));
    }
    firsts = makeCC(std::move(first), alphabet);
    return makeAlt(alts.begin(), alts.end());
}

//  The general resolution of e{lb,ub}+.
static RE * withAssertion(RE * e, const int lb, const int ub) {
    RE * const notE = makeNegativeLookAheadAssertion(e);
    if (ub == Rep::UNBOUNDED_REP) {
        return makeSeq({makeRep(e, lb, ub), notE});
    }
    return makeAlt({makeSeq({makeRep(e, lb, ub - 1), notE}), makeRep(e, ub, ub)});
}

//  The characters that can begin the items rest (the items following a
//  repetition in a sequence), or nullptr if they cannot be determined: an item
//  that may be empty (a repetition with no lower bound) adds the first
//  characters of its body, and those of the items after it; assertions are
//  passed over (which can only add characters); $ ends the items.  Items that
//  may all be empty, up to the end of the sequence, are followed by whatever
//  follows it, which is not known here.  (The items may include sequences, such
//  as resolved repetitions with their following items, which are seen through,
//  and Strings, which are the sequences of their singleton CCs.)
static void flattenItems(const std::vector<RE *> & items, std::vector<RE *> & flat) {
    for (RE * item : items) {
        if (Seq * const seq = dyn_cast<Seq>(item)) {
            flattenItems(std::vector<RE *>(seq->begin(), seq->end()), flat);
        } else if (String * const s = dyn_cast<String>(item)) {
            for (const char32_t c : *s) {
                flat.push_back(makeCC(c));
            }
        } else {
            flat.push_back(item);
        }
    }
}

static CC * firstCharClassOfItems(const std::vector<RE *> & items) {
    std::vector<RE *> rest;
    flattenItems(items, rest);
    CC * first = nullptr;
    auto add = [&](CC * cc) -> bool {
        if (cc == nullptr || (first && first->getAlphabet() != cc->getAlphabet())) return false;
        first = first ? makeCC(first, cc) : cc;
        return true;
    };
    for (RE * item : rest) {
        if (isa<End>(item)) {
            return first ? first : makeCC();
        } else if (isa<Assertion>(item)) {
            continue;
        } else if (Rep * const rep = dyn_cast<Rep>(item)) {
            if (rep->getLB() == 0) {
                if (!add(firstCharClass(rep->getRE()))) return nullptr;
                continue;
            }
        }
        if (!add(firstCharClass(item))) return nullptr;
        return first;
    }
    return nullptr;
}

//  The possessive repetition rep (with its body transformed), followed by the
//  items rest (transformed), within the same sequence; returns the items of the
//  sequence that replace rep and rest.
RE * ResolvePossessive::resolve(Rep * rep, const std::vector<RE *> & rest, bool atLookaheadEnd) {
    RE * const e = rep->getRE();
    const int lb = rep->getLB();
    const int ub = rep->getUB();
    CC * const S = resolveCharClass(e);
    //  A class of strings, made unambiguous: its repetitions reach the same
    //  positions however they are matched.
    CC * firsts = nullptr;
    if (RE * const d = S ? nullptr : unambiguousStrings(e, mChoice, firsts)) {
        RE * const following = makeSeq(rest.begin(), rest.end());
        if (lb == ub || (rest.empty() && atLookaheadEnd) || (!rest.empty() && isa<End>(rest.front()))) {
            return makeSeq({makeRep(d, lb, ub), following});
        }
        if (!rest.empty()) {
            CC * const first = firstCharClassOfItems(rest);
            if (first && first->getAlphabet() == firsts->getAlphabet() && intersectCC(first, firsts)->empty()) {
                return makeSeq({makeRep(d, lb, ub), following});
            }
        }
        return makeSeq({withAssertion(d, lb, ub), following});
    }
    if (lb == ub) {
        return makeSeq({makeRep(e, lb, ub), makeSeq(rest.begin(), rest.end())});
    }
    if (S) {
        if (rest.empty()) {
            if (atLookaheadEnd) return makeRep(e, lb, ub);
        } else if (isa<End>(rest.front())) {
            return makeSeq({makeRep(e, lb, ub), makeSeq(rest.begin(), rest.end())});
        } else {
            RE * const following = makeSeq(rest.begin(), rest.end());
            CC * const first = firstCharClassOfItems(rest);
            if (first && first->getAlphabet() == S->getAlphabet()) {
                if (intersectCC(first, S)->empty()) {
                    return makeSeq({makeRep(e, lb, ub), following});
                }
                //  A String (not empty, see transformSeq) is the sequence of
                //  its singleton CCs: its first CC begins the items.
                RE * front = rest.front();
                RE * after = makeSeq(rest.begin() + 1, rest.end());
                if (String * const s = dyn_cast<String>(front)) {
                    front = makeCC((*s)[0]);
                    if (s->size() > 1) {
                        after = makeSeq({makeString(std::u32string(s->begin() + 1, s->end())), after});
                    }
                }
                if (CC * const B = resolveCharClass(front)) {
                    RE * const notS = makeSeq({subtractCC(B, S), after});
                    if (ub == Rep::UNBOUNDED_REP) {
                        return makeSeq({makeRep(e, lb, ub), notS});
                    }
                    return makeAlt({makeSeq({makeRep(e, lb, ub - 1), notS}),
                                    makeSeq({makeRep(e, ub, ub), B, after})});
                }
            }
        }
    }
    std::vector<std::vector<CC *>> strings;
    if (!S && parseStringClass(e, strings)) {
        CC * chars = nullptr;
        for (const auto & str : strings) {
            for (CC * cc : str) {
                if (chars && chars->getAlphabet() != cc->getAlphabet()) goto general;
                chars = chars ? makeCC(chars, cc) : cc;
            }
        }
        if (rest.empty()) {
            if (atLookaheadEnd) return makeRep(e, lb, ub);
        } else if (isa<End>(rest.front())) {
            return makeSeq({makeRep(e, lb, ub), makeSeq(rest.begin(), rest.end())});
        } else {
            RE * const following = makeSeq(rest.begin(), rest.end());
            CC * const first = firstCharClassOfItems(rest);
            if (first && (first->getAlphabet() == chars->getAlphabet()) && intersectCC(first, chars)->empty()) {
                return makeSeq({makeRep(e, lb, ub), following});
            }
        }
    }
general:
    return makeSeq({withAssertion(e, lb, ub), makeSeq(rest.begin(), rest.end())});
}

RE * ResolvePossessive::transformRep(Rep * rep) {
    const bool atEnd = mAtLookaheadEnd;
    mAtLookaheadEnd = false;
    RE * const e = transform(rep->getRE());
    mAtLookaheadEnd = atEnd;
    if (rep->getKind() == Rep::Kind::Possessive) {
        return resolve(cast<Rep>(Rep::Create(e, rep->getLB(), rep->getUB(), Rep::Kind::Possessive)), {}, atEnd);
    }
    if (e == rep->getRE()) return rep;
    return makeRep(e, rep->getLB(), rep->getUB());
}

RE * ResolvePossessive::transformSeq(Seq * seq) {
    const bool atEnd = mAtLookaheadEnd;
    std::vector<RE *> items(seq->begin(), seq->end());
    //  Transform the items from the last, so that each possessive repetition is
    //  resolved with the transformed items following it.
    std::vector<RE *> rest;
    for (size_t i = items.size(); i-- > 0; ) {
        mAtLookaheadEnd = atEnd && rest.empty();
        Rep * const rep = dyn_cast<Rep>(items[i]);
        if (rep && rep->getKind() == Rep::Kind::Possessive) {
            mAtLookaheadEnd = false;
            RE * const e = transform(rep->getRE());
            Rep * const resolvedBody = cast<Rep>(Rep::Create(e, rep->getLB(), rep->getUB(), Rep::Kind::Possessive));
            RE * const replaced = resolve(resolvedBody, rest, atEnd && rest.empty());
            rest.clear();
            rest.push_back(replaced);
        } else {
            RE * const t = transform(items[i]);
            //  An empty String, the empty sequence, has no items.
            if (isa<String>(t) && cast<String>(t)->empty()) continue;
            rest.insert(rest.begin(), t);
        }
    }
    mAtLookaheadEnd = atEnd;
    return makeSeq(rest.begin(), rest.end());
}

RE * ResolvePossessive::transformAssertion(Assertion * a) {
    const bool atEnd = mAtLookaheadEnd;
    mAtLookaheadEnd = (a->getKind() == Assertion::Kind::LookAhead);
    RE * const asserted = transform(a->getAsserted());
    mAtLookaheadEnd = atEnd;
    if (asserted == a->getAsserted()) return a;
    return makeAssertion(asserted, a->getKind(), a->getSense());
}

RE * resolvePossessiveQuantifiers(RE * re, PossessiveChoice choice) {
    return ResolvePossessive(choice).transformRE(re);
}

}

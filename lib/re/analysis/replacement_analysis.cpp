/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/analysis/replacement_analysis.h>

#include <algorithm>
#include <climits>
#include <llvm/Support/Casting.h>
#include <re/adt/adt.h>
#include <re/alphabet/alphabet.h>
#include <re/analysis/re_analysis.h>
#include <re/analysis/re_inspector.h>

using namespace llvm;

namespace re {

namespace {

struct CaptureCollector final : public RE_Inspector {
    CaptureCollector(std::map<std::string, Capture *> & captures)
    : RE_Inspector(NameProcessingMode::None, InspectionMode::IgnoreNonUnique), mCaptures(captures) {}
    void inspectCapture(Capture * c) override {
        mCaptures.emplace(c->getName(), c);
        RE_Inspector::inspectCapture(c);
    }
    std::map<std::string, Capture *> & mCaptures;
};

// A CC of one Unicode codepoint: a literal character of a replacement.
bool literalCodepoint(const RE * re, codepoint_t & cp) {
    const CC * const x = dyn_cast<CC>(re);
    if ((x == nullptr) || (x->getAlphabet() != &cc::Unicode) || x->empty() || (x->min_codepoint() != x->max_codepoint())) {
        return false;
    }
    cp = x->min_codepoint();
    return true;
}

// The items of a replacement, if it is a sequence of literal characters and
// references.
bool replacementItems(RE * replacement, std::vector<RE *> & items) {
    if (Seq * const seq = dyn_cast<Seq>(replacement)) {
        items.assign(seq->begin(), seq->end());
    } else {
        items.push_back(replacement);
    }
    for (RE * item : items) {
        codepoint_t cp;
        if (!literalCodepoint(item, cp) && !isa<Reference>(item)) return false;
    }
    return true;
}

// The one string that an RE matches, if it matches one only: literal
// characters, possibly captured or repeated a fixed number of times.
bool singleString(RE * re, std::u32string & s) {
    codepoint_t cp;
    if (literalCodepoint(re, cp)) {
        s.push_back(static_cast<char32_t>(cp));
        return true;
    } else if (Seq * const seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) {
            if (!singleString(e, s)) return false;
        }
        return true;
    } else if (Capture * const c = dyn_cast<Capture>(re)) {
        return singleString(c->getCapturedRE(), s);
    } else if (Rep * const rep = dyn_cast<Rep>(re)) {
        std::u32string body;
        if ((rep->getLB() != rep->getUB()) || !singleString(rep->getRE(), body)) return false;
        for (int k = 0; k < rep->getLB(); k++) s += body;
        return true;
    } else if (Alt * const alt = dyn_cast<Alt>(re)) {
        return (alt->size() == 1) && singleString(alt->front(), s);
    }
    return false;
}

// The captures of an RE that are on every path (not within a repetition, an
// alternation or an assertion): they have one binding in every match.
void unconditionalCaptures(RE * r, std::map<std::string, Capture *> & captures) {
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) unconditionalCaptures(e, captures);
    } else if (Alt * const alt = dyn_cast<Alt>(r)) {
        if (alt->size() == 1) unconditionalCaptures(alt->front(), captures);
    } else if (Capture * const c = dyn_cast<Capture>(r)) {
        captures.emplace(c->getName(), c);
        unconditionalCaptures(c->getCapturedRE(), captures);
    } else if (Group * const g = dyn_cast<Group>(r)) {
        unconditionalCaptures(g->getRE(), captures);
    }
}

unsigned utf8Length(char32_t cp) {
    return (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
}

// Is r zero-width (an assertion, Start or End)?
bool zeroWidthItem(const RE * r) {
    if (isa<Assertion>(r) || isa<Start>(r) || isa<End>(r)) return true;
    if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(r)) {
        return pe->getKind() == PropertyExpression::Kind::Boundary;
    }
    return false;
}

// The class of a character class item (of Unicode characters), or nullptr.
CC * unicodeClass(RE * r) {
    CC * const cc = resolveCharClass(r);
    return (cc && (cc->getAlphabet() == &cc::Unicode)) ? cc : nullptr;
}

// The characters that can begin a nonempty match of r, and whether r can
// match the empty string.  False if unknown.
bool firstChars(RE * r, UCD::UnicodeSet & chars, bool & nullable) {
    nullable = false;
    if (CC * const cc = unicodeClass(r)) {
        chars = chars + *cc;
        return true;
    }
    if (zeroWidthItem(r)) {
        nullable = true;
        return true;
    }
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        nullable = true;
        for (auto i = seq->begin(); nullable && (i != seq->end()); ++i) {
            if (!firstChars(*i, chars, nullable)) return false;
        }
        return true;
    } else if (Alt * const alt = dyn_cast<Alt>(r)) {
        for (RE * e : *alt) {
            bool e_nullable;
            if (!firstChars(e, chars, e_nullable)) return false;
            nullable |= e_nullable;
        }
        return true;
    } else if (Rep * const rep = dyn_cast<Rep>(r)) {
        if (rep->getUB() == 0) {
            nullable = true;
            return true;
        }
        if (!firstChars(rep->getRE(), chars, nullable)) return false;
        nullable |= (rep->getLB() == 0);
        return true;
    } else if (Capture * const c = dyn_cast<Capture>(r)) {
        return firstChars(c->getCapturedRE(), chars, nullable);
    } else if (Group * const g = dyn_cast<Group>(r)) {
        return firstChars(g->getRE(), chars, nullable);
    } else if (Name * const n = dyn_cast<Name>(r)) {
        return n->getDefinition() && firstChars(n->getDefinition(), chars, nullable);
    }
    return false;
}

// The characters that can be matched by r (not those of its assertions).
// False if unknown.
bool allChars(RE * r, UCD::UnicodeSet & chars) {
    if (CC * const cc = unicodeClass(r)) {
        chars = chars + *cc;
        return true;
    }
    if (zeroWidthItem(r)) return true;
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) if (!allChars(e, chars)) return false;
        return true;
    } else if (Alt * const alt = dyn_cast<Alt>(r)) {
        for (RE * e : *alt) if (!allChars(e, chars)) return false;
        return true;
    } else if (Rep * const rep = dyn_cast<Rep>(r)) {
        return (rep->getUB() == 0) || allChars(rep->getRE(), chars);
    } else if (Capture * const c = dyn_cast<Capture>(r)) {
        return allChars(c->getCapturedRE(), chars);
    } else if (Group * const g = dyn_cast<Group>(r)) {
        return allChars(g->getRE(), chars);
    } else if (Name * const n = dyn_cast<Name>(r)) {
        return n->getDefinition() && allChars(n->getDefinition(), chars);
    }
    return false;
}

// An item of a search pattern for the capture walk: an RE without the
// captures walked, or the opening (2j) or closing (2j + 1) of capture j.
struct WalkItem {
    RE * re;
    int mark;
};

bool containsCapture(RE * r, const std::map<std::string, unsigned> & wanted) {
    for (const auto & c : capturesByName(r)) {
        if (wanted.count(c.first)) return true;
    }
    return false;
}

bool flattenForWalk(RE * r, const std::map<std::string, unsigned> & wanted, std::vector<WalkItem> & items,
                    RE *& construct, std::string & reason) {
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) {
            if (!flattenForWalk(e, wanted, items, construct, reason)) return false;
        }
        return true;
    }
    if (Alt * const alt = dyn_cast<Alt>(r)) {
        if (alt->size() == 1) return flattenForWalk(alt->front(), wanted, items, construct, reason);
    }
    if (!containsCapture(r, wanted)) {
        items.push_back(WalkItem{r, -1});
        return true;
    }
    if (Capture * const c = dyn_cast<Capture>(r)) {
        const auto f = wanted.find(c->getName());
        if (f != wanted.end()) items.push_back(WalkItem{c, static_cast<int>(2 * f->second)});
        if (!flattenForWalk(c->getCapturedRE(), wanted, items, construct, reason)) return false;
        if (f != wanted.end()) items.push_back(WalkItem{c, static_cast<int>(2 * f->second + 1)});
        return true;
    } else if (Group * const g = dyn_cast<Group>(r)) {
        return flattenForWalk(g->getRE(), wanted, items, construct, reason);
    }
    construct = r;
    reason = "a capture within a repetition, an alternation or an assertion has no unique binding in a match";
    return false;
}

// Is a run of X at item i always maximal within the match: the characters
// that can follow it within the match are not of X, or it is followed by
// a negative lookahead (?!Y) with Y including X, or by End?
bool maximalRun(const std::vector<WalkItem> & items, size_t i, const CC * X) {
    for (size_t j = i + 1; j < items.size(); j++) {
        RE * const r = items[j].re;
        if (items[j].mark >= 0) continue;
        if (zeroWidthItem(r)) {
            if (isa<End>(r)) return true;
            if (const Assertion * const a = dyn_cast<Assertion>(r)) {
                if ((a->getKind() == Assertion::Kind::LookAhead) && (a->getSense() == Assertion::Sense::Negative)) {
                    const CC * const Y = unicodeClass(a->getAsserted());
                    if (Y && X->subset(*Y)) return true;
                }
            }
            continue;
        }
        UCD::UnicodeSet first;
        bool nullable;
        if (!firstChars(r, first, nullable) || first.intersects(*X)) return false;
        if (!nullable) return true;
    }
    // The end of the match ends the run (the walk stays within the match).
    return true;
}

}

std::map<std::string, Capture *> capturesByName(RE * re) {
    std::map<std::string, Capture *> captures;
    CaptureCollector(captures).inspectRE(re);
    return captures;
}

std::vector<Reference *> unresolvedReferences(RE * replacement, RE * search) {
    const auto captures = capturesByName(search);
    std::vector<RE *> items;
    replacementItems(replacement, items);
    std::vector<Reference *> unresolved;
    for (RE * item : items) {
        if (Reference * const r = dyn_cast<Reference>(item)) {
            if (captures.count(r->getName()) == 0) unresolved.push_back(r);
        }
    }
    return unresolved;
}

bool foldReplacement(RE * replacement, RE * search, std::vector<ReplacementItem> & folded) {
    folded.clear();
    std::vector<RE *> items;
    if (!replacementItems(replacement, items)) return false;
    std::map<std::string, Capture *> captures;
    unconditionalCaptures(search, captures);
    auto literal = [&](const std::u32string & s) {
        if (folded.empty() || folded.back().reference) folded.emplace_back();
        folded.back().literal += s;
    };
    for (RE * item : items) {
        codepoint_t cp;
        if (literalCodepoint(item, cp)) {
            literal(std::u32string(1, static_cast<char32_t>(cp)));
            continue;
        }
        Reference * const r = cast<Reference>(item);
        const auto f = captures.find(r->getName());
        std::u32string s;
        if ((f != captures.end()) && singleString(f->second->getCapturedRE(), s)) {
            if (!s.empty()) literal(s);
        } else {
            folded.emplace_back();
            folded.back().reference = r;
        }
    }
    return true;
}

std::pair<int, int> replacementLengthRange(RE * replacement, RE * search, const cc::Alphabet * alphabet) {
    std::vector<RE *> items;
    if (!replacementItems(replacement, items)) return std::make_pair(0, INT_MAX);
    const auto captures = capturesByName(search);
    long lo = 0, hi = 0;
    for (RE * item : items) {
        codepoint_t cp;
        if (literalCodepoint(item, cp)) {
            const unsigned lgth = (alphabet == &cc::UTF8) ? utf8Length(static_cast<char32_t>(cp)) : 1;
            lo += lgth;
            hi += lgth;
            continue;
        }
        const auto f = captures.find(cast<Reference>(item)->getName());
        if (f == captures.end()) return std::make_pair(0, INT_MAX);
        const auto range = getLengthRange(f->second, alphabet);
        lo += range.first;
        hi += range.second;
    }
    // (INT_MAX stands for no upper bound.)
    return std::make_pair(static_cast<int>(std::min<long>(lo, INT_MAX)), static_cast<int>(std::min<long>(hi, INT_MAX)));
}

std::string toUTF8String(const std::u32string & s) {
    std::string utf8;
    for (const char32_t cp : s) {
        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return utf8;
}

bool planCaptureWalk(RE * search, const std::vector<std::string> & captures, CaptureWalk & walk,
                     RE *& construct, std::string & reason) {
    walk = CaptureWalk{};
    std::map<std::string, unsigned> wanted;
    std::vector<std::string> names;
    for (const std::string & c : captures) {
        if (wanted.emplace(c, names.size()).second) names.push_back(c);
    }
    std::vector<WalkItem> items;
    if (!flattenForWalk(search, wanted, items, construct, reason)) return false;
    // The fixed length of the items from each on (or -1).
    const size_t n = items.size();
    std::vector<int> suffix(n + 1, 0);
    for (size_t i = n; i-- > 0; ) {
        if (items[i].mark >= 0) {
            suffix[i] = suffix[i + 1];
            continue;
        }
        const auto range = getLengthRange(items[i].re, &cc::Unicode);
        suffix[i] = ((suffix[i + 1] >= 0) && (range.first == range.second)) ? suffix[i + 1] + range.first : -1;
    }
    std::vector<CaptureWalk::CaptureBoundaries> found(names.size());
    std::vector<bool> seen(names.size(), false);
    bool forward = true;
    long prefixMin = 0;
    for (size_t i = 0; i < n; i++) {
        RE * const r = items[i].re;
        if (items[i].mark >= 0) {
            const unsigned j = items[i].mark / 2;
            const bool open = (items[i].mark % 2) == 0;
            CaptureWalk::Boundary b;
            if (forward) {
                b.at = walk.steps.size();
            } else if (suffix[i + 1] >= 0) {
                b.fromEnd = true;
                b.at = suffix[i + 1];
            } else {
                construct = r;
                reason = std::string("the ") + (open ? "start" : "end") + " of the capture is not found from the "
                         "start of the match (by fixed lengths and maximal runs of a class), nor at a fixed "
                         "length from its end";
                return false;
            }
            if (open) {
                found[j].start = b;
                found[j].open = i;
                found[j].prefixMinLength = static_cast<int>(std::min<long>(prefixMin, INT_MAX));
                seen[j] = true;
            } else {
                found[j].follow = b;
                found[j].close = i;
            }
            continue;
        }
        const auto range = getLengthRange(r, &cc::Unicode);
        prefixMin += range.first;
        if (!forward || zeroWidthItem(r)) continue;
        if (range.first == range.second) {
            if (range.first > 0) walk.steps.push_back(CaptureWalk::Step{range.first, nullptr});
            continue;
        }
        // An item of variable length matching characters of X only, followed
        // within the match by a character not of X: its match is the run of X.
        UCD::UnicodeSet chars;
        if (allChars(r, chars)) {
            CC * const X = makeCC(std::move(chars), &cc::Unicode);
            if (maximalRun(items, i, X)) {
                walk.steps.push_back(CaptureWalk::Step{0, X});
                continue;
            }
        }
        forward = false;
    }
    for (size_t j = 0; j < names.size(); j++) {
        if (!seen[j]) {
            construct = search;
            reason = "the search pattern has no capture " + names[j];
            return false;
        }
    }
    for (const std::string & c : captures) {
        walk.captures.push_back(found[wanted[c]]);
    }
    return true;
}

}

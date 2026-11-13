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

struct AllCaptureCollector final : public RE_Inspector {
    AllCaptureCollector(std::map<std::string, std::vector<Capture *>> & captures)
    : RE_Inspector(NameProcessingMode::None, InspectionMode::IgnoreNonUnique), mCaptures(captures) {}
    void inspectCapture(Capture * c) override {
        std::vector<Capture *> & of = mCaptures[c->getName()];
        if (std::find(of.begin(), of.end(), c) == of.end()) of.push_back(c);
        RE_Inspector::inspectCapture(c);
    }
    std::map<std::string, std::vector<Capture *>> & mCaptures;
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
    // (The singleton CCs of a String are items of the sequence.)
    auto addItem = [&](RE * item) {
        if (const String * const s = dyn_cast<String>(item)) {
            for (const char32_t c : *s) items.push_back(makeCC(c));
        } else {
            items.push_back(item);
        }
    };
    if (Seq * const seq = dyn_cast<Seq>(replacement)) {
        for (RE * item : *seq) addItem(item);
    } else {
        addItem(replacement);
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
    } else if (String * const str = dyn_cast<String>(re)) {
        s.append(str->begin(), str->end());
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

// The captures of an RE that are on every path, by name: not within a
// repetition or an assertion, nor within an alternation unless each of its
// alternatives binds a capture of the name on every path (as the alternatives
// of a branch reset group (?|...) do).  They have one binding in every match,
// by one of the captures of the name.
void unconditionalCaptures(RE * r, std::map<std::string, std::vector<Capture *>> & captures) {
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) unconditionalCaptures(e, captures);
    } else if (Alt * const alt = dyn_cast<Alt>(r)) {
        if (alt->size() == 1) {
            unconditionalCaptures(alt->front(), captures);
        } else if (alt->size() > 1) {
            std::vector<std::map<std::string, std::vector<Capture *>>> of;
            for (RE * e : *alt) {
                of.emplace_back();
                unconditionalCaptures(e, of.back());
            }
            for (const auto & c : of[0]) {
                bool everywhere = true;
                for (size_t k = 1; everywhere && (k < of.size()); k++) everywhere = (of[k].count(c.first) != 0);
                if (!everywhere) continue;
                std::vector<Capture *> & all = captures[c.first];
                for (auto & m : of) all.insert(all.end(), m[c.first].begin(), m[c.first].end());
            }
        }
    } else if (Capture * const c = dyn_cast<Capture>(r)) {
        captures[c->getName()].push_back(c);
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
    } else if (String * const s = dyn_cast<String>(r)) {
        nullable = s->empty();
        if (!nullable) chars.insert((*s)[0]);
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

// The characters that can end a nonempty match of r, and whether r can match
// the empty string.  False if unknown.
bool lastChars(RE * r, UCD::UnicodeSet & chars, bool & nullable) {
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        nullable = true;
        for (auto i = seq->rbegin(); nullable && (i != seq->rend()); ++i) {
            if (!lastChars(*i, chars, nullable)) return false;
        }
        return true;
    } else if (String * const s = dyn_cast<String>(r)) {
        nullable = s->empty();
        if (!nullable) chars.insert((*s)[s->size() - 1]);
        return true;
    } else if (Alt * const alt = dyn_cast<Alt>(r)) {
        nullable = false;
        for (RE * e : *alt) {
            bool e_nullable;
            if (!lastChars(e, chars, e_nullable)) return false;
            nullable |= e_nullable;
        }
        return true;
    } else if (Rep * const rep = dyn_cast<Rep>(r)) {
        if (rep->getUB() == 0) {
            nullable = true;
            return true;
        }
        if (!lastChars(rep->getRE(), chars, nullable)) return false;
        nullable |= (rep->getLB() == 0);
        return true;
    } else if (Capture * const c = dyn_cast<Capture>(r)) {
        return lastChars(c->getCapturedRE(), chars, nullable);
    } else if (Group * const g = dyn_cast<Group>(r)) {
        return lastChars(g->getRE(), chars, nullable);
    } else if (Name * const n = dyn_cast<Name>(r)) {
        return n->getDefinition() && lastChars(n->getDefinition(), chars, nullable);
    }
    // A class, or a zero-width item.
    return firstChars(r, chars, nullable);
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
    } else if (String * const s = dyn_cast<String>(r)) {
        for (const char32_t c : *s) chars.insert(c);
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
    if (String * const s = dyn_cast<String>(r)) {
        // The items of its singleton CCs.
        for (const char32_t c : *s) items.push_back(WalkItem{makeCC(c), -1});
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
bool followNotIn(const std::vector<WalkItem> & items, size_t from, const CC * X);

bool maximalRun(const std::vector<WalkItem> & items, size_t i, const CC * X) {
    return followNotIn(items, i + 1, X);
}

// The same for a run of X ending before item from.
bool followNotIn(const std::vector<WalkItem> & items, size_t from, const CC * X) {
    for (size_t j = from; j < items.size(); j++) {
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


// Is a run of X at item i always maximal backward within the match: the
// characters that can precede it within the match are not of X, or it is
// preceded by a negative lookbehind (?<!Y) with Y including X, or by Start?
// (The start of the match does not end it: a walk back may continue into an
// adjacent match.)
bool maximalRunBackward(const std::vector<WalkItem> & items, size_t i, const CC * X) {
    for (size_t j = i; j-- > 0; ) {
        RE * const r = items[j].re;
        if (items[j].mark >= 0) continue;
        if (zeroWidthItem(r)) {
            if (isa<Start>(r)) return true;
            if (const Assertion * const a = dyn_cast<Assertion>(r)) {
                if ((a->getKind() == Assertion::Kind::LookBehind) && (a->getSense() == Assertion::Sense::Negative)) {
                    const CC * const Y = unicodeClass(a->getAsserted());
                    if (Y && X->subset(*Y)) return true;
                }
            }
            continue;
        }
        UCD::UnicodeSet last;
        bool nullable;
        if (!lastChars(r, last, nullable) || last.intersects(*X)) return false;
        if (!nullable) return true;
    }
    return false;
}

// The fixed length of the items from each on (or -1), skipping the marks.
std::vector<int> fixedSuffixes(const std::vector<WalkItem> & items) {
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
    return suffix;
}

// The walk back from the last character of the match: its steps, and the
// number of its steps after the items from each mark on (back, or -1 where it
// stops).
void backWalk(const std::vector<WalkItem> & items, std::vector<int> & back, std::vector<CaptureWalk::Step> & backSteps) {
    const size_t n = items.size();
    back.assign(n, -1);
    backSteps.clear();
    for (size_t i = n; i-- > 0; ) {
        RE * const r = items[i].re;
        if (items[i].mark >= 0) {
            back[i] = static_cast<int>(backSteps.size());
            continue;
        }
        if (zeroWidthItem(r)) continue;
        const auto range = getLengthRange(r, &cc::Unicode);
        if (range.first == range.second) {
            if (range.first > 0) backSteps.push_back(CaptureWalk::Step{range.first, nullptr});
            continue;
        }
        UCD::UnicodeSet chars;
        if (allChars(r, chars)) {
            CC * const X = makeCC(std::move(chars), &cc::Unicode);
            if (maximalRunBackward(items, i, X)) {
                backSteps.push_back(CaptureWalk::Step{0, X});
                continue;
            }
        }
        break;
    }
}

std::string boundaryNotFound(bool open, bool backward) {
    return std::string("the ") + (open ? "start" : "end") + " of the capture is not found from the "
           "start of the match (by fixed lengths and maximal runs of a class), nor at a fixed "
           "length from its end" +
           (backward ? ", nor by a walk back from its end after a prefix that is not nullable"
                     : " (and the walk back from its end, by reversal, is not allowed)");
}

//
// The walk of captures bound within alternations (as by a branch reset group
// (?|...), whose alternatives bind captures of the same names).  The search
// pattern is expanded into its paths through the alternations binding the
// captures walked, and through its other alternations if there are not too
// many paths: each path is a sequence of items as for a search pattern
// without such alternations, and every match of the pattern is a match of
// one of them.  A walk found for the paths together, giving each boundary in
// the same way on every path (after the same steps forward from the start of
// the match, at the same fixed length from its end, or after the same steps
// back from its end), finds the boundaries in every match, whichever path it
// matches.  (Where a match is that of several paths, they bind the captures
// alike: the walk finds the same boundaries for each.)
//
// The steps forward are found between consecutive marks of the captures (the
// segments), on all paths together: where the items of the segment have the
// same fixed length on every path, one step of that length; otherwise, a
// step of the length of the fixed positions with which the segment begins on
// every path, then a run of the class of all the characters of the rest of
// the segment on every path (empty on some), which must be maximal on every
// path (not followed within the match by a character of the class).  As
//      (?|(a)(b+)|(u\x{308})(c+))      1, a run of [\x{308}] | a run of [bc]
//
constexpr size_t MaxWalkPaths = 256;

bool expandPaths(RE * r, const std::map<std::string, unsigned> & wanted, bool all,
                 std::vector<std::vector<WalkItem>> & paths, RE *& construct, std::string & reason) {
    auto append = [&](const WalkItem & item) {
        for (std::vector<WalkItem> & p : paths) p.push_back(item);
    };
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) {
            if (!expandPaths(e, wanted, all, paths, construct, reason)) return false;
        }
        return true;
    }
    if (String * const s = dyn_cast<String>(r)) {
        for (const char32_t c : *s) append(WalkItem{makeCC(c), -1});
        return true;
    }
    if (Alt * const alt = dyn_cast<Alt>(r)) {
        if (alt->size() == 1) return expandPaths(alt->front(), wanted, all, paths, construct, reason);
        if ((alt->size() > 1) && (all || containsCapture(r, wanted))) {
            std::vector<std::vector<WalkItem>> result;
            for (RE * e : *alt) {
                std::vector<std::vector<WalkItem>> through = paths;
                if (!expandPaths(e, wanted, all, through, construct, reason)) return false;
                if (result.size() + through.size() > MaxWalkPaths) {
                    construct = r;
                    reason = "the search pattern has too many paths through its alternations binding captures "
                             "(more than " + std::to_string(MaxWalkPaths) + ") for the walk of its captures";
                    return false;
                }
                for (std::vector<WalkItem> & p : through) result.push_back(std::move(p));
            }
            paths = std::move(result);
            return true;
        }
    }
    if (!containsCapture(r, wanted)) {
        append(WalkItem{r, -1});
        return true;
    }
    if (Capture * const c = dyn_cast<Capture>(r)) {
        const auto f = wanted.find(c->getName());
        if (f != wanted.end()) append(WalkItem{c, static_cast<int>(2 * f->second)});
        if (!expandPaths(c->getCapturedRE(), wanted, all, paths, construct, reason)) return false;
        if (f != wanted.end()) append(WalkItem{c, static_cast<int>(2 * f->second + 1)});
        return true;
    } else if (Group * const g = dyn_cast<Group>(r)) {
        return expandPaths(g->getRE(), wanted, all, paths, construct, reason);
    }
    construct = r;
    reason = "a capture within a repetition or an assertion has no unique binding in a match";
    return false;
}

// The characters at each position of an item of fixed length, if known.
bool fixedPositions(RE * r, std::vector<UCD::UnicodeSet> & positions) {
    if (CC * const cc = unicodeClass(r)) {
        positions.push_back(UCD::UnicodeSet() + *cc);
        return true;
    }
    if (zeroWidthItem(r)) return true;
    if (Seq * const seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) {
            if (!fixedPositions(e, positions)) return false;
        }
        return true;
    } else if (String * const s = dyn_cast<String>(r)) {
        for (const char32_t c : *s) {
            positions.emplace_back();
            positions.back().insert(c);
        }
        return true;
    } else if (Alt * const alt = dyn_cast<Alt>(r)) {
        if (alt->empty()) return false;
        std::vector<UCD::UnicodeSet> all;
        bool first = true;
        for (RE * e : *alt) {
            std::vector<UCD::UnicodeSet> p;
            if (!fixedPositions(e, p)) return false;
            if (first) {
                all = std::move(p);
                first = false;
            } else if (p.size() != all.size()) {
                return false;
            } else {
                for (size_t k = 0; k < p.size(); k++) all[k] = all[k] + p[k];
            }
        }
        positions.insert(positions.end(), all.begin(), all.end());
        return true;
    } else if (Rep * const rep = dyn_cast<Rep>(r)) {
        if (rep->getLB() != rep->getUB()) return false;
        std::vector<UCD::UnicodeSet> body;
        if (!fixedPositions(rep->getRE(), body)) return false;
        for (int k = 0; k < rep->getLB(); k++) positions.insert(positions.end(), body.begin(), body.end());
        return true;
    } else if (Capture * const c = dyn_cast<Capture>(r)) {
        return fixedPositions(c->getCapturedRE(), positions);
    } else if (Group * const g = dyn_cast<Group>(r)) {
        return fixedPositions(g->getRE(), positions);
    } else if (Name * const n = dyn_cast<Name>(r)) {
        return n->getDefinition() && fixedPositions(n->getDefinition(), positions);
    }
    return false;
}

// The elements of a segment of a path, for the walk: each position of its
// items of fixed length, and each item of variable length, with their
// characters (if known).
struct WalkElement {
    bool position;
    bool known;
    UCD::UnicodeSet chars;
};

std::vector<WalkElement> segmentElements(const std::vector<WalkItem> & items, size_t from, size_t to) {
    std::vector<WalkElement> elements;
    for (size_t i = from; i < to; i++) {
        RE * const r = items[i].re;
        if ((items[i].mark >= 0) || zeroWidthItem(r)) continue;
        const auto range = getLengthRange(r, &cc::Unicode);
        if (range.first == range.second) {
            if (range.first == 0) continue;
            std::vector<UCD::UnicodeSet> positions;
            if (fixedPositions(r, positions) && (positions.size() == static_cast<size_t>(range.first))) {
                for (UCD::UnicodeSet & p : positions) elements.push_back(WalkElement{true, true, std::move(p)});
            } else {
                UCD::UnicodeSet chars;
                const bool known = allChars(r, chars);
                for (int k = 0; k < range.first; k++) elements.push_back(WalkElement{true, known, chars});
            }
        } else {
            UCD::UnicodeSet chars;
            const bool known = allChars(r, chars);
            elements.push_back(WalkElement{false, known, std::move(chars)});
        }
    }
    return elements;
}

// The steps over a segment on all paths together (see above): the segment of
// each path is its items [from, to).  False if none are found.
bool segmentSteps(const std::vector<std::vector<WalkItem>> & paths, const std::vector<size_t> & from,
                  const std::vector<size_t> & to, std::vector<CaptureWalk::Step> & steps) {
    std::vector<std::vector<WalkElement>> elements;
    size_t common = SIZE_MAX;       // the fixed positions with which every segment begins
    bool sameFixed = true;          // all segments of fixed positions only, of the same length
    for (size_t b = 0; b < paths.size(); b++) {
        elements.push_back(segmentElements(paths[b], from[b], to[b]));
        const std::vector<WalkElement> & e = elements.back();
        size_t lead = 0;
        while ((lead < e.size()) && e[lead].position) lead++;
        common = std::min(common, lead);
        sameFixed = sameFixed && (lead == e.size()) && (e.size() == elements.front().size());
    }
    if (sameFixed) {
        const size_t length = elements.front().size();
        if (length > 0) steps.push_back(CaptureWalk::Step{static_cast<int>(length), nullptr});
        return true;
    }
    UCD::UnicodeSet rest;
    for (const std::vector<WalkElement> & e : elements) {
        for (size_t k = common; k < e.size(); k++) {
            if (!e[k].known) return false;
            rest = rest + e[k].chars;
        }
    }
    CC * const X = makeCC(std::move(rest), &cc::Unicode);
    for (size_t b = 0; b < paths.size(); b++) {
        if (!followNotIn(paths[b], to[b], X)) return false;
    }
    if (common > 0) steps.push_back(CaptureWalk::Step{static_cast<int>(common), nullptr});
    steps.push_back(CaptureWalk::Step{0, X});
    return true;
}

bool sameSteps(const std::vector<CaptureWalk::Step> & a, const std::vector<CaptureWalk::Step> & b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); k++) {
        if (a[k].length != b[k].length) return false;
        if ((a[k].run == nullptr) != (b[k].run == nullptr)) return false;
        if (a[k].run && !(static_cast<const UCD::UnicodeSet &>(*a[k].run) == *b[k].run)) return false;
    }
    return true;
}

// The walk for the captures (names, numbered as in wanted) on all the paths
// of a search pattern (see expandPaths): the boundaries of each capture, in
// found (open and close as on the first path).
bool planPathsWalk(const std::vector<std::vector<WalkItem>> & paths, const std::vector<std::string> & names,
                   RE * search, CaptureWalk & walk, std::vector<CaptureWalk::CaptureBoundaries> & found,
                   RE *& construct, std::string & reason, bool backward) {
    const size_t B = paths.size();
    const int marks = static_cast<int>(2 * names.size());
    // The positions of the marks on each path, in their order on the first.
    std::vector<int> order;
    std::vector<std::vector<size_t>> at(B);
    for (size_t b = 0; b < B; b++) {
        std::vector<int> count(marks, 0);
        std::vector<int> seq;
        for (size_t i = 0; i < paths[b].size(); i++) {
            const int m = paths[b][i].mark;
            if (m < 0) continue;
            count[m]++;
            seq.push_back(m);
            at[b].push_back(i);
        }
        for (int j = 0; j < static_cast<int>(names.size()); j++) {
            if (count[2 * j] > 1) {
                construct = search;
                reason = "the capture " + names[j] + " is bound more than once in a match";
                return false;
            }
            if (count[2 * j] == 0) {
                construct = search;
                bool elsewhere = false;
                for (const std::vector<WalkItem> & p : paths) {
                    for (const WalkItem & item : p) elsewhere = elsewhere || (item.mark == 2 * j);
                }
                reason = elsewhere ? "the capture " + names[j] + " is not bound on every path through the "
                                     "alternations of the search pattern: it has no unique binding in a match"
                                   : "the search pattern has no capture " + names[j];
                return false;
            }
        }
        if (b == 0) {
            order = seq;
        } else if (seq != order) {
            construct = search;
            reason = "the captures are not in the same order on every path through the alternations of the "
                     "search pattern";
            return false;
        }
    }
    std::vector<std::vector<int>> suffix(B), back(B);
    std::vector<std::vector<long>> prefixMin(B);
    std::vector<std::vector<CaptureWalk::Step>> backSteps(B);
    for (size_t b = 0; b < B; b++) {
        suffix[b] = fixedSuffixes(paths[b]);
        prefixMin[b].assign(paths[b].size() + 1, 0);
        for (size_t i = 0; i < paths[b].size(); i++) {
            const long lgth = (paths[b][i].mark >= 0) ? 0 : getLengthRange(paths[b][i].re, &cc::Unicode).first;
            prefixMin[b][i + 1] = std::min<long>(prefixMin[b][i] + lgth, INT_MAX);
        }
        if (backward) backWalk(paths[b], back[b], backSteps[b]);
    }
    bool sameBack = backward;
    for (size_t b = 1; sameBack && (b < B); b++) sameBack = sameSteps(backSteps[b], backSteps[0]);
    if (sameBack) walk.backSteps = backSteps[0];
    bool forward = true;
    std::vector<size_t> from(B, 0), to(B, 0);
    for (size_t k = 0; k < order.size(); k++) {
        const unsigned j = order[k] / 2;
        const bool open = (order[k] % 2) == 0;
        for (size_t b = 0; b < B; b++) {
            from[b] = (k == 0) ? 0 : at[b][k - 1] + 1;
            to[b] = at[b][k];
        }
        CaptureWalk::Boundary boundary;
        if (forward) forward = segmentSteps(paths, from, to, walk.steps);
        bool fromEnd = true, walkBack = sameBack;
        long least = LONG_MAX;
        for (size_t b = 0; b < B; b++) {
            const size_t i = at[b][k];
            fromEnd = fromEnd && (suffix[b][i + 1] >= 0) && (suffix[b][i + 1] == suffix[0][at[0][k] + 1]);
            walkBack = walkBack && (back[b][i] >= 0) && (back[b][i] == back[0][at[0][k]]) && (prefixMin[b][i] > 0);
            least = std::min(least, prefixMin[b][i]);
        }
        if (forward) {
            boundary.at = walk.steps.size();
        } else if (fromEnd) {
            boundary.fromEnd = true;
            boundary.at = suffix[0][at[0][k] + 1];
        } else if (walkBack) {
            boundary.backward = true;
            boundary.at = back[0][at[0][k]];
        } else {
            construct = paths[0][at[0][k]].re;
            reason = boundaryNotFound(open, backward) + ", in the same way on every path through the "
                     "alternations of the search pattern";
            return false;
        }
        if (open) {
            found[j].start = boundary;
            found[j].open = at[0][k];
            found[j].prefixMinLength = static_cast<int>(least);
        } else {
            found[j].follow = boundary;
            found[j].close = at[0][k];
        }
    }
    if (!sameBack) walk.backSteps.clear();
    return true;
}

}

std::map<std::string, Capture *> capturesByName(RE * re) {
    std::map<std::string, Capture *> captures;
    CaptureCollector(captures).inspectRE(re);
    return captures;
}

std::map<std::string, std::vector<Capture *>> allCapturesByName(RE * re) {
    std::map<std::string, std::vector<Capture *>> captures;
    AllCaptureCollector(captures).inspectRE(re);
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
    std::map<std::string, std::vector<Capture *>> captures;
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
        // (Folded if every capture of the name, on every path, matches the
        // same string.)
        const auto f = captures.find(r->getName());
        std::u32string s;
        bool single = (f != captures.end()) && singleString(f->second.front()->getCapturedRE(), s);
        for (size_t k = 1; single && (f != captures.end()) && (k < f->second.size()); k++) {
            std::u32string t;
            single = singleString(f->second[k]->getCapturedRE(), t) && (t == s);
        }
        if (single) {
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
    const auto captures = allCapturesByName(search);
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
        // (The range of all the captures of the name.)
        auto range = getLengthRange(f->second.front(), alphabet);
        for (Capture * const c : f->second) {
            const auto r = getLengthRange(c, alphabet);
            range = std::make_pair(std::min(range.first, r.first), std::max(range.second, r.second));
        }
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
                     RE *& construct, std::string & reason, bool backward) {
    walk = CaptureWalk{};
    std::map<std::string, unsigned> wanted;
    std::vector<std::string> names;
    for (const std::string & c : captures) {
        if (wanted.emplace(c, names.size()).second) names.push_back(c);
    }
    std::vector<WalkItem> items;
    std::vector<CaptureWalk::CaptureBoundaries> found(names.size());
    if (!flattenForWalk(search, wanted, items, construct, reason)) {
        // Captures bound within alternations: the walk on all the paths
        // through them (through the other alternations as well, if there are
        // not too many paths).
        if (!isa<Alt>(construct)) return false;
        std::vector<std::vector<WalkItem>> paths(1);
        if (!expandPaths(search, wanted, true, paths, construct, reason)) {
            paths.assign(1, std::vector<WalkItem>{});
            if (!expandPaths(search, wanted, false, paths, construct, reason)) return false;
        }
        if (!planPathsWalk(paths, names, search, walk, found, construct, reason, backward)) return false;
        for (const std::string & c : captures) {
            walk.captures.push_back(found[wanted[c]]);
        }
        return true;
    }
    const size_t n = items.size();
    const std::vector<int> suffix = fixedSuffixes(items);
    // The walk back from the last character of the match: the number of its
    // steps after the items from each marker on (or -1 where it stops).
    std::vector<int> back(n, -1);
    if (backward) backWalk(items, back, walk.backSteps);
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
            } else if ((back[i] >= 0) && (prefixMin > 0)) {
                // (A boundary after a prefix that is not nullable is after a
                // character of the match, see RE_PipelineBuilder::captureBoundaries.)
                b.backward = true;
                b.at = back[i];
            } else {
                construct = r;
                reason = boundaryNotFound(open, backward);
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

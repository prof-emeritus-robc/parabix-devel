/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_plan.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include <re/transforms/re_transformer.h>
#include "charset_analysis.h"

using namespace llvm;

namespace ldml {

//  Does a set have strings (e.g. [{ch}]), or is it something other than a set
//  of single characters?
static bool hasStrings(const re::RE * re) {
    if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        return seq->size() != 1 || hasStrings(seq->front());
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * a : *alt) {
            if (hasStrings(a)) return true;
        }
        return false;
    } else if (const re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return true;
        return hasStrings(name->getDefinition());
    } else if (const re::Diff * diff = dyn_cast<re::Diff>(re)) {
        return hasStrings(diff->getLH()) || hasStrings(diff->getRH());
    } else if (const re::Intersect * x = dyn_cast<re::Intersect>(re)) {
        return hasStrings(x->getLH()) || hasStrings(x->getRH());
    }
    return isa<re::Capture>(re) || isa<re::Reference>(re) || isa<re::Rep>(re);
}

//  The fixed string of a result (characters, or variables defined as strings).
static bool fixedString(const re::RE * re, std::u32string & out) {
    if (const re::CC * cc = dyn_cast<re::CC>(re)) {
        if (cc->count() != 1) return false;
        out.push_back(static_cast<char32_t>(cc->min_codepoint()));
        return true;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) {
            if (!fixedString(e, out)) return false;
        }
        return true;
    } else if (const re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return false;
        return fixedString(name->getDefinition(), out);
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        return alt->size() == 1 && fixedString(alt->front(), out);
    }
    return false;
}

//  The items of a text to replace, through sequences, variables defined as
//  strings and captures: e.g., a $v [bc] with $v = xy is a, x, y, [bc].
static void textItems(re::RE * re, std::vector<re::RE *> & items) {
    if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (re::RE * e : *seq) textItems(e, items);
    } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
        textItems(c->getCapturedRE(), items);
    } else if (re::Alt * alt = dyn_cast<re::Alt>(re); alt && alt->size() == 1) {
        textItems(alt->front(), items);
    } else if (re::Name * name = dyn_cast<re::Name>(re);
               name && !isFunctionCall(name) && name->getDefinition() && !CharSetAnalysis::isSet(name->getDefinition())) {
        textItems(name->getDefinition(), items);
    } else {
        items.push_back(re);
    }
}

//  A set without strings (through variables), not including the text boundary.
static bool plainSet(const re::RE * re) {
    return CharSetAnalysis::isSet(re) && !hasStrings(re) && !mayIncludeTextBoundary(re);
}

//  The last item of a text, if it is an unbounded repetition: the items it
//  matches first (if any) and the set of its run.  ICU matches it
//  possessively, taking the longest run: S+ is S then a run of S, S* a run of
//  S, and (S T*)+ (as in ($alif) $alif+ with $alif = [أإآا] [:M:]*) is S then
//  a run of the characters of S and T.
static bool runItem(re::RE * re, std::vector<re::RE *> & first, UCD::UnicodeSet & run, CharSetAnalysis & sets) {
    if (re::Name * n = dyn_cast<re::Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && runItem(n->getDefinition(), first, run, sets);
    }
    re::Rep * const rep = dyn_cast<re::Rep>(re);
    if (rep == nullptr || rep->getUB() != re::Rep::UNBOUNDED_REP || rep->getLB() > 1) return false;
    re::RE * body = rep->getRE();
    while (re::Name * n = dyn_cast<re::Name>(body)) {
        if (isFunctionCall(n) || n->getDefinition() == nullptr || CharSetAnalysis::isSet(n->getDefinition())) break;
        body = n->getDefinition();
    }
    if (plainSet(body)) {
        if (rep->getLB() == 1) first.push_back(body);
        run = sets.setOf(body, false);
        return true;
    }
    const re::Seq * seq = dyn_cast<re::Seq>(body);
    if (rep->getLB() == 1 && seq && seq->size() == 2 && plainSet(seq->front())) {
        const re::Rep * star = dyn_cast<re::Rep>(seq->back());
        if (star && star->getLB() == 0 && star->getUB() == re::Rep::UNBOUNDED_REP && plainSet(star->getRE())) {
            first.push_back(seq->front());
            run = sets.setOf(seq->front(), false) + sets.setOf(star->getRE(), false);
            return true;
        }
    }
    return false;
}

static bool isEmptyText(const re::RE * re);

//  Whether an after context, matched by the regular expression engine as a
//  lookahead, has an unbounded repetition of something other than a set of
//  single characters (such as a set with strings, [{m̥}m]*): the engine does
//  not support such variable-length lookaheads (their bodies must have a unique
//  prefix, or be chains of repeated sets).
static bool unsupportedLookahead(const re::RE * re) {
    if (re == nullptr) return false;
    if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        //  (A repeated set may be captured, as in ([ab])*.)
        const re::RE * body = rep->getRE();
        while (const re::Capture * c = dyn_cast<re::Capture>(body)) body = c->getCapturedRE();
        if (rep->getUB() == re::Rep::UNBOUNDED_REP && !(CharSetAnalysis::isSet(body) && !hasStrings(body))) return true;
        return unsupportedLookahead(rep->getRE());
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) if (unsupportedLookahead(e)) return true;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * e : *alt) if (unsupportedLookahead(e)) return true;
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
            return unsupportedLookahead(n->getDefinition());
        }
    } else if (const re::Capture * c = dyn_cast<re::Capture>(re)) {
        return unsupportedLookahead(c->getCapturedRE());
    }
    return false;
}

//  The characters a pattern may match (false if not known).
static bool matchableChars(const re::RE * re, UCD::UnicodeSet & chars, CharSetAnalysis & sets) {
    if (CharSetAnalysis::isSet(re)) {
        chars = chars + sets.setOf(re, true);
        return true;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) if (!matchableChars(e, chars, sets)) return false;
        return true;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * e : *alt) if (!matchableChars(e, chars, sets)) return false;
        return true;
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        return matchableChars(rep->getRE(), chars, sets);
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && matchableChars(n->getDefinition(), chars, sets);
    }
    return false;
}

//  A pattern that ends with a set of characters (no strings), one character or
//  an unbounded run of them (at least one): the set, and the characters that
//  may come before it in a match (added to before).
static bool finalItem(const re::RE * re, UCD::UnicodeSet & last, UCD::UnicodeSet & before, CharSetAnalysis & sets) {
    if (plainSet(re)) {
        last = sets.setOf(re, false);
        return true;
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        if (!plainSet(rep->getRE()) || rep->getLB() < 1 || rep->getUB() != re::Rep::UNBOUNDED_REP) return false;
        last = sets.setOf(rep->getRE(), false);
        return true;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        if (seq->empty() || !finalItem(seq->back(), last, before, sets)) return false;
        for (auto i = seq->begin(); i + 1 != seq->end(); ++i) {
            if (!matchableChars(*i, before, sets)) return false;
        }
        return true;
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && finalItem(n->getDefinition(), last, before, sets);
    }
    return false;
}

//  A pattern that begins with a set of characters (no strings): the set, and
//  the characters that may follow it in a match (added to rest).
static bool leadingSet(const re::RE * re, UCD::UnicodeSet & first, UCD::UnicodeSet & rest, CharSetAnalysis & sets) {
    if (plainSet(re)) {
        first = sets.setOf(re, false);
        return true;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        if (seq->empty() || !leadingSet(seq->front(), first, rest, sets)) return false;
        for (auto i = std::next(seq->begin()); i != seq->end(); ++i) {
            if (!matchableChars(*i, rest, sets)) return false;
        }
        return true;
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && leadingSet(n->getDefinition(), first, rest, sets);
    }
    return false;
}

static bool containsCapture(const re::RE * re) {
    if (isa<re::Capture>(re) || isa<re::Reference>(re)) return true;
    if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) if (containsCapture(e)) return true;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * e : *alt) if (containsCapture(e)) return true;
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        return containsCapture(rep->getRE());
    }
    return false;
}

//  The minimum and maximum lengths of a pattern (max UINT_MAX if unbounded;
//  false if not known).
static bool lengthRange(const re::RE * re, unsigned & min, unsigned & max) {
    constexpr unsigned Unbounded = UINT_MAX;
    auto add = [](unsigned a, unsigned b) {return (a == Unbounded || b == Unbounded) ? Unbounded : a + b;};
    if (CharSetAnalysis::isSet(re)) {
        if (!hasStrings(re)) {
            min = max = 1;
            return true;
        }
        //  A set with strings: from the shortest member to the longest.
        std::function<bool (const re::RE *, unsigned &, unsigned &)> members = [&](const re::RE * e, unsigned & lo, unsigned & hi) {
            if (const re::Alt * alt = dyn_cast<re::Alt>(e)) {
                for (const re::RE * a : *alt) {
                    if (isBoundary(a)) continue;
                    if (!members(a, lo, hi)) return false;
                }
                return true;
            } else if (const re::Name * n = dyn_cast<re::Name>(e)) {
                return !isFunctionCall(n) && n->getDefinition() && members(n->getDefinition(), lo, hi);
            } else if (const re::Seq * seq = dyn_cast<re::Seq>(e)) {
                std::u32string str;
                if (!fixedString(seq, str)) return false;
                lo = std::min<unsigned>(lo, str.size());
                hi = std::max<unsigned>(hi, str.size());
                return true;
            } else if (CharSetAnalysis::isSetExpression(e)) {
                lo = std::min(lo, 1u);
                hi = std::max(hi, 1u);
                return true;
            }
            return false;
        };
        min = Unbounded;
        max = 0;
        return members(re, min, max) && max > 0;
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        min = max = 0;
        for (const re::RE * e : *seq) {
            unsigned a, b;
            if (!lengthRange(e, a, b)) return false;
            min += a;
            max = add(max, b);
        }
        return true;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        min = Unbounded;
        max = 0;
        for (const re::RE * e : *alt) {
            unsigned a, b;
            if (!lengthRange(e, a, b)) return false;
            min = std::min(min, a);
            max = std::max(max, b);
        }
        return !alt->empty();
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        unsigned a, b;
        if (!lengthRange(rep->getRE(), a, b)) return false;
        min = a * static_cast<unsigned>(rep->getLB());
        max = (rep->getUB() == re::Rep::UNBOUNDED_REP || b == Unbounded) ? Unbounded : b * static_cast<unsigned>(rep->getUB());
        return true;
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && lengthRange(n->getDefinition(), min, max);
    }
    return false;
}

//  The first item of a context (after), as a set of characters, if it is one.
static bool firstContextSet(re::RE * context, UCD::UnicodeSet & chars, CharSetAnalysis & sets) {
    re::RE * first = context;
    while (true) {
        if (re::Seq * seq = dyn_cast<re::Seq>(first)) {
            if (seq->empty()) return false;
            first = seq->front();
        } else if (re::Name * n = dyn_cast<re::Name>(first);
                   n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
            first = n->getDefinition();
        } else {
            break;
        }
    }
    if (!CharSetAnalysis::isSet(first) || hasStrings(first)) return false;
    chars = sets.setOf(first, false);
    return true;
}

//  A MultipleCaptureMultipleReplacement rule (see
//  MultipleCaptureMultipleReplacementSet): the rule, and its set (without
//  rules), or false (with the reason, if the rule has the form).
static bool multipleCaptureMultipleReplacement(const ConversionRule * c, MultipleCaptureMultipleReplacement & rule,
                                               MultipleCaptureMultipleReplacementSet & set, CharSetAnalysis & sets,
                                               std::string & reason) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (result->hasCursor() && (!isEmptyText(result->getResultToRevisit()) || result->getCursorOffset() != 0)) return false;
    std::vector<re::RE *> items;
    std::function<void (re::RE *)> flatten = [&](re::RE * re) {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) flatten(e);
        } else if (re::Name * n = dyn_cast<re::Name>(re);
                   n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())
                   && !isa<re::Rep>(n->getDefinition())) {
            flatten(n->getDefinition());
        } else {
            items.push_back(re);
        }
    };
    flatten(source->getText());
    //  The text: T0 (B1) T1 ... (Bk) Tk.
    std::vector<std::vector<UCD::UnicodeSet>> texts(1);
    std::vector<const re::Capture *> captures;
    for (re::RE * item : items) {
        if (const re::Capture * cap = dyn_cast<re::Capture>(item)) {
            if (containsCapture(cap->getCapturedRE())) return false;
            captures.push_back(cap);
            texts.emplace_back();
        } else if (plainSet(item)) {
            texts.back().push_back(sets.setOf(item, false));
        } else {
            return false;
        }
    }
    const size_t k = captures.size();
    if (k == 0) return false;
    //  The result: R0 $1 R1 ... $k Rk.
    std::vector<re::RE *> out;
    std::function<void (re::RE *)> flattenResult = [&](re::RE * re) {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) flattenResult(e);
        } else {
            out.push_back(re);
        }
    };
    flattenResult(result->getText());
    std::vector<std::u32string> results(1);
    std::vector<size_t> order;              // the captures referred to, in order
    for (re::RE * item : out) {
        if (const re::Reference * r = dyn_cast<re::Reference>(item)) {
            size_t i = 0;
            while (i < k && r->getName() != captures[i]->getName()) i++;
            if (i == k) return false;
            order.push_back(i);
            results.emplace_back();
        } else {
            std::u32string str;
            if (!fixedString(item, str)) return false;
            results.back() += str;
        }
    }
    if (results.size() != k + 1) return false;
    //  Captures referred to in order, but for one capture j referred to before
    //  captures i to j - 1 (see MultipleCaptureMultipleReplacementSet::moved):
    //  planned in order, the text Q_i before $j and Q_(i+1) between $j and $i
    //  before capture i, the texts within the region before their captures, and
    //  no text between capture j - 1 and j.  (0-based captures; Q_t is the text
    //  before the t-th reference.)
    bool moved = false;
    size_t mi = 0, mj = 0;
    {
        size_t m = 0;
        while (m < order.size() && order[m] == m) m++;
        if (m < order.size()) {
            const size_t j = order[m];
            std::vector<size_t> expected;
            for (size_t c = 0; c < m; c++) expected.push_back(c);
            expected.push_back(j);
            for (size_t c = m; c < order.size(); c++) if (c != j) expected.push_back(c);
            if (j <= m || order != expected) return false;
            moved = true;
            mi = m;
            mj = j;
            std::vector<std::u32string> inOrder(k + 1);
            for (size_t c = 0; c < mi; c++) inOrder[c] = results[c];
            inOrder[mi] = results[mi] + results[mi + 1];
            for (size_t c = mi + 1; c < mj; c++) inOrder[c] = results[c + 1];
            inOrder[mj] = std::u32string();
            for (size_t c = mj + 1; c <= k; c++) inOrder[c] = results[c];
            set.swapBefore = static_cast<unsigned>(results[mi + 1].size());
            for (size_t c = mi + 1; c <= mj; c++) {
                for (char32_t ch : results[c]) set.swapChars.insert(ch);
            }
            results = inOrder;
        }
    }
    //  The lengths and characters of the captures.
    constexpr unsigned Unbounded = UINT_MAX;
    std::vector<unsigned> minLength(k + 1), maxLength(k + 1);
    std::vector<UCD::UnicodeSet> captureChars(k + 1);
    UCD::UnicodeSet all;                    // the characters that may occur in a match
    for (size_t i = 1; i <= k; i++) {
        if (!lengthRange(captures[i - 1]->getCapturedRE(), minLength[i], maxLength[i])
                || !matchableChars(captures[i - 1]->getCapturedRE(), captureChars[i], sets)) {
            reason = "a capture has a pattern whose length or characters are not known";
            return false;
        }
        all = all + captureChars[i];
    }
    for (const auto & t : texts) for (const UCD::UnicodeSet & chars : t) all = all + chars;
    if (moved) {
        if (minLength[mi + 1] == 0 || minLength[mj + 1] == 0) {
            reason = "it refers to its captures out of order, of which one moved may be empty";
            return false;
        }
        for (size_t c = mi + 1; c <= mj + 1; c++) set.swapChars = set.swapChars + captureChars[c];
        if (k == 2 && minLength[1] == maxLength[1] && minLength[2] == maxLength[2]) {
            //  Two captures of fixed lengths: by SwapBack.
            set.reversed = true;
            set.swapLength = minLength[2];
            set.swapDistance = set.swapBefore + minLength[1];
        } else {
            set.moved = true;
            set.moveFirst = static_cast<unsigned>(mi);
            set.moveCapture = static_cast<unsigned>(mj);
        }
    }
    const std::vector<UCD::UnicodeSet> & first = texts.front();
    const std::vector<UCD::UnicodeSet> & last = texts.back();
    if (first.empty() && minLength[1] == 0) {
        reason = "the text may begin with an empty capture";
        return false;
    }
    //  How the matches begin: at the characters of T0[0] (which may occur nowhere
    //  else in a match), or after the end of the previous match (which may not
    //  end within another).
    UCD::UnicodeSet inside;
    for (size_t i = 0; i < texts.size(); i++) {
        for (size_t j = (i == 0 ? 1 : 0); j < texts[i].size(); j++) inside = inside + texts[i][j];
    }
    for (size_t i = 2; i <= k; i++) inside = inside + captureChars[i];
    if (!first.empty()) {
        set.startChars = first.front();
        set.startsByFirst = !set.startChars.intersects(inside + captureChars[1]);
    } else {
        //  With T0 empty, a match begins with B1: by the first characters of B1,
        //  if it begins with a set (as with a fixed literal).
        UCD::UnicodeSet rest;
        if (leadingSet(captures[0]->getCapturedRE(), set.startChars, rest, sets)) {
            set.startsByFirst = !set.startChars.intersects(inside + rest);
        }
    }
    unsigned totalMin = 0, totalMax = 0;
    for (const auto & t : texts) {
        totalMin += static_cast<unsigned>(t.size());
        totalMax += static_cast<unsigned>(t.size());
    }
    for (size_t i = 1; i <= k; i++) {
        totalMin += minLength[i];
        totalMax = (maxLength[i] == Unbounded || totalMax == Unbounded) ? Unbounded : totalMax + maxLength[i];
    }
    set.singleCharacter = !set.startsByFirst && totalMin == 1 && totalMax == 1;
    if (!set.startsByFirst && !set.singleCharacter) {
        bool endsUnique = false;
        if (!last.empty()) {
            UCD::UnicodeSet before;
            for (size_t i = 0; i < texts.size(); i++) {
                for (size_t j = 0; j < texts[i].size(); j++) {
                    if (i + 1 == texts.size() && j + 1 == texts[i].size()) continue;
                    before = before + texts[i][j];
                }
            }
            for (size_t i = 1; i <= k; i++) before = before + captureChars[i];
            endsUnique = !last.back().intersects(before);
            //  More precisely, Tk may not occur ending within a match: overlapping
            //  itself (d positions before its end, d < |Tk|), or within the part of
            //  the match before it.
            if (!endsUnique) {
                UCD::UnicodeSet earlier;
                for (size_t i = 0; i + 1 < texts.size(); i++) for (const UCD::UnicodeSet & chars : texts[i]) earlier = earlier + chars;
                for (size_t i = 1; i <= k; i++) earlier = earlier + captureChars[i];
                const size_t m = last.size();
                auto mayEndAt = [&](size_t d) {
                    for (size_t i = 0; i < m; i++) {
                        const UCD::UnicodeSet & actual = d + i < m ? last[m - 1 - d - i] : earlier;
                        if (!last[m - 1 - i].intersects(actual)) return false;
                    }
                    return true;
                };
                endsUnique = true;
                for (size_t d = 1; d <= m && endsUnique; d++) endsUnique = !mayEndAt(d);
            }
        }
        //  With Tk empty, the last capture ending with a set S, one character or a
        //  run of them (possessive: a match ends with the run), S not occurring
        //  before in a match.
        if (!endsUnique && last.empty()) {
            UCD::UnicodeSet finalSet, before;
            if (finalItem(captures[k - 1]->getCapturedRE(), finalSet, before, sets)) {
                for (const auto & t : texts) for (const UCD::UnicodeSet & chars : t) before = before + chars;
                for (size_t i = 1; i < k; i++) before = before + captureChars[i];
                endsUnique = !finalSet.intersects(before);
            }
        }
        UCD::UnicodeSet next;
        if (!endsUnique && source->getAfterContext() && firstContextSet(source->getAfterContext(), next, sets)) {
            endsUnique = !next.intersects(all);
        }
        if (!endsUnique) {
            reason = "the starts of its matches cannot be told apart (a match may end within another)";
            return false;
        }
    }
    //  The replacements at the boundaries.  A boundary i (at Ti) is at a fixed
    //  offset from the start if the captures before it have fixed lengths, or
    //  from the end if those after it do.
    auto fixed = [&](size_t i) {return minLength[i] == maxLength[i];};
    std::vector<BoundaryReplacement> replacements;
    for (size_t i = 0; i <= k; i++) {
        if (i == 0 && !first.empty()) continue;         // T0 and R0: given by the rule
        const std::vector<UCD::UnicodeSet> & t = texts[i];
        const std::u32string & r = results[i];
        if (t.empty() && r.empty()) continue;
        //  The position b of the first character of Ti (or after the boundary),
        //  from the start, and the number of positions from it to the end of the match.
        bool fromStart = true, fromEnd = true;
        unsigned b = static_cast<unsigned>(first.size());
        for (size_t j = 1; j <= i; j++) {
            fromStart &= fixed(j);
            b += minLength[j] + (j < i ? static_cast<unsigned>(texts[j].size()) : 0);
        }
        unsigned after = 0;                             // the length after Ti
        for (size_t j = i + 1; j <= k; j++) {
            fromEnd &= fixed(j);
            after += minLength[j] + static_cast<unsigned>(texts[j].size());
        }
        BoundaryReplacement br;
        br.replacement = r;
        if (!t.empty()) {
            br.kind = BoundaryReplacement::Kind::Replace;
            br.text = t;
            if (fromStart) {
                br.offset = b;
            } else if (fromEnd) {
                br.fromEnd = true;
                br.offset = static_cast<unsigned>(t.size()) - 1 + after;
            } else {
                reason = "a replaced text lies between captures of variable length";
                return false;
            }
        } else {
            //  An insertion: after the last character of Bi, or before the first
            //  character of Bi+1.
            br.kind = BoundaryReplacement::Kind::Keep;
            if (i > 0 && minLength[i] >= 1 && ((fromStart && b >= 1) || fromEnd)) {
                br.keepAt = 0;
                br.capture = static_cast<unsigned>(i);
                br.text = {captureChars[i]};
                if (fromStart) br.offset = b - 1; else {br.fromEnd = true; br.offset = after;}
            } else if (i < k && minLength[i + 1] >= 1 && (fromStart || (fromEnd && after >= 1))) {
                br.keepAt = static_cast<unsigned>(r.size());
                br.capture = static_cast<unsigned>(i + 1);
                br.text = {captureChars[i + 1]};
                if (fromStart) br.offset = b; else {br.fromEnd = true; br.offset = after - 1;}
            } else {
                reason = "an insertion is not next to a capture that is never empty, at a fixed offset";
                return false;
            }
        }
        //  Two insertions around the same character are combined.
        bool merged = false;
        for (BoundaryReplacement & o : replacements) {
            if (o.fromEnd != br.fromEnd || o.offset != br.offset) continue;
            if (o.kind == BoundaryReplacement::Kind::Keep && br.kind == BoundaryReplacement::Kind::Keep
                    && o.capture == br.capture && o.keepAt == o.replacement.size() && br.keepAt == 0) {
                o.text.front() = o.text.front() & br.text.front();
                o.replacement += br.replacement;
                merged = true;
            } else {
                reason = "two replacements at the same position";
                return false;
            }
        }
        if (!merged) replacements.push_back(std::move(br));
    }
    for (const re::Capture * cap : captures) set.captures.push_back(cap->getCapturedRE());
    set.texts.assign(texts.begin() + 1, texts.end());
    set.replacements = std::move(replacements);
    set.results.assign(results.begin() + 1, results.end());
    set.before = source->getBeforeContext();
    set.after = source->getAfterContext();
    set.engineBefore = set.before ? engineContext(set.before, false) : nullptr;
    set.engineAfter = set.after ? engineContext(set.after, true) : nullptr;
    rule.first = first;
    rule.firstReplacement = results.front();
    rule.rule = c;
    return true;
}

//  The key of a MultipleCaptureMultipleReplacement set: the rules of a set
//  differ only in T0 and R0 (with T0 not empty, of the same length).
static std::string captureReplacementKey(const MultipleCaptureMultipleReplacementSet & set,
                                         const MultipleCaptureMultipleReplacement & rule) {
    auto appendSet = [](std::string & key, const UCD::UnicodeSet & chars) {
        for (const auto & range : chars) key += " " + std::to_string(range.first) + "-" + std::to_string(range.second);
        key += ";";
    };
    std::string key = (set.before ? printPattern(set.before) : "") + " | " + (set.after ? printPattern(set.after) : "")
                      + " | " + std::to_string(rule.first.size()) + " |";
    if (rule.first.empty()) {
        for (char32_t ch : rule.firstReplacement) key += " " + std::to_string(static_cast<unsigned>(ch));
    }
    for (const re::RE * b : set.captures) key += " (" + printPattern(b) + ")";
    for (const auto & t : set.texts) {
        key += " T";
        for (const UCD::UnicodeSet & chars : t) appendSet(key, chars);
    }
    for (const BoundaryReplacement & br : set.replacements) {
        key += " R" + std::to_string(static_cast<int>(br.kind)) + (br.fromEnd ? "e" : "s") + std::to_string(br.offset)
               + "k" + std::to_string(br.keepAt) + ":";
        for (char32_t ch : br.replacement) key += " " + std::to_string(static_cast<unsigned>(ch));
        for (const UCD::UnicodeSet & chars : br.text) appendSet(key, chars);
    }
    key += set.startsByFirst ? " first" : (set.singleCharacter ? " single" : " ends");
    if (set.reversed) {
        key += " swap" + std::to_string(set.swapLength) + "/" + std::to_string(set.swapDistance) + "/" + std::to_string(set.swapBefore);
    }
    if (set.moved) {
        key += " move" + std::to_string(set.moveFirst) + "/" + std::to_string(set.moveCapture) + "/" + std::to_string(set.swapBefore);
    }
    return key;
}

static bool isEmptyText(const re::RE * re) {
    const re::Seq * seq = dyn_cast_or_null<re::Seq>(re);
    return re == nullptr || (seq && seq->empty());
}

//  A context as a single item (through sequences of one item and variables
//  not defined as sets), or nullptr.
static const re::RE * singleContextItem(const re::RE * context) {
    while (true) {
        if (const re::Seq * seq = dyn_cast_or_null<re::Seq>(context)) {
            if (seq->size() != 1) return nullptr;
            context = seq->front();
        } else if (const re::Name * n = dyn_cast_or_null<re::Name>(context);
                   n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
            context = n->getDefinition();
        } else {
            return context;
        }
    }
}

//  The characters with which a context may end (adding them to last): its last
//  item, a set, or a repetition of a set (with the characters before it, if it
//  may be empty).  False if not known.
static bool lastContextChars(const re::RE * context, UCD::UnicodeSet & last, CharSetAnalysis & sets) {
    if (const re::Name * n = dyn_cast_or_null<re::Name>(context);
            n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
        return lastContextChars(n->getDefinition(), last, sets);
    }
    if (const re::Seq * seq = dyn_cast_or_null<re::Seq>(context)) {
        for (auto i = seq->rbegin(); i != seq->rend(); ++i) {
            bool mayBeEmpty = false;
            if (const re::Rep * rep = dyn_cast<re::Rep>(*i)) {
                if (!plainSet(rep->getRE())) return false;
                last = last + sets.setOf(rep->getRE(), false);
                mayBeEmpty = rep->getLB() == 0;
            } else if (!lastContextChars(*i, last, sets)) {
                return false;
            }
            if (!mayBeEmpty) return true;
        }
        return false;       // (the context may be empty)
    }
    if (context == nullptr || !plainSet(context)) return false;
    last = last + sets.setOf(context, false);
    return true;
}

//  The first item of a context (as for firstContextSet), or nullptr.
static const re::RE * firstContextItem(const re::RE * context) {
    while (true) {
        if (const re::Seq * seq = dyn_cast_or_null<re::Seq>(context)) {
            if (seq->empty()) return nullptr;
            context = seq->front();
        } else if (const re::Name * n = dyn_cast_or_null<re::Name>(context);
                   n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
            context = n->getDefinition();
        } else {
            return context;
        }
    }
}

//  Whether every set of a context contains all of the characters given or none.
static bool uniformContext(const re::RE * re, const UCD::UnicodeSet & chars, CharSetAnalysis & sets) {
    if (re == nullptr) return true;
    if (CharSetAnalysis::isSet(re)) {
        const UCD::UnicodeSet s = sets.setOf(re, true);
        return !s.intersects(chars) || chars.subset(s);
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (const re::RE * e : *seq) if (!uniformContext(e, chars, sets)) return false;
        return true;
    } else if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * e : *alt) if (!uniformContext(e, chars, sets)) return false;
        return true;
    } else if (const re::Rep * rep = dyn_cast<re::Rep>(re)) {
        return uniformContext(rep->getRE(), chars, sets);
    } else if (const re::Capture * c = dyn_cast<re::Capture>(re)) {
        return uniformContext(c->getCapturedRE(), chars, sets);
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        if (isTextBoundary(n)) return true;
        return !isFunctionCall(n) && n->getDefinition() && uniformContext(n->getDefinition(), chars, sets);
    }
    return false;
}

namespace {
//  A context without its captures (to which a fixed result does not refer): the
//  regular expression engine does not support captures in lookaheads.
class StripCaptures final : public re::RE_Transformer {
public:
    StripCaptures() : re::RE_Transformer("StripCaptures") {}
    re::RE * transformCapture(re::Capture * c) override {return transform(c->getCapturedRE());}
};
}

//  An insertion rule (see InsertionRule), or false with the reason.
static bool insertionRule(const ConversionRule * c, InsertionRule & ins, CharSetAnalysis & sets, std::string & reason) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (result->hasCursor() && (!isEmptyText(result->getResultToRevisit()) || result->getCursorOffset() != 0)) {
        reason = "the result has text to revisit";
        return false;
    }
    if (!fixedString(result->getText(), ins.replacement) || ins.replacement.empty()) {
        reason = "the text inserted is not a fixed string";
        return false;
    }
    re::RE * const before = source->getBeforeContext();
    if (!lastContextChars(before, ins.before, sets)) {
        reason = "its before context does not end with a character of a set";
        return false;
    }
    const re::RE * const single = singleContextItem(before);
    ins.singleBefore = single && plainSet(single);
    ins.engineBefore = engineContext(before, false);
    re::RE * const after = source->getAfterContext();
    const re::RE * first = firstContextItem(after);
    //  (Or with a repetition of a set, at least once: its first character.)
    if (const re::Rep * rep = dyn_cast_or_null<re::Rep>(first); rep && rep->getLB() >= 1) first = rep->getRE();
    if (first == nullptr || !plainSet(first)) {
        reason = "its after context does not begin with a character of a set";
        return false;
    }
    ins.afterFirst = sets.setOf(first, false);
    ins.after = after;
    ins.engineAfter = engineContext(StripCaptures().transformRE(after), true);
    ins.rule = c;
    return true;
}

//  A BeforeCaptureCopy rule (see DeferredRule), P (C) { T } A → R $1 ; with a
//  text of single characters: the characters of the text, R, the capture C, and
//  the before context with C replaced by its pattern.
static bool beforeCaptureCopy(const ConversionRule * c, CharSetAnalysis & sets, std::vector<UCD::UnicodeSet> & text,
                              std::u32string & replacement, re::Capture *& capture, re::RE *& before) {
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    if (source->getBeforeContext() == nullptr) return false;
    if (result->hasCursor() && (!isEmptyText(result->getResultToRevisit()) || result->getCursorOffset() != 0)) return false;
    if (containsCapture(source->getText()) || (source->getAfterContext() && containsCapture(source->getAfterContext()))) return false;
    //  The before context ends with the capture.
    std::vector<re::RE *> items;
    std::function<void (re::RE *)> flatten = [&](re::RE * re) {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) flatten(e);
        } else {
            items.push_back(re);
        }
    };
    flatten(source->getBeforeContext());
    if (items.empty() || !isa<re::Capture>(items.back())) return false;
    capture = cast<re::Capture>(items.back());
    if (containsCapture(capture->getCapturedRE())) return false;
    for (size_t i = 0; i + 1 < items.size(); i++) if (containsCapture(items[i])) return false;
    //  The result: R $1.
    std::vector<re::RE *> out;
    std::function<void (re::RE *)> flattenResult = [&](re::RE * re) {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) flattenResult(e);
        } else {
            out.push_back(re);
        }
    };
    flattenResult(result->getText());
    if (out.empty()) return false;
    const re::Reference * ref = dyn_cast<re::Reference>(out.back());
    if (ref == nullptr || ref->getName() != capture->getName()) return false;
    replacement.clear();
    for (size_t i = 0; i + 1 < out.size(); i++) if (!fixedString(out[i], replacement)) return false;
    std::vector<re::RE *> textItemList;
    textItems(source->getText(), textItemList);
    if (textItemList.empty()) return false;
    text.clear();
    for (re::RE * item : textItemList) {
        if (!plainSet(item)) return false;
        text.push_back(sets.setOf(item, false));
    }
    items.back() = capture->getCapturedRE();
    before = re::makeSeq(items.begin(), items.end());
    return true;
}

//  The shape of a capture copied (see CopyCapture).
static bool copyCaptureShape(re::RE * pattern, CopyCapture & shape, CharSetAnalysis & sets) {
    shape.pattern = pattern;
    std::vector<re::RE *> items;
    textItems(pattern, items);
    if (items.empty()) return false;
    for (re::RE * item : items) {
        while (re::Name * n = dyn_cast<re::Name>(item)) {
            if (isFunctionCall(n) || n->getDefinition() == nullptr || CharSetAnalysis::isSet(n->getDefinition())) break;
            item = n->getDefinition();
        }
        if (plainSet(item)) {
            (shape.star ? shape.suffix : shape.prefix).push_back(sets.setOf(item, false));
        } else if (re::Rep * rep = dyn_cast<re::Rep>(item);
                   rep && !shape.star && rep->getUB() == re::Rep::UNBOUNDED_REP && rep->getLB() <= 1 && plainSet(rep->getRE())) {
            shape.star = true;
            shape.starSet = sets.setOf(rep->getRE(), false);
        } else {
            return false;
        }
    }
    return true;
}

//  Whether the matches of a MultipleCaptureMultipleReplacement set are the same
//  with its captures matched greedily rather than possessively: what follows
//  each repetition (in the captures, the texts after them, or the after
//  context) cannot begin with a character it repeats, so that a match never
//  needs fewer repetitions.
static bool greedyMatchesPossessive(const MultipleCaptureMultipleReplacementSet & set, CharSetAnalysis & sets) {
    std::vector<re::RE *> items;
    std::function<void (re::RE *)> flatten = [&](re::RE * re) {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) flatten(e);
        } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
            flatten(c->getCapturedRE());
        } else if (re::Name * n = dyn_cast<re::Name>(re);
                   n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
            flatten(n->getDefinition());
        } else {
            items.push_back(re);
        }
    };
    for (size_t i = 0; i < set.captures.size(); i++) {
        flatten(set.captures[i]);
        for (const UCD::UnicodeSet & chars : set.texts[i]) items.push_back(re::makeCC(chars, &cc::Unicode));
    }
    if (set.after) flatten(set.after);
    for (size_t i = 0; i < items.size(); i++) {
        const re::Rep * rep = dyn_cast<re::Rep>(items[i]);
        if (rep == nullptr || rep->getLB() == rep->getUB()) continue;
        UCD::UnicodeSet repeated;
        if (!matchableChars(rep->getRE(), repeated, sets)) return false;
        //  The characters with which what follows may begin (through optional items).
        UCD::UnicodeSet next;
        size_t j = i + 1;
        for (; j < items.size(); j++) {
            const re::RE * item = items[j];
            const re::Rep * r = dyn_cast<re::Rep>(item);
            UCD::UnicodeSet chars;
            if (!matchableChars(r ? r->getRE() : item, chars, sets)) {
                if (isa<re::End>(item) || isBoundary(item)) break;
                return false;
            }
            next = next + chars;
            if (!(r && r->getLB() == 0)) break;
        }
        if (next.intersects(repeated)) return false;
    }
    return true;
}

static std::set<std::u32string> setStrings(const re::RE * re);

//  The expansion of a MultipleCaptureMultipleReplacement rule whose captures
//  have finitely many lengths into rules whose captures have fixed lengths (so
//  that its replacements are at fixed offsets): each capture is a sequence of
//  sets, sets with strings and optional items, and the rules are the
//  combinations of the alternatives of each item (an optional item present or
//  absent; a set with strings, each of its strings, then its characters), in
//  the order in which ICU, matching possessively, prefers them (present first,
//  longest first).  The rules are equivalent to the rule as long as a less
//  preferred alternative cannot match where a preferred one matches but the
//  rest of the rule does not: what follows an optional item may not begin with
//  its characters, nor what follows a set with strings with the character
//  extending one of its strings (or characters) into a longer one.
static bool expandFiniteCaptures(ConversionRule * c, CharSetAnalysis & sets, std::vector<Rule *> & variants) {
    constexpr size_t MaxVariants = 64;
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    //  The items of the text, with the captures as items.
    std::vector<re::RE *> textItemList;
    std::function<void (re::RE *)> flattenText = [&](re::RE * re) {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) flattenText(e);
        } else {
            textItemList.push_back(re);
        }
    };
    flattenText(source->getText());
    //  An item of a capture: its alternatives (each a sequence of sets), the
    //  characters it may match, and whether it is variable (with its
    //  constraint on what follows: the characters that may not begin it).
    struct Item {
        std::vector<std::vector<re::RE *>> alts;
        UCD::UnicodeSet first;          // the characters with which it may begin
        UCD::UnicodeSet forbiddenNext;  // the characters that may not follow it
        bool optional = false;
    };
    std::vector<Item> items;            // all the items of the text, in order
    std::vector<size_t> owner;          // the item of the text (index in textItemList) of each item
    auto setItem = [&](re::RE * set, Item & item) -> bool {
        if (!CharSetAnalysis::isSet(set)) return false;
        const UCD::UnicodeSet chars = sets.setOf(set, false);
        const std::set<std::u32string> strings = setStrings(set);
        std::vector<std::u32string> ordered(strings.begin(), strings.end());
        std::stable_sort(ordered.begin(), ordered.end(), [](const std::u32string & a, const std::u32string & b) {return a.size() > b.size();});
        for (const std::u32string & str : ordered) {
            std::vector<re::RE *> alt;
            for (char32_t ch : str) alt.push_back(re::makeCC(static_cast<UCD::codepoint_t>(ch), &cc::Unicode));
            item.alts.push_back(alt);
            item.first.insert(str[0]);
            for (size_t k = 1; k < str.size(); k++) {
                const std::u32string prefix = str.substr(0, k);
                if ((k == 1 && chars.contains(prefix[0])) || strings.count(prefix)) item.forbiddenNext.insert(str[k]);
            }
        }
        if (!chars.empty()) item.alts.push_back({re::makeCC(chars, &cc::Unicode)});
        item.first = item.first + chars;
        return !item.alts.empty();
    };
    std::function<bool (re::RE *, size_t)> addItems = [&](re::RE * re, size_t t) -> bool {
        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
            for (re::RE * e : *seq) if (!addItems(e, t)) return false;
            return true;
        } else if (re::Name * n = dyn_cast<re::Name>(re);
                   n && !isFunctionCall(n) && n->getDefinition() && !CharSetAnalysis::isSet(n->getDefinition())) {
            return addItems(n->getDefinition(), t);
        } else if (re::Rep * rep = dyn_cast<re::Rep>(re)) {
            if (rep->getLB() != 0 || rep->getUB() != 1) return false;
            Item item;
            if (!setItem(rep->getRE(), item)) return false;
            item.optional = true;
            item.forbiddenNext = item.forbiddenNext + item.first;
            item.alts.push_back({});
            items.push_back(std::move(item));
            owner.push_back(t);
            return true;
        }
        Item item;
        if (!setItem(re, item)) return false;
        items.push_back(std::move(item));
        owner.push_back(t);
        return true;
    };
    for (size_t t = 0; t < textItemList.size(); t++) {
        re::RE * const re = textItemList[t];
        if (re::Capture * cap = dyn_cast<re::Capture>(re)) {
            if (containsCapture(cap->getCapturedRE()) || !addItems(cap->getCapturedRE(), t)) return false;
        } else if (!addItems(re, t)) {
            return false;
        }
    }
    //  The equivalence: what follows each item may not begin with a character it forbids.
    UCD::UnicodeSet afterFirst;
    if (source->getAfterContext() && !firstContextSet(source->getAfterContext(), afterFirst, sets)) afterFirst = UCD::UnicodeSet(0, 0x10FFFF);
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].forbiddenNext.empty()) continue;
        UCD::UnicodeSet next;
        size_t j = i + 1;
        for (; j < items.size(); j++) {
            next = next + items[j].first;
            if (!items[j].optional) break;
        }
        if (j == items.size()) next = next + afterFirst;
        if (next.intersects(items[i].forbiddenNext)) return false;
    }
    //  The combinations, the first item varying slowest.
    size_t count = 1;
    for (const Item & item : items) {
        count *= item.alts.size();
        if (count > MaxVariants) return false;
    }
    if (count == 1) return false;
    for (size_t v = 0; v < count; v++) {
        std::vector<size_t> choice(items.size());
        size_t rest = v;
        for (size_t i = items.size(); i-- > 0; ) {
            choice[i] = rest % items[i].alts.size();
            rest /= items[i].alts.size();
        }
        //  The text: the items outside the captures in place, and each capture
        //  rebuilt from the alternatives of its items.
        std::map<std::string, re::Capture *> captures;
        std::vector<re::RE *> ordered;
        for (size_t t = 0, i = 0; t < textItemList.size(); t++) {
            std::vector<re::RE *> parts;
            for (; i < items.size() && owner[i] == t; i++) {
                for (re::RE * e : items[i].alts[choice[i]]) parts.push_back(e);
            }
            if (re::Capture * cap = dyn_cast<re::Capture>(textItemList[t])) {
                re::Capture * const rebuilt = re::makeCapture(cap->getName(), re::makeSeq(parts.begin(), parts.end()));
                captures.emplace(cap->getName(), rebuilt);
                ordered.push_back(rebuilt);
            } else {
                ordered.insert(ordered.end(), parts.begin(), parts.end());
            }
        }
        std::function<re::RE * (re::RE *)> refs = [&](re::RE * re) -> re::RE * {
            if (re == nullptr) return nullptr;
            if (re::Reference * ref = dyn_cast<re::Reference>(re)) {
                auto f = captures.find(ref->getName());
                return f == captures.end() ? re : re::makeReference(ref->getName(), f->second, ref->getInstance());
            } else if (re::Name * n = dyn_cast<re::Name>(re)) {
                return isFunctionCall(n) ? makeFunctionCall(getFunctionID(n), refs(n->getDefinition())) : re;
            } else if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
                std::vector<re::RE *> es;
                for (re::RE * e : *seq) es.push_back(refs(e));
                return re::makeSeq(es.begin(), es.end());
            }
            return re;
        };
        RuleSide * const src = RuleSide::Create(source->getBeforeContext(), re::makeSeq(ordered.begin(), ordered.end()), false,
                                                nullptr, 0, source->getAfterContext());
        RuleSide * const res = RuleSide::Create(refs(result->getBeforeContext()), refs(result->getCompletedResult()),
                                                result->hasCursor(), refs(result->getResultToRevisit()),
                                                result->getCursorOffset(), refs(result->getAfterContext()));
        variants.push_back(makeConversionRule(src, Direction::Forward, res));
    }
    return true;
}

//  Whether a BeforeCaptureCopy rule P (C) { T } → R $1 reproduces its before
//  context (see DeferredRule::selfReproducing), given the items of P, R and the
//  shape of C:
//  1. P matches R: a sequence of sets, one for each character of R, holding it;
//     or, with R empty, P, matched right to left, matches the end of any copy
//     (its sets holding all the characters the copy may have at their
//     positions, its repetitions of sets holding none of them, matching nothing);
//  2. C, matched right to left before R c, takes no character before c: its
//     first item is a set, or the character before c (the last of R, or with R
//     empty, of the previous copy) is not in the set of its repetition.
static bool selfReproducingContext(const std::vector<re::RE *> & p, const std::u32string & r, const CopyCapture & c,
                                   CharSetAnalysis & sets) {
    //  The characters a copy may have at its last positions, from the end (as far
    //  as they are known: its suffix, or all its sets, without a repetition).
    //  (Sets of P may include the text boundary: at a run of texts, a character
    //  precedes.)
    auto charSet = [&](const re::RE * re) {return CharSetAnalysis::isSet(re) && !hasStrings(re);};
    std::vector<UCD::UnicodeSet> copyEnd(c.suffix.rbegin(), c.suffix.rend());
    if (!c.star) copyEnd.insert(copyEnd.end(), c.prefix.rbegin(), c.prefix.rend());
    if (copyEnd.empty() && c.star) copyEnd.push_back(c.starSet);
    if (!r.empty()) {
        if (p.size() != r.size()) return false;
        for (size_t i = 0; i < r.size(); i++) {
            if (!charSet(p[i]) || !sets.setOf(p[i], false).contains(r[i])) return false;
        }
    } else {
        size_t k = 0;       // the position of the copy reached, from its end
        for (size_t i = p.size(); i-- > 0; ) {
            re::RE * item = p[i];
            while (re::Name * n = dyn_cast<re::Name>(item)) {
                if (isFunctionCall(n) || n->getDefinition() == nullptr || CharSetAnalysis::isSet(n->getDefinition())) break;
                item = n->getDefinition();
            }
            if (k >= copyEnd.size() || (c.star && k >= c.suffix.size())) return false;
            if (const re::Rep * rep = dyn_cast<re::Rep>(item); rep && rep->getLB() == 0 && charSet(rep->getRE())) {
                if (sets.setOf(rep->getRE(), false).intersects(copyEnd[k])) return false;
                continue;
            }
            if (!charSet(item) || !(copyEnd[k] - sets.setOf(item, false)).empty()) return false;
            k++;
        }
    }
    if (c.prefix.empty() && c.star) {
        const UCD::UnicodeSet before = r.empty() ? copyEnd.front() : UCD::UnicodeSet(r.back());
        if (before.intersects(c.starSet)) return false;
    }
    return true;
}

//  Whether a before context may match before a text continuing the run of a
//  self-reproducing rule, as far as the block R c written before it tells: its
//  items, matched right to left against the characters the block may have at
//  each position (from its end: the copy, R, then the copy again; the
//  repetition of a copy matching any number of characters of its set), each
//  holding one of them (a repetition of a set holding any number of them).
//  Beyond what is known of the block, it may match.
static bool mayMatchBeforeRun(re::RE * before, const std::u32string & r, const CopyCapture & c, CharSetAnalysis & sets) {
    if (before == nullptr) return true;
    //  The block from its end: sets of one character, or a repetition (star).
    //  (Before the block, the copy c again: at the first text of the run, the
    //  region that the rule copied, and at the next, the copy written before.)
    struct Element {
        UCD::UnicodeSet chars;
        bool star;
    };
    std::vector<Element> block;
    auto addCopy = [&]() {
        for (size_t i = c.suffix.size(); i-- > 0; ) block.push_back(Element{c.suffix[i], false});
        if (c.star) block.push_back(Element{c.starSet, true});
        for (size_t i = c.prefix.size(); i-- > 0; ) block.push_back(Element{c.prefix[i], false});
    };
    addCopy();
    for (size_t i = r.size(); i-- > 0; ) block.push_back(Element{UCD::UnicodeSet(r[i]), false});
    addCopy();
    const size_t end = block.size();
    //  The positions of the block reached (end: beyond what is known), with
    //  the repetitions matching no character skipped.
    std::vector<bool> reached(end + 1, false);
    auto close = [&](std::vector<bool> & states) {
        for (size_t k = 0; k < end; k++) if (states[k] && block[k].star) states[k + 1] = true;
    };
    //  The positions reached from states by a character of chars.
    auto step = [&](const std::vector<bool> & states, const UCD::UnicodeSet & chars) {
        std::vector<bool> next(end + 1, false);
        for (size_t k = 0; k < end; k++) {
            if (!states[k] || !chars.intersects(block[k].chars)) continue;
            next[block[k].star ? k : k + 1] = true;
        }
        close(next);
        return next;
    };
    reached[0] = true;
    close(reached);
    std::vector<re::RE *> items;
    textItems(before, items);
    for (size_t i = items.size(); i-- > 0; ) {
        if (reached[end]) return true;
        re::RE * item = items[i];
        while (re::Name * n = dyn_cast<re::Name>(item)) {
            if (isFunctionCall(n) || n->getDefinition() == nullptr || CharSetAnalysis::isSet(n->getDefinition())) break;
            item = n->getDefinition();
        }
        if (const re::Rep * rep = dyn_cast<re::Rep>(item); rep && rep->getLB() == 0
                && CharSetAnalysis::isSet(rep->getRE()) && !hasStrings(rep->getRE())) {
            const UCD::UnicodeSet chars = sets.setOf(rep->getRE(), false);
            for (bool grown = true; grown; ) {
                const std::vector<bool> next = step(reached, chars);
                grown = false;
                for (size_t k = 0; k <= end; k++) {
                    if (next[k] && !reached[k]) reached[k] = grown = true;
                }
            }
            continue;
        }
        if (!CharSetAnalysis::isSet(item) || hasStrings(item)) return true;
        reached = step(reached, sets.setOf(item, false));
        if (std::find(reached.begin(), reached.end(), true) == reached.end()) return false;
    }
    return true;
}

TransformPlan planTransform(const std::vector<Rule *> & rulesGiven) {
    TransformPlan plan;
    CharSetAnalysis sets;
    TransformStep * group = nullptr;
    //  The MultipleCaptureMultipleReplacement rules whose replacements are not at
    //  fixed offsets, for captures of variable length, expanded where possible
    //  into rules whose captures have fixed lengths (see expandFiniteCaptures).
    std::vector<Rule *> rules;
    for (Rule * r : rulesGiven) {
        ConversionRule * const c = dyn_cast<ConversionRule>(r);
        if (c && c->getDirection() == Direction::Forward && containsCapture(c->getSourceSide(Direction::Forward)->getText())) {
            MultipleCaptureMultipleReplacement mr;
            MultipleCaptureMultipleReplacementSet ms;
            std::string reason;
            if (!multipleCaptureMultipleReplacement(c, mr, ms, sets, reason)
                    && (reason == "a replaced text lies between captures of variable length"
                        || reason == "an insertion is not next to a capture that is never empty, at a fixed offset")) {
                std::vector<Rule *> variants;
                bool ok = expandFiniteCaptures(c, sets, variants);
                for (Rule * v : variants) {
                    MultipleCaptureMultipleReplacement vr;
                    MultipleCaptureMultipleReplacementSet vs;
                    std::string vreason;
                    ok &= multipleCaptureMultipleReplacement(cast<ConversionRule>(v), vr, vs, sets, vreason);
                }
                if (ok) {
                    rules.insert(rules.end(), variants.begin(), variants.end());
                    continue;
                }
            }
        }
        rules.push_back(r);
    }
    //  The triggers of the BeforeCaptureCopy rules of each group (between
    //  transform rules): the rules of the group whose texts begin with them are
    //  deferred.
    std::vector<UCD::UnicodeSet> triggers(1);
    for (Rule * r : rules) {
        if (isa<TransformRule>(r)) {
            triggers.emplace_back();
        } else if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            std::vector<UCD::UnicodeSet> text;
            std::u32string replacement;
            re::Capture * capture = nullptr;
            re::RE * before = nullptr;
            if (beforeCaptureCopy(c, sets, text, replacement, capture, before)) {
                triggers.back() = triggers.back() + text.front();
            }
        }
    }
    //  The prefix runs (see ComposeRevisits) of the groups of prefix runs alone.
    std::set<const Rule *> prefixRuns;
    for (size_t start = 0; start < rules.size(); ) {
        size_t end = start;
        bool allRuns = true;
        while (end < rules.size() && !isa<TransformRule>(rules[end])) {
            PrefixRun run;
            if (const ConversionRule * c = dyn_cast<ConversionRule>(rules[end])) allRuns &= prefixRun(c, run);
            ++end;
        }
        for (size_t k = start; k < end && allRuns; ++k) {
            if (isa<ConversionRule>(rules[k])) prefixRuns.insert(rules[k]);
        }
        start = end + 1;
    }
    //  The chain revisits (see ComposeRevisits) of the groups of chain revisits
    //  alone, with the same E, M, A and Y.
    std::set<const Rule *> chainRevisits;
    for (size_t start = 0; start < rules.size(); ) {
        size_t end = start;
        bool allChains = true, any = false;
        ChainRevisit first;
        while (end < rules.size() && !isa<TransformRule>(rules[end])) {
            ChainRevisit ch;
            if (const ConversionRule * c = dyn_cast<ConversionRule>(rules[end])) {
                if (!chainRevisit(c, ch)) {
                    allChains = false;
                } else if (!any) {
                    first = ch;
                    any = true;
                } else {
                    allChains &= ch.first == first.first && ch.fixed == first.fixed && ch.star == first.star
                                 && ch.replacement == first.replacement;
                }
            }
            ++end;
        }
        for (size_t k = start; k < end && allChains && any; ++k) {
            if (isa<ConversionRule>(rules[k])) chainRevisits.insert(rules[k]);
        }
        start = end + 1;
    }
    //  The mark run edits (see ComposeRevisits) of the groups of mark run edits
    //  and rules kept before them alone.
    std::set<const Rule *> markRuns;
    for (size_t start = 0; start < rules.size(); ) {
        size_t end = start;
        bool all = true;
        std::vector<MarkRunEdit> edits;
        while (end < rules.size() && !isa<TransformRule>(rules[end])) {
            if (const ConversionRule * c = dyn_cast<ConversionRule>(rules[end])) {
                MarkRunEdit e;
                std::vector<UCD::UnicodeSet> items;
                if (markRunEdit(c, e)) {
                    edits.push_back(e);
                } else {
                    all &= keptText(c, items);
                }
            }
            ++end;
        }
        for (size_t k = start; k < end && all && !edits.empty(); ++k) {
            if (isa<ConversionRule>(rules[k])) markRuns.insert(rules[k]);
        }
        start = end + 1;
    }
    size_t groupIndex = 0;
    for (Rule * r : rules) {
        if (FilterRule * f = dyn_cast<FilterRule>(r)) {
            plan.filter = f->getFilterSet();
            plan.filterSet = sets.setOf(f->getFilterSet(), false);
        } else if (TransformRule * t = dyn_cast<TransformRule>(r)) {
            group = nullptr;
            groupIndex++;
            TransformStep step;
            step.kind = TransformStep::Kind::Transform;
            step.transform = t->getForwardID();
            step.filter = t->getForwardFilter();
            if (step.filter) step.filterSet = sets.setOf(step.filter, false);
            plan.steps.push_back(std::move(step));
        } else if (ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            const RuleSide * source = c->getSourceSide(Direction::Forward);
            const RuleSide * result = c->getResultSide(Direction::Forward);
            re::RE * const text = source->getText();
            //  A prefix run (see ComposeRevisits), in a group of prefix runs.
            if (PrefixRun run; prefixRuns.count(c) && prefixRun(c, run)) {
                if (group == nullptr) {
                    TransformStep step;
                    step.kind = TransformStep::Kind::Conversion;
                    plan.steps.push_back(std::move(step));
                    group = &plan.steps.back();
                }
                group->prefixRuns.push_back(run);
                group->prefixRunRules.push_back(c);
                continue;
            }
            //  A mark run edit (see ComposeRevisits), or a rule kept before them,
            //  in a group of mark run edits.
            if (markRuns.count(c)) {
                if (group == nullptr) {
                    TransformStep step;
                    step.kind = TransformStep::Kind::Conversion;
                    plan.steps.push_back(std::move(step));
                    group = &plan.steps.back();
                }
                MarkRunEdit e;
                std::vector<UCD::UnicodeSet> items;
                //  Within the filter (ICU's text matches no other character).
                auto within = [&](UCD::UnicodeSet & set) {
                    if (plan.filter) set = set & plan.filterSet;
                };
                if (markRunEdit(c, e)) {
                    for (UCD::UnicodeSet & p : e.prefix) within(p);
                    within(e.run);
                    within(e.firstRun);
                    for (UCD::UnicodeSet & t : e.suffix) within(t);
                    group->markRunEdits.push_back(e);
                    group->markRunEditRules.push_back(c);
                } else if (keptText(c, items)) {
                    for (UCD::UnicodeSet & i : items) within(i);
                    group->markRunKept.push_back(items);
                    group->markRunKeptBefore.push_back(static_cast<unsigned>(group->markRunEdits.size()));
                }
                continue;
            }
            //  A chain revisit (see ComposeRevisits), in a group of chain revisits.
            if (ChainRevisit ch; chainRevisits.count(c) && chainRevisit(c, ch)) {
                if (group == nullptr) {
                    TransformStep step;
                    step.kind = TransformStep::Kind::Conversion;
                    plan.steps.push_back(std::move(step));
                    group = &plan.steps.back();
                }
                if (group->chainRevisitRules.empty()) {
                    group->chainRevisit = ch;
                } else {
                    group->chainRevisit.last = group->chainRevisit.last + ch.last;
                }
                group->chainRevisitRules.push_back(c);
                continue;
            }
            if (unsupportedLookahead(source->getAfterContext())) {
                plan.problems.push_back(printRule(r) + "  (the after context repeats a set with strings or a sequence, "
                                        "which the regular expression engine does not support in lookaheads)");
                continue;
            }
            std::vector<re::RE *> items;
            textItems(text, items);
            //  A deferred rule (see DeferredRule): its text begins with a trigger
            //  of the BeforeCaptureCopy rules of its group.
            if (const UCD::UnicodeSet & trig = triggers[groupIndex];
                    !trig.empty() && !items.empty() && CharSetAnalysis::isSet(items.front())
                    && sets.setOf(items.front(), false).intersects(trig)) {
                DeferredRule d;
                d.rule = c;
                re::Capture * capture = nullptr;
                re::RE * before = source->getBeforeContext();
                std::string reason;
                CopyCapture copyCapture;
                if (beforeCaptureCopy(c, sets, d.text, d.replacement, capture, before)) {
                    d.capture = 0;
                    if (!copyCaptureShape(capture->getCapturedRE(), copyCapture, sets)) {
                        reason = "the capture it copies is not a sequence of sets with at most one unbounded repetition of a set";
                    }
                } else {
                    capture = nullptr;
                    before = source->getBeforeContext();
                    d.text.clear();
                    d.replacement.clear();
                    bool singleChars = true;
                    for (re::RE * item : items) {
                        singleChars &= plainSet(item);
                        if (singleChars) d.text.push_back(sets.setOf(item, false));
                    }
                    if (!singleChars) {
                        reason = "its text is not a fixed-length sequence of characters";
                    } else if (result->hasCursor() && (!isEmptyText(result->getResultToRevisit()) || result->getCursorOffset() != 0)) {
                        reason = "its result has text to revisit";
                    } else if (!fixedString(result->getText(), d.replacement)) {
                        reason = "its result is neither a fixed string nor one followed by a capture of its before context";
                    } else if ((before && containsCapture(before)) || (source->getAfterContext() && containsCapture(source->getAfterContext()))) {
                        reason = "its contexts have captures";
                    }
                }
                if (reason.empty() && !(d.text.front() - trig).empty()) {
                    reason = "its text may also begin with characters other than the triggers";
                }
                if (!reason.empty()) {
                    plan.problems.push_back(printRule(r) + "  (a rule deferred after the BeforeCaptureCopy rules of its group, but "
                                            + reason + ")");
                    continue;
                }
                if (group == nullptr) {
                    TransformStep step;
                    step.kind = TransformStep::Kind::Conversion;
                    plan.steps.push_back(std::move(step));
                    group = &plan.steps.back();
                }
                if (capture) {
                    std::vector<re::RE *> pItems;
                    std::function<void (re::RE *)> flatten = [&](re::RE * re) {
                        if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
                            for (re::RE * e : *seq) flatten(e);
                        } else {
                            pItems.push_back(re);
                        }
                    };
                    flatten(source->getBeforeContext());
                    pItems.pop_back();      // (the capture)
                    d.selfReproducing = selfReproducingContext(pItems, d.replacement, copyCapture, sets);
                    const std::string key = printPattern(capture->getCapturedRE());
                    size_t k = 0;
                    while (k < group->copyCaptures.size() && printPattern(group->copyCaptures[k].pattern) != key) k++;
                    if (k == group->copyCaptures.size()) group->copyCaptures.push_back(copyCapture);
                    d.capture = static_cast<int>(k);
                }
                d.engineBefore = before ? engineContext(before, false) : nullptr;
                d.engineAfter = source->getAfterContext() ? engineContext(source->getAfterContext(), true) : nullptr;
                group->deferredTriggers = group->deferredTriggers + d.text.front();
                for (size_t k = 1; k < d.text.size(); k++) {
                    if (group->deferredTails.size() < k) group->deferredTails.resize(k);
                    group->deferredTails[k - 1] = group->deferredTails[k - 1] + d.text[k];
                }
                //  The characters through which the before context may reach back:
                //  those of its items other than the first.
                if (before) {
                    std::vector<re::RE *> contextItems;
                    textItems(before, contextItems);
                    for (size_t k = 1; k < contextItems.size(); k++) {
                        UCD::UnicodeSet chars;
                        if (!matchableChars(contextItems[k], chars, sets)) chars = UCD::UnicodeSet(0, 0x10FFFF);
                        group->deferredReach = group->deferredReach + chars;
                    }
                }
                group->deferred.push_back(std::move(d));
                continue;
            }
            //  A run at the end of the text (see StringRule::run).
            UCD::UnicodeSet run;
            if (!items.empty()) {
                std::vector<re::RE *> first;
                if (runItem(items.back(), first, run, sets)) {
                    items.pop_back();
                    items.insert(items.end(), first.begin(), first.end());
                }
            }
            std::string problem;
            std::u32string replacement;
            bool singleChars = !items.empty();
            for (re::RE * item : items) singleChars &= CharSetAnalysis::isSet(item) && !hasStrings(item);
            bool boundary = false;
            for (re::RE * item : items) boundary |= mayIncludeTextBoundary(item);
            //  An identity rule, (S) → $1 with S a set of characters: a single-character
            //  rule keeping its characters.
            bool identity = false;
            if (items.size() == 1 && run.empty() && !result->hasCursor()) {
                auto single = [](const re::RE * re) {
                    const re::Seq * seq = dyn_cast<re::Seq>(re);
                    return (seq && seq->size() == 1) ? seq->front() : re;
                };
                const re::Capture * cap = dyn_cast<re::Capture>(single(text));
                const re::Reference * ref = dyn_cast<re::Reference>(single(result->getText()));
                identity = cap && ref && ref->getName() == cap->getName() && plainSet(cap->getCapturedRE());
            }
            MultipleCaptureMultipleReplacement capture;
            MultipleCaptureMultipleReplacementSet set;
            std::u32string fixedResult;
            std::string captureReason;
            //  An insertion rule (A { } C → S).
            if (items.empty() && run.empty()) {
                InsertionRule ins;
                std::string reason;
                if (!insertionRule(c, ins, sets, reason)) {
                    plan.problems.push_back(printRule(r) + "  (an insertion rule, but " + reason + ")");
                    continue;
                }
                if (group == nullptr) {
                    TransformStep step;
                    step.kind = TransformStep::Kind::Conversion;
                    plan.steps.push_back(std::move(step));
                    group = &plan.steps.back();
                }
                //  No rule of the group before it may apply at the position (the
                //  rules so far: its single-character rules, string rules, and the
                //  first characters of its MultipleCaptureMultipleReplacement rules).
                UCD::UnicodeSet earlier;
                for (const CharMapSubgroup & g : group->subgroups) earlier = earlier + g.characters();
                for (const StringRule & sr : group->stringRules) earlier = earlier + sr.text.front();
                bool unknown = false;
                for (const MultipleCaptureMultipleReplacementSet & set : group->captureReplacements) {
                    for (const MultipleCaptureMultipleReplacement & m : set.rules) {
                        if (!m.first.empty()) earlier = earlier + m.first.front();
                        else if (set.startsByFirst) earlier = earlier + set.startChars;
                        else unknown = true;
                    }
                }
                if (unknown || earlier.intersects(ins.afterFirst)) {
                    plan.problems.push_back(printRule(r) + "  (an insertion rule, but a rule before it in its group may "
                                            "apply where it inserts)");
                    continue;
                }
                group->insertions.push_back(std::move(ins));
                continue;
            }
            //  A MultipleCaptureMultipleReplacement rule (whose text has a fixed length or
            //  not, with a result referring to its captures in order).
            if (!identity && (!singleChars || !fixedString(result->getText(), fixedResult))
                    && multipleCaptureMultipleReplacement(c, capture, set, sets, captureReason)) {
                if (group == nullptr) {
                    TransformStep step;
                    step.kind = TransformStep::Kind::Conversion;
                    plan.steps.push_back(std::move(step));
                    group = &plan.steps.back();
                }
                const std::string key = captureReplacementKey(set, capture);
                MultipleCaptureMultipleReplacementSet * found = nullptr;
                for (MultipleCaptureMultipleReplacementSet & s : group->captureReplacements) {
                    if (captureReplacementKey(s, s.rules.front()) == key) found = &s;
                }
                if (found == nullptr) {
                    group->captureReplacements.push_back(set);
                    found = &group->captureReplacements.back();
                }
                found->rules.push_back(std::move(capture));
                continue;
            }
            if (!captureReason.empty()) {
                plan.problems.push_back(printRule(r) + "  (a MultipleCaptureMultipleReplacement rule, but " + captureReason + ")");
                continue;
            }
            if (!singleChars) {
                problem = "the text to replace is not a fixed-length sequence of characters";
            } else if (boundary) {
                problem = "the text to replace includes the text boundary";
            } else if (!run.empty() && source->getAfterContext()) {
                problem = "the text to replace ends with a repetition followed by an after context";
            } else if (result->hasCursor() && (!isEmptyText(result->getResultToRevisit()) || result->getCursorOffset() != 0)) {
                problem = "the result has text to revisit";
            } else if (!identity && !fixedString(result->getText(), replacement)) {
                problem = "the result is not a fixed string";
            }
            if (!problem.empty()) {
                plan.problems.push_back(printRule(r) + "  (" + problem + ")");
                continue;
            }
            if (group == nullptr) {
                TransformStep step;
                step.kind = TransformStep::Kind::Conversion;
                plan.steps.push_back(std::move(step));
                group = &plan.steps.back();
            }
            re::RE * const before = source->getBeforeContext();
            re::RE * const after = source->getAfterContext();
            group->rules.push_back(c);
            if (items.size() > 1 || !run.empty()) {
                StringRule s;
                s.run = run;
                for (re::RE * item : items) s.text.push_back(sets.setOf(item, false));
                s.replacement = replacement;
                s.before = before;
                s.after = after;
                s.engineBefore = before ? engineContext(before, false) : nullptr;
                s.engineAfter = after ? engineContext(after, true) : nullptr;
                s.rule = c;
                group->stringRules.push_back(std::move(s));
                continue;
            }
            const std::string key = (before ? printPattern(before) : "") + " { } " + (after ? printPattern(after) : "");
            CharMapSubgroup * subgroup = nullptr;
            for (CharMapSubgroup & s : group->subgroups) {
                if (s.contextKey == key) {
                    subgroup = &s;
                    break;
                }
            }
            if (subgroup == nullptr) {
                group->subgroups.emplace_back();
                subgroup = &group->subgroups.back();
                subgroup->before = before;
                subgroup->after = after;
                subgroup->engineBefore = before ? engineContext(before, false) : nullptr;
                subgroup->engineAfter = after ? engineContext(after, true) : nullptr;
                subgroup->contextKey = key;
            }
            subgroup->rules++;
            //  The earliest rule for a character takes precedence.
            const UCD::UnicodeSet chars = sets.setOf(items.front(), false) - subgroup->characters();
            if (chars.empty()) continue;
            subgroup->all = subgroup->all + chars;
            if (identity) {
                subgroup->kept = subgroup->kept + chars;
                continue;
            }
            CharMapping * same = nullptr;
            for (CharMapping & m : subgroup->mappings) {
                if (m.replacement == replacement) same = &m;
            }
            if (same) {
                same->chars = same->chars + chars;
            } else {
                subgroup->mappings.push_back(CharMapping{chars, replacement});
            }
        }
    }
    //  As the rules of a group are applied at all positions at once, a rule may
    //  match within the text of a string rule (where ICU, having replaced the
    //  text, does not apply rules).  A single-character rule matching there is
    //  kept from applying by the coverage of the string rules (see
    //  TransformStep::coverStringRules).  String rules matching within the texts
    //  of string rules are supported if the string rules of the group can be
    //  applied in a single pass (see StringRuleOverlaps): in a topological order,
    //  each applying where it does not begin within an applied match of a rule
    //  preceding it (TransformStep::stringOrder), and a rule of two characters
    //  matching within its own text by the selection of its matches (see
    //  StringRule::selfOverlapping).  Otherwise, the matches that apply are
    //  selected sequentially (TransformStep::sequentialSelection).
    //  A quick check first: a rule may match within the text only if its first
    //  characters may occur at a position of the text other than the first.
    //  The MultipleCaptureMultipleReplacement rules: they may not overlap the
    //  string rules of their group (neither matching within the other), and the
    //  single-character rules apply outside their matches.
    //  The rules with captures referred to in reverse order: the rules of their
    //  group whose before contexts may tell the order of the characters of the
    //  region apart (as ICU matches them against the converted text) are not
    //  supported.
    for (TransformStep & step : plan.steps) {
        UCD::UnicodeSet swapChars;
        for (const MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) swapChars = swapChars + set.swapChars;
        if (swapChars.empty()) continue;
        std::vector<const re::RE *> befores;
        for (const CharMapSubgroup & g : step.subgroups) befores.push_back(g.before);
        for (const StringRule & sr : step.stringRules) befores.push_back(sr.before);
        for (const MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) befores.push_back(set.before);
        for (const InsertionRule & ins : step.insertions) befores.push_back(ins.engineBefore);
        bool uniform = true;
        for (const re::RE * b : befores) uniform &= uniformContext(b, swapChars, sets);
        if (uniform) continue;
        for (const MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
            if (!set.reversed && !set.moved) continue;
            for (const MultipleCaptureMultipleReplacement & r : set.rules) {
                plan.problems.push_back(printRule(r.rule) + "  (a MultipleCaptureMultipleReplacement rule, but it refers to its "
                                        "captures out of order, and a before context of its group may tell their order apart)");
            }
        }
    }
    //  The insertion rules (see InsertionRule): not in a group with string rules,
    //  and the text inserted may not let an insertion rule match again at the
    //  position (its last character, then before the position, in the set of the
    //  before context of a rule whose after context may also match there).
    for (TransformStep & step : plan.steps) {
        if (step.insertions.empty()) continue;
        const bool strings = !step.stringRules.empty() || !step.captureReplacements.empty();
        for (const InsertionRule & ins : step.insertions) {
            if (strings) {
                plan.problems.push_back(printRule(ins.rule) + "  (an insertion rule, but in a group with string rules)");
                continue;
            }
            for (const InsertionRule & other : step.insertions) {
                if (other.before.contains(ins.replacement.back()) && other.afterFirst.intersects(ins.afterFirst)) {
                    plan.problems.push_back(printRule(ins.rule) + "  (an insertion rule, but its text may let an insertion "
                                            "rule match again: " + printRule(other.rule) + ")");
                    break;
                }
            }
        }
    }
    for (TransformStep & step : plan.steps) {
        if (step.captureReplacements.empty()) continue;
        step.coverStringRules = true;
        RuleOverlapAnalysis analysis;
        //  The rules of different sets that may overlap are selected sequentially.
        //  (Every position at which a match may begin must then be known: its sets
        //  must find their starts by their first characters, or have matches of
        //  one character.  A set whose starts are those of its spans misses the
        //  matches beginning within another, which apply where that one does not.)
        for (size_t a = 0; a < step.captureReplacements.size(); a++) {
            for (size_t b = a + 1; b < step.captureReplacements.size(); b++) {
                for (const MultipleCaptureMultipleReplacement & r1 : step.captureReplacements[a].rules) {
                    for (const MultipleCaptureMultipleReplacement & r2 : step.captureReplacements[b].rules) {
                        if (analysis.mayOverlap(r1.rule, r2.rule) || analysis.mayMatchWithin(r1.rule, r2.rule)
                                || analysis.mayMatchWithin(r2.rule, r1.rule)) {
                            step.captureSelection = true;
                        }
                    }
                }
            }
        }
        //  String rules that may overlap MultipleCaptureMultipleReplacement rules
        //  (one matching within the other): their matches are selected together
        //  (jointSelection).  A rule of each may not match at the same position (as
        //  the matches selected there would apply to both).
        for (const MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
            for (const MultipleCaptureMultipleReplacement & r : set.rules) {
                for (const StringRule & sr : step.stringRules) {
                    if (analysis.mayMatchWithin(sr.rule, r.rule) || analysis.mayMatchWithin(r.rule, sr.rule)) {
                        step.jointSelection = true;
                    }
                    if (analysis.mayOverlap(sr.rule, r.rule)) {
                        plan.problems.push_back(printRule(r.rule) + "  (a MultipleCaptureMultipleReplacement rule, but a string "
                                                "rule may match at the same position: " + printRule(sr.rule) + ")");
                    }
                }
            }
        }
        if (step.jointSelection) step.captureSelection = true;
        if (step.captureSelection) {
            for (MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
                if (set.startsByFirst || set.singleCharacter) continue;
                //  The starts found by the engine, with the captures in a lookahead.
                bool supported = true;
                for (const re::RE * c : set.captures) supported &= !unsupportedLookahead(c);
                if (supported || greedyMatchesPossessive(set, sets)) {
                    set.exactStarts = true;
                    set.greedyStarts = !supported;
                    continue;
                }
                for (const MultipleCaptureMultipleReplacement & r : set.rules) {
                    plan.problems.push_back(printRule(r.rule) + "  (a MultipleCaptureMultipleReplacement rule, but the matches of "
                                            "other such rules may overlap its matches, not all of whose starts are known)");
                }
            }
        }
    }
    for (TransformStep & step : plan.steps) {
        if (step.stringRules.empty()) continue;
        //  A group with runs: its matches have no fixed length, and are selected
        //  sequentially; the single-character rules apply outside them.  So are
        //  those of a group whose string rules may overlap its
        //  MultipleCaptureMultipleReplacement rules (with theirs).
        bool runs = step.jointSelection;
        for (const StringRule & s : step.stringRules) runs |= !s.run.empty();
        if (runs) {
            step.coverStringRules = true;
            step.sequentialSelection = true;
            for (size_t r = 0; r < step.stringRules.size(); r++) step.stringOrder.push_back(r);
            step.stringPredecessors.assign(step.stringRules.size(), {});
            continue;
        }
        std::vector<UCD::UnicodeSet> firstChars;
        std::vector<bool> single;
        for (const ConversionRule * r : step.rules) {
            std::vector<re::RE *> items;
            textItems(r->getSourceSide(Direction::Forward)->getText(), items);
            firstChars.push_back(items.empty() ? UCD::UnicodeSet() : sets.setOf(items.front(), false));
            single.push_back(items.size() == 1);
        }
        RuleOverlapAnalysis analysis;
        for (const StringRule & s : step.stringRules) {
            if (step.coverStringRules) break;
            UCD::UnicodeSet within;
            for (size_t k = 1; k < s.text.size(); k++) within = within + s.text[k];
            for (size_t i = 0; i < step.rules.size(); i++) {
                if (!single[i] || (firstChars[i] & within).empty()) continue;
                if (analysis.mayMatchWithin(step.rules[i], s.rule)) {
                    step.coverStringRules = true;
                    break;
                }
            }
        }
        //  String rules within the texts of string rules.
        const StringRuleOverlaps overlaps = analyzeStringRuleOverlaps(step);
        if (overlaps.pairs.empty()) continue;
        if (overlaps.singlePass()) {
            step.stringOrder = overlaps.order;
            step.stringPredecessors = overlaps.predecessors;
            for (const StringRuleOverlaps::Pair & p : overlaps.pairs) {
                if (p.kind == StringRuleOverlaps::Kind::Self) step.stringRules[p.outer].selfOverlapping = true;
            }
            continue;
        }
        //  Otherwise, the matches are selected sequentially.
        step.sequentialSelection = true;
        step.stringOrder.clear();
        for (size_t r = 0; r < step.stringRules.size(); r++) step.stringOrder.push_back(r);
        step.stringPredecessors.assign(step.stringRules.size(), {});
    }
    //  The deferred rules (see DeferredRule) are applied after the other rules of
    //  their group, to the converted text.  The other rules may therefore not
    //  match the triggers, nor begin with the characters that may follow one in
    //  the text of a deferred rule; their before contexts may not see the
    //  triggers or the characters with which the deferred rules replace them
    //  (which ICU, but not the other rules, sees); and the after contexts of the
    //  deferred rules, which see the converted text, must match it as they match
    //  the text (ICU matching them against the text not yet converted): each is
    //  a single set holding all or none of the characters the other rules
    //  change and replace them with, and these rules delete no character.
    for (TransformStep & step : plan.steps) {
        if (step.deferred.empty()) continue;
        std::string reason;
        UCD::UnicodeSet textChars, firstChars, changed;
        bool deletes = false;
        std::vector<const re::RE *> befores;
        auto addReplacement = [&](const std::u32string & r) {
            for (char32_t ch : r) changed.insert(ch);
            deletes |= r.empty();
        };
        for (const CharMapSubgroup & g : step.subgroups) {
            textChars = textChars + g.characters();
            firstChars = firstChars + g.characters();
            for (const CharMapping & m : g.mappings) {
                changed = changed + m.chars;
                addReplacement(m.replacement);
            }
            if (g.before) befores.push_back(g.before);
        }
        for (const StringRule & sr : step.stringRules) {
            for (const UCD::UnicodeSet & chars : sr.text) textChars = textChars + chars;
            textChars = textChars + sr.run;
            firstChars = firstChars + sr.text.front();
            //  (Only the character just after the text of a deferred rule matters to
            //  its after context: the first of the text of a rule.)
            changed = changed + sr.text.front();
            addReplacement(sr.replacement);
            if (sr.before) befores.push_back(sr.before);
        }
        UCD::UnicodeSet tails;
        for (const UCD::UnicodeSet & t : step.deferredTails) tails = tails + t;
        UCD::UnicodeSet output;
        for (const DeferredRule & d : step.deferred) for (char32_t ch : d.replacement) output.insert(ch);
        for (const CopyCapture & c : step.copyCaptures) {
            for (const UCD::UnicodeSet & chars : c.prefix) output = output + chars;
            for (const UCD::UnicodeSet & chars : c.suffix) output = output + chars;
            output = output + c.starSet;
        }
        if (!step.captureReplacements.empty() || !step.insertions.empty()) {
            reason = "its group has MultipleCaptureMultipleReplacement or insertion rules";
        } else if (textChars.intersects(step.deferredTriggers)) {
            reason = "another rule of its group may match a trigger";
        } else if (firstChars.intersects(tails)) {
            reason = "another rule of its group may begin with a character of its text after the first";
        }
        for (const re::RE * b : befores) {
            if (!reason.empty()) break;
            UCD::UnicodeSet chars;
            if (!matchableChars(b, chars, sets) || chars.intersects(step.deferredTriggers + tails + output)) {
                reason = "the before context of another rule of its group may see the triggers or their replacements";
            }
        }
        for (const DeferredRule & d : step.deferred) {
            if (!reason.empty()) break;
            re::RE * const after = d.rule->getSourceSide(Direction::Forward)->getAfterContext();
            if (after == nullptr) continue;
            std::vector<re::RE *> afterItems;
            textItems(after, afterItems);
            if (afterItems.size() != 1 || !CharSetAnalysis::isSet(afterItems.front()) || hasStrings(afterItems.front())) {
                reason = "a deferred rule has an after context of more than a set of characters";
                break;
            }
            const UCD::UnicodeSet a = sets.setOf(afterItems.front(), false);
            if (a.intersects(changed) && !(changed - a).empty()) {
                reason = "a deferred rule has an after context that the other rules of its group may change";
            } else if (deletes) {
                reason = "a deferred rule has an after context, and the other rules of its group delete characters";
            }
        }
        //  The deferred rules that may interfere with the runs of each
        //  self-reproducing rule.
        for (size_t i = 0; i < step.deferred.size(); i++) {
            DeferredRule & d = step.deferred[i];
            if (!d.selfReproducing) continue;
            for (size_t j = 0; j < i; j++) {
                re::RE * const before = step.deferred[j].rule->getSourceSide(Direction::Forward)->getBeforeContext();
                if (mayMatchBeforeRun(before, d.replacement, step.copyCaptures[d.capture], sets)) d.runInterferers.push_back(j);
            }
        }
        if (reason.empty()) continue;
        for (const DeferredRule & d : step.deferred) {
            plan.problems.push_back(printRule(d.rule) + "  (a rule deferred after the BeforeCaptureCopy rules of its group, but "
                                    + reason + ")");
        }
    }
    //  The prefix runs of a group are applied together, as a group of their own
    //  (with no other rules), in families (see prefixRunFamilies).
    for (const TransformStep & step : plan.steps) {
        if (step.prefixRuns.empty()) continue;
        const std::string reason = prefixRunFamilies(step.prefixRuns);
        if (reason.empty()) continue;
        for (const ConversionRule * c : step.prefixRunRules) {
            plan.problems.push_back(printRule(c) + "  (a prefix run, but " + reason + ")");
        }
    }
    return plan;
}

StringRuleOverlaps analyzeStringRuleOverlaps(const TransformStep & step) {
    StringRuleOverlaps result;
    const size_t n = step.stringRules.size();
    //  within[a][b]: rule b may match within the text of rule a.  Quick checks
    //  first, of the texts without the contexts (which only restrict matches):
    //  the first characters of b must occur at a position of the text of a other
    //  than the first, and, for some position k of the text of a other than the
    //  first, each character of the text of b up to the end of the text of a
    //  must be one that may occur at its position in the text of a.
    std::vector<std::vector<bool>> within(n, std::vector<bool>(n, false));
    RuleOverlapAnalysis analysis;
    auto textsOverlap = [](const std::vector<UCD::UnicodeSet> & outer, const std::vector<UCD::UnicodeSet> & inner) {
        for (size_t k = 1; k < outer.size(); k++) {
            bool overlap = true;
            for (size_t i = 0; overlap && i < inner.size() && k + i < outer.size(); i++) {
                overlap = outer[k + i].intersects(inner[i]);
            }
            if (overlap) return true;
        }
        return false;
    };
    for (size_t a = 0; a < n; a++) {
        const StringRule & outer = step.stringRules[a];
        UCD::UnicodeSet inside;
        for (size_t k = 1; k < outer.text.size(); k++) inside = inside + outer.text[k];
        for (size_t b = 0; b < n; b++) {
            const StringRule & inner = step.stringRules[b];
            if (!inner.text.front().intersects(inside) || !textsOverlap(outer.text, inner.text)) continue;
            within[a][b] = analysis.mayMatchWithin(inner.rule, outer.rule);
        }
    }
    //  The pairs, and the precedence graph of the one-way pairs and self-overlaps.
    std::vector<std::vector<size_t>> precedes(n);
    for (size_t a = 0; a < n; a++) {
        if (within[a][a]) {
            result.pairs.push_back({a, a, StringRuleOverlaps::Kind::Self});
            result.self++;
            if (step.stringRules[a].text.size() != 2) result.selfSelectable = false;
            precedes[a].push_back(a);
        }
        for (size_t b = 0; b < n; b++) {
            if (b == a || !within[a][b]) continue;
            if (within[b][a]) {
                if (a < b) {
                    result.pairs.push_back({a, b, StringRuleOverlaps::Kind::TwoWay});
                    result.twoWay++;
                }
            } else {
                result.pairs.push_back({a, b, StringRuleOverlaps::Kind::OneWay});
                result.oneWay++;
                precedes[a].push_back(b);
            }
        }
    }
    //  A depth-first search of the graph (with or without the self-overlaps):
    //  a cycle (an edge to a rule on the path), or the number of rules in the
    //  longest chain (0 without edges).
    std::vector<size_t> postorder;
    auto search = [&](bool withSelf, std::vector<size_t> & cycle) -> std::pair<bool, unsigned> {
        enum : unsigned char {Unvisited, OnPath, Done};
        std::vector<unsigned char> state(n, Unvisited);
        std::vector<unsigned> chain(n, 1);
        std::vector<size_t> path;
        std::vector<std::pair<size_t, size_t>> stack;     // (rule, next edge)
        unsigned stages = 0;
        for (size_t root = 0; root < n; root++) {
            if (state[root] != Unvisited) continue;
            stack.emplace_back(root, 0);
            state[root] = OnPath;
            path.push_back(root);
            while (!stack.empty()) {
                auto & top = stack.back();
                const size_t v = top.first;
                if (top.second < precedes[v].size()) {
                    const size_t w = precedes[v][top.second++];
                    if (w == v && !withSelf) continue;
                    if (state[w] == OnPath) {
                        cycle.assign(std::find(path.begin(), path.end(), w), path.end());
                        return {false, 0};
                    } else if (state[w] == Unvisited) {
                        state[w] = OnPath;
                        path.push_back(w);
                        stack.emplace_back(w, 0);
                    }
                } else {
                    bool edges = false;
                    for (size_t w : precedes[v]) {
                        if (w == v) continue;
                        edges = true;
                        chain[v] = std::max(chain[v], chain[w] + 1);
                    }
                    if (edges) stages = std::max(stages, chain[v]);
                    if (!withSelf) postorder.push_back(v);
                    state[v] = Done;
                    path.pop_back();
                    stack.pop_back();
                }
            }
        }
        return {true, stages};
    };
    std::tie(result.acyclic, result.stages) = search(true, result.cycle);
    std::vector<size_t> ignored;
    std::tie(result.acyclicApartFromSelf, result.stagesApartFromSelf) = search(false, ignored);
    //  The strongly connected components of the graph of all the overlaps
    //  (Tarjan's algorithm, iteratively).
    {
        std::vector<std::vector<size_t>> edges(n);
        for (size_t a = 0; a < n; a++) {
            for (size_t b = 0; b < n; b++) {
                if (within[a][b]) edges[a].push_back(b);
            }
        }
        const size_t none = SIZE_MAX;
        std::vector<size_t> index(n, none), low(n, 0);
        std::vector<bool> onStack(n, false);
        std::vector<size_t> stack;
        std::vector<std::pair<size_t, size_t>> work;
        size_t next = 0;
        for (size_t root = 0; root < n; root++) {
            if (index[root] != none) continue;
            work.emplace_back(root, 0);
            while (!work.empty()) {
                const size_t v = work.back().first;
                size_t & e = work.back().second;
                if (e == 0 && index[v] == none) {
                    index[v] = low[v] = next++;
                    stack.push_back(v);
                    onStack[v] = true;
                }
                if (e < edges[v].size()) {
                    const size_t w = edges[v][e++];
                    if (index[w] == none) {
                        work.emplace_back(w, 0);
                    } else if (onStack[w]) {
                        low[v] = std::min(low[v], index[w]);
                    }
                    continue;
                }
                if (low[v] == index[v]) {
                    std::vector<size_t> component;
                    size_t w;
                    do {
                        w = stack.back();
                        stack.pop_back();
                        onStack[w] = false;
                        component.push_back(w);
                    } while (w != v);
                    if (component.size() > 1 || within[v][v]) {
                        result.cyclicComponents++;
                        result.largestComponent = std::max(result.largestComponent, component.size());
                        bool pairs = true;
                        for (size_t r : component) pairs &= step.stringRules[r].text.size() == 2;
                        if (pairs) result.pairComponents++; else result.pairSelectable = false;
                    }
                }
                work.pop_back();
                if (!work.empty()) {
                    const size_t u = work.back().first;
                    low[u] = std::min(low[u], low[v]);
                }
            }
        }
    }
    if (result.acyclicApartFromSelf) {
        //  Each rule follows the rules it is reached from.
        result.order.assign(postorder.rbegin(), postorder.rend());
        result.predecessors.resize(n);
        for (size_t a = 0; a < n; a++) {
            for (size_t b : precedes[a]) {
                if (b != a) result.predecessors[b].push_back(a);
            }
        }
    }
    return result;
}

//  The strings (of two or more characters) of a set, such as [aeiou{ü}] or
//  [$vowel-[ae]], through alternations, differences, intersections and
//  variables.
static std::set<std::u32string> setStrings(const re::RE * re) {
    std::set<std::u32string> strings;
    if (const re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (const re::RE * a : *alt) {
            const std::set<std::u32string> s = setStrings(a);
            strings.insert(s.begin(), s.end());
        }
    } else if (const re::Seq * seq = dyn_cast<re::Seq>(re)) {
        std::u32string str;
        if (seq->size() > 1 && fixedString(seq, str)) strings.insert(str);
    } else if (const re::Diff * d = dyn_cast<re::Diff>(re)) {
        strings = setStrings(d->getLH());
        for (const std::u32string & str : setStrings(d->getRH())) strings.erase(str);
    } else if (const re::Intersect * x = dyn_cast<re::Intersect>(re)) {
        const std::set<std::u32string> r = setStrings(x->getRH());
        for (const std::u32string & str : setStrings(x->getLH())) if (r.count(str)) strings.insert(str);
    } else if (const re::Name * n = dyn_cast<re::Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition()) strings = setStrings(n->getDefinition());
    }
    return strings;
}

static re::RE * expandContext(re::RE * re, const std::map<unsigned, UCD::UnicodeSet> & insertions,
                              UCD::codepoint_t filler, CharSetAnalysis & sets) {
    if (isa<re::Diff>(re) || isa<re::Intersect>(re)) {
        //  (The strings of the set, as alternatives, as in an alternation.)
        const std::set<std::u32string> strings = setStrings(re);
        if (!strings.empty()) {
            std::vector<re::RE *> alts;
            for (const std::u32string & str : strings) {
                std::vector<re::RE *> chars;
                for (char32_t ch : str) chars.push_back(re::makeCC(static_cast<UCD::codepoint_t>(ch), &cc::Unicode));
                alts.push_back(re::makeSeq(chars.begin(), chars.end()));
            }
            CharSetAnalysis charSets;
            alts.push_back(expandContext(re::makeCC(sets.setOf(re, false), &cc::Unicode), insertions, filler, charSets));
            return re::makeAlt(alts.begin(), alts.end());
        }
    }
    if (isa<re::CC>(re) || isa<re::PropertyExpression>(re) || isa<re::Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)) {
        UCD::UnicodeSet chars = sets.setOf(re, false);
        chars = chars - UCD::UnicodeSet(filler);
        //  The characters by the number of filler codepoints following them.
        std::vector<re::RE *> alts;
        UCD::UnicodeSet plain = chars;
        for (const auto & k : insertions) {
            if (k.first == 0) continue;
            const UCD::UnicodeSet followed = chars & k.second;
            if (followed.empty()) continue;
            plain = plain - followed;
            std::vector<re::RE *> seq{re::makeCC(followed, &cc::Unicode)};
            for (unsigned i = 0; i < k.first; i++) seq.push_back(re::makeCC(filler, &cc::Unicode));
            alts.push_back(re::makeSeq(seq.begin(), seq.end()));
        }
        alts.insert(alts.begin(), re::makeCC(plain, &cc::Unicode));
        return alts.size() == 1 ? alts[0] : re::makeAlt(alts.begin(), alts.end());
    } else if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
        std::vector<re::RE *> items;
        for (re::RE * e : *seq) items.push_back(expandContext(e, insertions, filler, sets));
        return re::makeSeq(items.begin(), items.end());
    } else if (re::Alt * alt = dyn_cast<re::Alt>(re)) {
        std::vector<re::RE *> items;
        for (re::RE * a : *alt) items.push_back(expandContext(a, insertions, filler, sets));
        return re::makeAlt(items.begin(), items.end());
    } else if (re::Rep * rep = dyn_cast<re::Rep>(re)) {
        return re::makeRep(expandContext(rep->getRE(), insertions, filler, sets), rep->getLB(), rep->getUB());
    } else if (re::Assertion * a = dyn_cast<re::Assertion>(re)) {
        return re::makeAssertion(expandContext(a->getAsserted(), insertions, filler, sets), a->getKind(), a->getSense());
    } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
        return expandContext(c->getCapturedRE(), insertions, filler, sets);
    } else if (re::Name * name = dyn_cast<re::Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return re;
        return expandContext(name->getDefinition(), insertions, filler, sets);
    }
    return re;
}

re::RE * expandedContext(re::RE * context, const std::map<unsigned, UCD::UnicodeSet> & insertions,
                         UCD::codepoint_t filler) {
    CharSetAnalysis sets;
    return expandContext(context, insertions, filler, sets);
}

static void collectItems(re::RE * re, bool repeated, std::vector<PatternItem> & items, CharSetAnalysis & sets) {
    if (isa<re::CC>(re) || isa<re::PropertyExpression>(re) || isa<re::Any>(re) || isa<re::Diff>(re) || isa<re::Intersect>(re)
            || ((isa<re::Alt>(re) || isa<re::Name>(re)) && CharSetAnalysis::isSet(re))) {
        items.push_back(PatternItem{sets.setOf(re, true), repeated});
    } else if (re::Seq * seq = dyn_cast<re::Seq>(re)) {
        for (re::RE * e : *seq) collectItems(e, repeated, items, sets);
    } else if (re::Alt * alt = dyn_cast<re::Alt>(re)) {
        for (re::RE * a : *alt) collectItems(a, repeated, items, sets);
    } else if (re::Rep * rep = dyn_cast<re::Rep>(re)) {
        collectItems(rep->getRE(), true, items, sets);
    } else if (re::Assertion * a = dyn_cast<re::Assertion>(re)) {
        collectItems(a->getAsserted(), repeated, items, sets);
    } else if (re::Capture * c = dyn_cast<re::Capture>(re)) {
        collectItems(c->getCapturedRE(), repeated, items, sets);
    } else if (re::Name * name = dyn_cast<re::Name>(re)) {
        if (!isFunctionCall(name) && name->getDefinition()) collectItems(name->getDefinition(), repeated, items, sets);
    }
}

std::vector<PatternItem> patternItems(re::RE * pattern) {
    CharSetAnalysis sets;
    std::vector<PatternItem> items;
    collectItems(pattern, false, items, sets);
    return items;
}

UCD::UnicodeSet patternCharacters(re::RE * pattern) {
    UCD::UnicodeSet chars;
    for (const PatternItem & item : patternItems(pattern)) chars = chars + item.chars;
    return chars;
}

}

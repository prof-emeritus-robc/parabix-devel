/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_plan.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
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
    for (re::RE * item : out) {
        if (const re::Reference * r = dyn_cast<re::Reference>(item)) {
            const size_t i = results.size() - 1;
            if (i >= k || r->getName() != captures[i]->getName()) return false;
            results.emplace_back();
        } else {
            std::u32string str;
            if (!fixedString(item, str)) return false;
            results.back() += str;
        }
    }
    if (results.size() != k + 1) return false;
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
    return key;
}

static bool isEmptyText(const re::RE * re) {
    const re::Seq * seq = dyn_cast_or_null<re::Seq>(re);
    return re == nullptr || (seq && seq->empty());
}

UCD::UnicodeSet CharMapSubgroup::characters() const {
    UCD::UnicodeSet all;
    for (const CharMapping & m : mappings) all = all + m.chars;
    return all + kept;
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
    const re::RE * const first = firstContextItem(after);
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

TransformPlan planTransform(const std::vector<Rule *> & rules) {
    TransformPlan plan;
    CharSetAnalysis sets;
    TransformStep * group = nullptr;
    for (Rule * r : rules) {
        if (FilterRule * f = dyn_cast<FilterRule>(r)) {
            plan.filter = f->getFilterSet();
            plan.filterSet = sets.setOf(f->getFilterSet(), false);
        } else if (TransformRule * t = dyn_cast<TransformRule>(r)) {
            group = nullptr;
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
            if (unsupportedLookahead(source->getAfterContext())) {
                plan.problems.push_back(printRule(r) + "  (the after context repeats a set with strings or a sequence, "
                                        "which the regular expression engine does not support in lookaheads)");
                continue;
            }
            std::vector<re::RE *> items;
            textItems(text, items);
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
        if (step.captureSelection) {
            for (const MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
                if (set.startsByFirst || set.singleCharacter) continue;
                for (const MultipleCaptureMultipleReplacement & r : set.rules) {
                    plan.problems.push_back(printRule(r.rule) + "  (a MultipleCaptureMultipleReplacement rule, but the matches of "
                                            "other such rules may overlap its matches, not all of whose starts are known)");
                }
            }
        }
        for (const MultipleCaptureMultipleReplacementSet & set : step.captureReplacements) {
            for (const MultipleCaptureMultipleReplacement & r : set.rules) {
                for (const StringRule & sr : step.stringRules) {
                    if (analysis.mayMatchWithin(sr.rule, r.rule) || analysis.mayMatchWithin(r.rule, sr.rule)) {
                        plan.problems.push_back(printRule(r.rule) + "  (a string rule may overlap this MultipleCaptureMultipleReplacement rule: "
                                                + printRule(sr.rule) + ")");
                        break;
                    }
                }
            }
        }
    }
    for (TransformStep & step : plan.steps) {
        if (step.stringRules.empty()) continue;
        //  A group with runs: its matches have no fixed length, and are selected
        //  sequentially; the single-character rules apply outside them.
        bool runs = false;
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

static re::RE * expandContext(re::RE * re, const std::map<unsigned, UCD::UnicodeSet> & insertions,
                              UCD::codepoint_t filler, CharSetAnalysis & sets) {
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

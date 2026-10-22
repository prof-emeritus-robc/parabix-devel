/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/analysis/replacement_analysis.h>

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

unsigned utf8Length(char32_t cp) {
    return (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
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
    const auto captures = capturesByName(search);
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

}

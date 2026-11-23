/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  NFD preservation of conversion groups (see Is_NFD_Preserving in
 *  include/ldml/transform_rules.h), and the NFD status of transforms (see
 *  include/ldml/nfd_status.h).
 */

#include <ldml/transform_rules.h>
#include <ldml/nfd_status.h>
#include <ldml/transform_rules_parser.h>
#include <ldml/transform_rules_printer.h>
#include "charset_analysis.h"
#include <fstream>
#include <memory>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <re/adt/adt.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>

using namespace llvm;
using namespace re;
using UCD::UnicodeSet;

namespace ldml {

namespace {

//  The marks by canonical combining class, the starters, and the characters
//  without a canonical decomposition.
struct CombiningClasses {
    std::vector<std::pair<unsigned, UnicodeSet>> marks;     // (ccc > 0, its characters), by ccc
    UnicodeSet starters;
    UnicodeSet nfdStable;
    static constexpr unsigned None = 256;   // no mark: above every ccc

    CombiningClasses() {
        auto * const ccc = cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
        for (unsigned v = 0; v < std::size(UCD::CCC_ns::enum_val); v++) {
            const UnicodeSet set = ccc->GetCodepointSet(v);
            if (set.empty()) continue;
            const unsigned n = UCD::CCC_ns::enum_val[v];
            if (n == 0) starters = starters + set; else marks.emplace_back(n, set);
        }
        std::sort(marks.begin(), marks.end(), [](const auto & a, const auto & b) {return a.first < b.first;});
        auto * const qc = cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::NFD_QC));
        nfdStable = qc->GetCodepointSet(UCD::NFD_QC_ns::Y);
    }
    //  The highest ccc of the marks of a set (0 if it has none).
    unsigned maxMark(const UnicodeSet & s) const {
        for (auto i = marks.rbegin(); i != marks.rend(); ++i) if (s.intersects(i->second)) return i->first;
        return 0;
    }
    //  The lowest ccc of the marks of a set (None if it has none).
    unsigned minMark(const UnicodeSet & s) const {
        for (const auto & m : marks) if (s.intersects(m.second)) return m.first;
        return None;
    }
    bool allMarks(const UnicodeSet & s) const {return !s.empty() && !s.intersects(starters);}
    //  May a character of q follow one of p in NFD?
    bool ordered(const UnicodeSet & p, const UnicodeSet & q) const {
        const unsigned a = maxMark(p);
        const unsigned b = minMark(q);
        return a == 0 || b == None || a <= b;
    }
    static const CombiningClasses & get() {
        static const CombiningClasses c;
        return c;
    }
};

//  The characters with which the matches of a pattern may begin and end, all
//  their characters, whether a match may be empty, and whether a text boundary
//  may be at its start or end.
struct Edges {
    UnicodeSet first, last, chars;
    bool nullable = true;
    bool firstBoundary = false;
    bool lastBoundary = false;

    static Edges character(const UnicodeSet & s) {
        Edges e;
        e.first = e.last = e.chars = s;
        e.nullable = false;
        return e;
    }
    static Edges boundary() {
        Edges e;
        e.firstBoundary = e.lastBoundary = true;
        return e;
    }
    void append(const Edges & e) {
        if (nullable) {
            first = first + e.first;
            firstBoundary = firstBoundary || e.firstBoundary;
        }
        if (e.nullable) {
            last = last + e.last;
            lastBoundary = lastBoundary || e.lastBoundary;
        } else {
            last = e.last;
            lastBoundary = e.lastBoundary;
        }
        chars = chars + e.chars;
        nullable = nullable && e.nullable;
    }
    void unite(const Edges & e) {
        first = first + e.first;
        last = last + e.last;
        chars = chars + e.chars;
        nullable = nullable || e.nullable;
        firstBoundary = firstBoundary || e.firstBoundary;
        lastBoundary = lastBoundary || e.lastBoundary;
    }
};

std::string hexCodepoint(UCD::codepoint_t cp) {
    std::string s;
    raw_string_ostream out(s);
    out << "U+" << format_hex_no_prefix(cp, 4, true);
    return out.str();
}

class Analysis {
public:
    bool check(const ConversionRule * rule, std::string & reason);
private:
    bool pattern(RE * re, Edges & e);
    bool result(RE * re, Edges & e, std::string & reason);
    CharSetAnalysis mSets;
    const CombiningClasses & C = CombiningClasses::get();
};

//  The edges of a pattern (a context or a text to replace); false if unknown.
bool Analysis::pattern(RE * re, Edges & e) {
    if (re == nullptr) {
        e = Edges();
        return true;
    }
    if (isa<Start>(re) || isa<End>(re) || isTextBoundary(re)) {
        e = Edges::boundary();
        return true;
    }
    if (isa<Assertion>(re)) {   // (zero-width)
        e = Edges();
        return true;
    }
    if (Seq * const seq = dyn_cast<Seq>(re)) {
        Edges acc;
        for (RE * item : *seq) {
            Edges ie;
            if (!pattern(item, ie)) return false;
            acc.append(ie);
        }
        e = acc;
        return true;
    }
    if (Alt * const alt = dyn_cast<Alt>(re)) {
        bool firstAlt = true;
        Edges acc;
        for (RE * item : *alt) {
            Edges ie;
            if (!pattern(item, ie)) return false;
            if (firstAlt) acc = ie; else acc.unite(ie);
            firstAlt = false;
        }
        e = acc;
        return true;
    }
    if (Rep * const rep = dyn_cast<Rep>(re)) {
        if (rep->getUB() == 0) {
            e = Edges();
            return true;
        }
        if (!pattern(rep->getRE(), e)) return false;
        if (rep->getLB() == 0) e.nullable = true;
        return true;
    }
    if (Capture * const c = dyn_cast<Capture>(re)) return pattern(c->getCapturedRE(), e);
    if (Name * const n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n) || n->getDefinition() == nullptr) return false;
        if (!CharSetAnalysis::isSet(n)) return pattern(n->getDefinition(), e);
    }
    if (CharSetAnalysis::isSet(re)) {
        e = Edges::character(mSets.setOf(re, true));
        if (mayIncludeTextBoundary(re)) {
            e.nullable = true;
            e.firstBoundary = e.lastBoundary = true;
        }
        return true;
    }
    return false;
}

//  The edges of a result, with its literal characters checked (NFD_QC=Y, in
//  canonical order) and the joins of its items; false with the reason if it
//  may not be in NFD or is not analysed.
bool Analysis::result(RE * re, Edges & e, std::string & reason) {
    if (re == nullptr) {
        e = Edges();
        return true;
    }
    if (Seq * const seq = dyn_cast<Seq>(re)) {
        Edges acc;
        for (RE * item : *seq) {
            Edges ie;
            if (!result(item, ie, reason)) return false;
            if (!ie.first.empty() && !C.ordered(acc.last, ie.first)) {
                reason = "the result puts a mark of ccc " + std::to_string(C.minMark(ie.first)) +
                         " after one of ccc " + std::to_string(C.maxMark(acc.last));
                return false;
            }
            acc.append(ie);
        }
        e = acc;
        return true;
    }
    if (const Reference * const ref = dyn_cast<Reference>(re)) {
        //  A part of the text (in NFD): its characters are in order.
        if (ref->getCapture() == nullptr || !pattern(ref->getCapture()->getCapturedRE(), e)) {
            reason = "the capture of " + ref->getName() + " is not analysed";
            return false;
        }
        e.firstBoundary = e.lastBoundary = false;
        return true;
    }
    if (Name * const n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) {
            reason = "the result calls a function (&" + getFunctionID(n).getCanonicalName() + ")";
            return false;
        }
        if (n->getDefinition()) return result(n->getDefinition(), e, reason);
    }
    if (const CC * const cc = dyn_cast<CC>(re)) {
        const UnicodeSet decomposable = UnicodeSet(*cc) - C.nfdStable;
        if (!decomposable.empty()) {
            reason = "the result has " + hexCodepoint(decomposable.front().first) + ", which has a canonical decomposition";
            return false;
        }
        e = Edges::character(*cc);
        return true;
    }
    reason = "the result is not analysed";
    return false;
}

//  The capture with which a pattern (a text to replace) or a result begins
//  (front) or ends, as an item (in a result, the capture referenced).
const Capture * edgeCapture(RE * re, bool front) {
    if (Seq * const seq = dyn_cast_or_null<Seq>(re)) {
        if (seq->empty()) return nullptr;
        return edgeCapture(front ? seq->front() : seq->back(), front);
    }
    if (const Capture * const c = dyn_cast_or_null<Capture>(re)) return c;
    if (const Reference * const r = dyn_cast_or_null<Reference>(re)) return r->getCapture();
    return nullptr;
}

bool Analysis::check(const ConversionRule * rule, std::string & reason) {
    const RuleSide * const source = rule->getSourceSide(Direction::Forward);
    const RuleSide * const target = rule->getResultSide(Direction::Forward);
    Edges S;
    if (!result(target->getText(), S, reason)) return false;
    Edges T, B, A;
    const bool knownT = pattern(source->getText(), T);
    //  The bound of the character before the result (the highest ccc it may
    //  have if it is a mark; 0: a starter, or none), and that of the character
    //  after it (the lowest ccc; None: a starter or none).
    unsigned before = 255;
    unsigned after = 1;
    if (source->hasBeforeContext() && pattern(source->getBeforeContext(), B) && !B.nullable && !B.lastBoundary) {
        before = C.maxMark(B.last);
    }
    if (knownT && !T.nullable && !T.firstBoundary && C.allMarks(T.first)) {
        before = std::min(before, C.maxMark(T.first));
    }
    if (source->hasAfterContext() && pattern(source->getAfterContext(), A) && !A.nullable && !A.firstBoundary) {
        after = C.minMark(A.first);
    }
    if (knownT && !T.nullable && !T.lastBoundary && C.allMarks(T.last)) {
        after = std::max(after, C.minMark(T.last));
    }
    //  A result that begins with a copy of the capture with which the text
    //  begins (not empty) begins with the same character, after the same one;
    //  likewise at the end.
    bool sameFirst = false, sameLast = false;
    if (knownT) {
        const Capture * const t0 = edgeCapture(source->getText(), true);
        const Capture * const t1 = edgeCapture(source->getText(), false);
        Edges c;
        sameFirst = t0 && t0 == edgeCapture(target->getText(), true) && pattern(t0->getCapturedRE(), c) && !c.nullable;
        sameLast = t1 && t1 == edgeCapture(target->getText(), false) && pattern(t1->getCapturedRE(), c) && !c.nullable;
    }
    if (!S.first.empty()) {
        const unsigned m = C.minMark(S.first);
        if (!sameFirst && before != 0 && m != CombiningClasses::None && before > m) {
            reason = "the result may begin with a mark of ccc " + std::to_string(m) + " after a mark of ccc " +
                     (before == 255 ? std::string("up to 255") : std::to_string(before));
            return false;
        }
        const unsigned x = C.maxMark(S.last);
        if (!sameLast && x != 0 && after != CombiningClasses::None && x > after) {
            reason = "the result may end with a mark of ccc " + std::to_string(x) + " before a mark of ccc " +
                     (after == 1 ? std::string("from 1") : std::to_string(after));
            return false;
        }
    }
    if (S.nullable) {
        const bool marksOnly = knownT && !T.nullable && C.allMarks(T.chars);
        if (!marksOnly && before != 0 && after != CombiningClasses::None && before > after) {
            reason = "deleting the text may bring together marks out of order";
            return false;
        }
    }
    return true;
}

}

bool Is_NFD_Preserving(const std::vector<Rule *> & group, NFD_PreservationFailure * failure) {
    Analysis analysis;
    for (const Rule * r : group) {
        const ConversionRule * const rule = dyn_cast<ConversionRule>(r);
        if (rule == nullptr || !appliesInDirection(rule, Direction::Forward)) continue;
        std::string reason;
        if (!analysis.check(rule, reason)) {
            if (failure) {
                failure->rule = rule;
                failure->reason = reason;
            }
            return false;
        }
    }
    return true;
}

std::vector<std::vector<Rule *>> ConversionGroups(const std::vector<Rule *> & rules) {
    std::vector<std::vector<Rule *>> groups;
    bool open = false;
    for (Rule * r : rules) {
        if (isa<TransformRule>(r)) {
            open = false;
        } else if (isa<ConversionRule>(r)) {
            if (!open) groups.emplace_back();
            open = true;
            groups.back().push_back(r);
        }
    }
    return groups;
}

}

namespace ldml {

const char * NFD_StatusName(NFD_Status s) {
    switch (s) {
        case NFD_Status::Always: return "Always";
        case NFD_Status::Preserving: return "Preserving";
        case NFD_Status::NotGuaranteed: return "NotGuaranteed";
    }
    return "?";
}

namespace {

NFD_StatusReport report(NFD_Status s, bool filterSafe, std::string reason) {
    NFD_StatusReport r;
    r.status = s;
    r.filterSafe = filterSafe;
    r.reason = std::move(reason);
    return r;
}

//  Does a full case mapping (a string override property) keep NFD (see
//  nfd_status.h)?  If not, the reason.
bool caseMappingPreserves(UCD::property_t p, std::string & reason) {
    const CombiningClasses & C = CombiningClasses::get();
    auto * const obj = cast<UCD::StringOverridePropertyObject>(UCD::getPropertyObject(p));
    const UnicodeSet mapped = (UnicodeSet(0, UCD::UNICODE_MAX) - obj->GetReflexiveSet()) & C.nfdStable;
    auto cccOf = [&](UCD::codepoint_t c) {return C.maxMark(UnicodeSet(c, c));};
    for (const auto & range : mapped) {
        for (UCD::codepoint_t c = range.first; c <= range.second; c++) {
            const std::u32string m = obj->GetU32StringValue(c);
            const std::string at = "the mapping of " + hexCodepoint(c);
            unsigned prev = 0;
            for (char32_t x : m) {
                if (!C.nfdStable.contains(x)) {
                    reason = at + " has " + hexCodepoint(x) + ", which has a canonical decomposition";
                    return false;
                }
                const unsigned k = cccOf(x);
                if (k != 0 && prev > k) {
                    reason = at + " is not in canonical order";
                    return false;
                }
                prev = k;
            }
            if (m.empty()) continue;
            const unsigned k = cccOf(c);
            const unsigned first = cccOf(m.front());
            const unsigned last = cccOf(m.back());
            const bool ok = k == 0 ? (first == 0 && last == 0) : ((first == 0 || first >= k) && (last == 0 || last <= k));
            if (!ok) {
                reason = at + " (ccc " + std::to_string(k) + ") begins or ends with a mark of another class";
                return false;
            }
        }
    }
    return true;
}

//  A step's status within a filter (nullptr: none).
NFD_StatusReport withinFilter(NFD_StatusReport r, const UnicodeSet * filter) {
    if (filter == nullptr || r.status == NFD_Status::NotGuaranteed) return r;
    const CombiningClasses & C = CombiningClasses::get();
    if (r.filterSafe || C.maxMark(UnicodeSet(0, UCD::UNICODE_MAX) - *filter) == 0) {
        r.status = NFD_Status::Preserving;
        return r;
    }
    return report(NFD_Status::NotGuaranteed, false, "within a filter that leaves out marks: " + r.reason);
}

std::string readFile(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}

NFD_StatusReport NFD_StatusAnalyzer::builtIn(const TransformEntry * entry) {
    const std::string & n = entry->canonicalName;
    if (n == "Any-NFD") return report(NFD_Status::Always, true, "Any-NFD");
    if (n == "Any-NFKD") return report(NFD_Status::Always, false, "Any-NFKD");
    if (n == "Any-Remove") return report(NFD_Status::Always, false, "Any-Remove");
    if (n == "Any-Name" || n.compare(0, 8, "Any-Hex/") == 0 || n == "Any-Hex") {
        return report(NFD_Status::Always, true, n + " (ASCII)");
    }
    if (n == "Any-Null" || n == "Any-FCD") return report(NFD_Status::Preserving, true, n);
    std::vector<UCD::property_t> mappings;
    if (n == "Any-Lower") mappings = {UCD::lc};
    else if (n == "Any-Upper") mappings = {UCD::uc};
    else if (n == "Any-Title") mappings = {UCD::tc, UCD::lc};
    else if (n == "Any-CaseFold") mappings = {UCD::cf};
    if (!mappings.empty()) {
        for (UCD::property_t p : mappings) {
            std::string why;
            if (!caseMappingPreserves(p, why)) return report(NFD_Status::NotGuaranteed, false, n + ": " + why);
        }
        return report(NFD_Status::Preserving, true, n);
    }
    return report(NFD_Status::NotGuaranteed, false, n);
}

NFD_StatusReport NFD_StatusAnalyzer::invoked(const TransformID & id) {
    const TransformEntry * const entry = mRegistry.lookup(id.getText());
    if (entry == nullptr) return report(NFD_Status::NotGuaranteed, false, id.getText() + " (unknown)");
    NFD_StatusReport r = Determine_NFD_Status(entry);
    if (!entry->isBuiltIn()) r.reason = entry->canonicalName + (r.reason.empty() ? "" : ": " + r.reason);
    return r;
}

NFD_StatusReport NFD_StatusAnalyzer::Determine_NFD_Status(const std::string & name) {
    const TransformEntry * const entry = mRegistry.lookup(name);
    if (entry == nullptr) return report(NFD_Status::NotGuaranteed, false, name + " (unknown)");
    return Determine_NFD_Status(entry);
}

NFD_StatusReport NFD_StatusAnalyzer::Determine_NFD_Status(const TransformEntry * entry) {
    const auto f = mCache.find(entry);
    if (f != mCache.end()) return f->second;
    if (entry->isBuiltIn()) return mCache[entry] = builtIn(entry);
    if (mActive[entry]) return report(NFD_Status::NotGuaranteed, false, "invokes itself");
    mActive[entry] = true;
    NFD_StatusReport r;
    try {
        const std::vector<Rule *> parsed = parseTransformRules(extractTRules(readFile(entry->file)));
        const std::vector<Rule *> rules = entry->direction == TransformDirection::Forward ? ExtractForwardRules(parsed)
                                                                                          : ExtractReverseBackwardRules(parsed);
        r = Determine_NFD_Status(rules);
    } catch (const std::exception & e) {
        r = report(NFD_Status::NotGuaranteed, false, std::string("rules not read: ") + e.what());
    }
    mActive[entry] = false;
    return mCache[entry] = r;
}

std::vector<NFD_StatusReport> NFD_StatusAnalyzer::stepStatuses(const std::vector<Rule *> & forwardRules,
                                                                std::vector<size_t> * transformSteps) {
    CharSetAnalysis sets;
    //  The global filter, and the filter carried through a block of transform
    //  rules (see nfd_status.h).
    std::unique_ptr<UnicodeSet> global;
    std::unique_ptr<UnicodeSet> carried;
    bool blockStart = true;
    std::vector<NFD_StatusReport> steps;
    std::vector<Rule *> group;
    auto endGroup = [&]() {
        if (group.empty()) return;
        NFD_PreservationFailure failure;
        NFD_StatusReport r = Is_NFD_Preserving(group, &failure)
            ? report(NFD_Status::Preserving, true, "")
            : report(NFD_Status::NotGuaranteed, false, "step " + std::to_string(steps.size() + 1) + " (conversion group): " +
                     failure.reason + ": " + printRule(failure.rule));
        steps.push_back(withinFilter(r, global.get()));
        group.clear();
        blockStart = true;
        carried.reset();
    };
    for (Rule * rule : forwardRules) {
        if (const FilterRule * fr = dyn_cast<FilterRule>(rule)) {
            if (!fr->isInverse()) global.reset(new UnicodeSet(sets.setOf(fr->getFilterSet(), true)));
        } else if (const TransformRule * tr = dyn_cast<TransformRule>(rule)) {
            endGroup();
            if (transformSteps) transformSteps->push_back(steps.size());
            std::unique_ptr<UnicodeSet> filter;
            if (tr->getForwardFilter()) {
                filter.reset(new UnicodeSet(sets.setOf(tr->getForwardFilter(), true)));
                if (blockStart && !global) carried.reset(new UnicodeSet(*filter));
            } else if (carried) {
                filter.reset(new UnicodeSet(*carried));
            }
            blockStart = false;
            if (global) {
                if (filter) *filter = *filter & *global; else filter.reset(new UnicodeSet(*global));
            }
            NFD_StatusReport r = tr->hasForward() ? withinFilter(invoked(tr->getForwardID()), filter.get())
                                                  : report(NFD_Status::Preserving, true, "Null");
            r.reason = "step " + std::to_string(steps.size() + 1) + " (" + tr->getForwardID().getText() + "): " + r.reason;
            steps.push_back(r);
        } else if (isa<ConversionRule>(rule)) {
            group.push_back(rule);
        }
    }
    endGroup();
    return steps;
}

//  The status of the text after the first n steps.
NFD_StatusReport NFD_StatusAnalyzer::combine(const std::vector<NFD_StatusReport> & steps, size_t n) {
    //  The last step that is Always, and the first step after it that is
    //  NotGuaranteed.
    size_t lastAlways = n;
    for (size_t i = 0; i < n; i++) if (steps[i].status == NFD_Status::Always) lastAlways = i;
    const size_t from = lastAlways == n ? 0 : lastAlways + 1;
    bool safe = true;
    for (size_t i = 0; i < n; i++) safe = safe && steps[i].status != NFD_Status::NotGuaranteed && steps[i].filterSafe;
    for (size_t i = from; i < n; i++) {
        if (steps[i].status == NFD_Status::NotGuaranteed) return report(NFD_Status::NotGuaranteed, false, steps[i].reason);
    }
    if (lastAlways != n) return report(NFD_Status::Always, safe, steps[lastAlways].reason);
    return report(NFD_Status::Preserving, safe, "");
}

NFD_StatusReport NFD_StatusAnalyzer::Determine_NFD_Status(const std::vector<Rule *> & forwardRules) {
    const std::vector<NFD_StatusReport> steps = stepStatuses(forwardRules, nullptr);
    return combine(steps, steps.size());
}

std::vector<NFD_Status> NFD_StatusAnalyzer::StatusBeforeTransformRules(const std::vector<Rule *> & forwardRules) {
    std::vector<size_t> at;
    const std::vector<NFD_StatusReport> steps = stepStatuses(forwardRules, &at);
    std::vector<NFD_Status> before;
    for (size_t n : at) before.push_back(combine(steps, n).status);
    return before;
}

}

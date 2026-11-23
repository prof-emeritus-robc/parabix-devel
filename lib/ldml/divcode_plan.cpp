/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/divcode_plan.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <re/adt/adt.h>
#include <re/adt/re_name.h>
#include <ldml/transform_rules.h>
#include <ldml/transform_rules_printer.h>
#include "charset_analysis.h"

using namespace re;
using namespace llvm;

namespace ldml {

bool DivCodeFamilies = true;

namespace {

//  The blocks (cp >> shift) a set spans.
std::set<UCD::codepoint_t> blocksOf(const UCD::UnicodeSet & set, unsigned shift) {
    std::set<UCD::codepoint_t> blocks;
    for (const auto & range : set) {
        for (UCD::codepoint_t b = range.first >> shift; b <= (range.second >> shift); b++) blocks.insert(b);
    }
    return blocks;
}

size_t blockCount(const UCD::UnicodeSet & set, unsigned shift) {
    size_t n = 0;
    UCD::codepoint_t last = ~0u;
    for (const auto & range : set) {
        UCD::codepoint_t lo = range.first >> shift;
        const UCD::codepoint_t hi = range.second >> shift;
        if (lo == last) lo++;
        if (hi >= lo) n += hi - lo + 1;
        last = hi;
    }
    return n;
}

UCD::UnicodeSet blockRange(UCD::codepoint_t block, unsigned shift) {
    return UCD::UnicodeSet(block << shift, (block << shift) | ((1u << shift) - 1));
}

//  Does a set include strings (sequences of two or more characters)?
bool hasStrings(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && hasStrings(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (const Seq * s = dyn_cast<Seq>(a); s && s->size() > 1) return true;
            if (hasStrings(a)) return true;
        }
    }
    return false;
}

//  Does a set include the text boundary (possibly through variables)?
bool includesBoundary(const RE * re) {
    if (isBoundary(re)) return true;
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && includesBoundary(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) if (includesBoundary(a)) return true;
    }
    return false;
}

//  The items of a context, a fixed sequence of sets (through variables), in
//  the order written: each its set and whether it includes the text boundary
//  (the boundary alone: an empty set with it).  False (with the reason) if it
//  is not such a sequence.  A null context has no items.
bool contextItems(RE * re, CharSetAnalysis & sets, std::vector<std::pair<UCD::UnicodeSet, bool>> & items, std::string & why,
                  UCD::UnicodeSet * through = nullptr) {
    if (re == nullptr) return true;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        //  (A before context S T*, T repeated next to the text: through, with
        //  the items before it.)
        auto end = seq->end();
        if (through && seq->size() >= 2) {
            if (const Rep * rep = dyn_cast<Rep>(seq->back())) {
                if (rep->getLB() == 0 && rep->getUB() == Rep::UNBOUNDED_REP && CharSetAnalysis::isSet(rep->getRE())
                        && !hasStrings(rep->getRE())) {
                    *through = sets.setOf(rep->getRE(), false);
                    --end;
                }
            }
        }
        for (auto i = seq->begin(); i != end; ++i) if (!contextItems(*i, sets, items, why)) return false;
        return true;
    }
    if (isBoundary(re)) {
        items.emplace_back(UCD::UnicodeSet(), true);
        return true;
    }
    if (CharSetAnalysis::isSet(re)) {
        if (hasStrings(re)) {
            why = "context sets with strings";
            return false;
        }
        items.emplace_back(sets.setOf(re, false), includesBoundary(re));
        return true;
    }
    if (Name * n = dyn_cast<Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition()) return contextItems(n->getDefinition(), sets, items, why);
    }
    why = "contexts that are not fixed sequences of sets (repetitions, alternatives of sequences, ...)";
    return false;
}

std::string hex(uint64_t v, int width = 0) {
    char b[24];
    snprintf(b, sizeof(b), "%0*llx", width, static_cast<unsigned long long>(v));
    return b;
}

void appendSet(std::string & s, const UCD::UnicodeSet & set) {
    s += '[';
    for (const auto & range : set) s += hex(range.first) + "-" + hex(range.second) + ",";
    s += ']';
}

uint64_t fnv1a(const std::string & s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

}

unsigned DivCodePlan::codeOf(UCD::codepoint_t cp) const {
    const auto f = std::lower_bound(blocks.begin(), blocks.end(), cp >> shift);
    return (f != blocks.end() && *f == (cp >> shift)) ? static_cast<unsigned>(f - blocks.begin()) + 1 : 0;
}

UCD::UnicodeSet DivCodePlan::pseudoSet(const UCD::UnicodeSet & set) const {
    UCD::UnicodeSet pseudo;
    const UCD::codepoint_t low = (1u << shift) - 1;
    for (size_t i = 0; i < blocks.size(); i++) {
        const UCD::UnicodeSet within = set & blockRange(blocks[i], shift);
        for (const auto & range : within) {
            pseudo = pseudo + UCD::UnicodeSet(((i + 1) << shift) | (range.first & low), ((i + 1) << shift) | (range.second & low));
        }
    }
    return pseudo;
}

UCD::UnicodeSet DivCodePlan::pseudoTest(const UCD::UnicodeSet & set) const {
    bool within = true;
    for (UCD::codepoint_t b : blocksOf(set, shift)) {
        if (!std::binary_search(blocks.begin(), blocks.end(), b)) {within = false; break;}
    }
    if (within) return pseudoSet(set);
    const UCD::UnicodeSet all(0, static_cast<UCD::codepoint_t>(((blocks.size() + 1) << shift) - 1));
    return all - pseudoSet(~set);
}

//  The converted items of a plan, and which characters of the converted rules
//  propagate (see DivCodePlan::propagating).
static void classifyConverted(DivCodePlan & plan) {
    auto sameItem = [](const DivContextItem & a, const DivContextItem & b) {
        return a.kind == b.kind && a.set == b.set && a.negated == b.negated && a.runEnd == b.runEnd && a.outside == b.outside && a.external == b.external
               && a.guardEnd == b.guardEnd && a.throughKind == b.throughKind && a.through == b.through;
    };
    auto sameContext = [&](const DivContext & a, const DivContext & b) {
        if (a.items.size() != b.items.size()) return false;
        for (size_t i = 0; i < a.items.size(); i++) if (!sameItem(a.items[i], b.items[i])) return false;
        return true;
    };
    UCD::UnicodeSet chars;
    for (const DivRule & d : plan.rules) {
        if (!d.converted && (d.family < 0 || !d.before.present())) continue;
        const DivContextItem & item = d.before.items[0];
        bool found = false;
        for (const DivContextItem & c : plan.convertedItems) found = found || sameItem(c, item);
        if (!found) plan.convertedItems.push_back(item);
        if (d.converted) chars = chars + d.text[0];
    }
    const size_t n = plan.convertedItems.size();
    plan.consumerIn.assign(n, UCD::UnicodeSet());
    if (n == 0) return;
    //  What a later before context sees of an outcome: "D" (deleted), or
    //  whether its last character is in the set of each item.
    auto signatureOf = [&](const std::u32string & result) {
        if (result.empty()) return std::string("D");
        std::string sig;
        for (const DivContextItem & item : plan.convertedItems) sig += item.set.contains(lastSeen(item, result)) ? '1' : '0';
        return sig;
    };
    //  The families: their rules' results (one of which applies).
    for (size_t f = 0; f < plan.convertedFamilies.size(); f++) {
        DivConvertedFamily & family = plan.convertedFamilies[f];
        std::set<std::string> sigs;
        for (const DivRule & d : plan.rules) if (d.family == static_cast<int>(f)) sigs.insert(signatureOf(d.result));
        if (sigs.empty()) continue;
        family.propagating = sigs.size() > 1;
        family.deleted = !family.propagating && *sigs.begin() == "D";
        family.in.assign(n, false);
        if (!family.propagating && !family.deleted) for (size_t i = 0; i < n; i++) family.in[i] = (*sigs.begin())[i] == '1';
    }
    if (chars.empty()) return;
    //  (Very many characters: all taken as propagating.)
    if (chars.count() > 20000) {
        plan.propagating = chars;
        return;
    }
    const UCD::UnicodeSet all(0, 0x10FFFF);
    auto signature = signatureOf;
    for (const auto & range : chars) {
        for (UCD::codepoint_t c = range.first; c <= range.second; c++) {
            //  The converted rules on c (no other rule applies at a dependent
            //  position): their results; the character itself, unless they have
            //  the same after context and their before items hold in every state
            //  (for every character before, and at the text boundary).
            std::vector<const DivRule *> rules;
            for (const DivRule & d : plan.rules) if (d.converted && d.text[0].contains(c)) rules.push_back(&d);
            std::set<std::string> sigs;
            UCD::UnicodeSet covered;
            bool boundary = false, sameAfter = true;
            for (const DivRule * d : rules) {
                sigs.insert(signature(d->result));
                const DivContextItem & item = d->before.items[0];
                covered = covered + (item.negated ? all - item.set : item.set);
                boundary = boundary || item.negated;
                sameAfter = sameAfter && sameContext(d->after, rules.front()->after);
            }
            if (!(sameAfter && boundary && covered == all)) sigs.insert(signature(std::u32string(1, static_cast<char32_t>(c))));
            if (sigs.size() > 1) {
                plan.propagating.insert(c);
            } else if (*sigs.begin() == "D") {
                plan.consumerDeleted.insert(c);
            } else {
                const std::string & sig = *sigs.begin();
                for (size_t i = 0; i < n; i++) if (sig[i] == '1') plan.consumerIn[i].insert(c);
            }
        }
    }
}

DivCodePlan planDivCodeGroup(const TransformStep & step, const DivCodeOptions & options) {
    DivCodePlan plan;
    plan.shift = options.shift;
    const unsigned s = options.shift;
    std::map<std::string, size_t> problems;
    auto problem = [&](const std::string & what) {problems[what]++;};

    if (step.kind != TransformStep::Kind::Conversion) {
        plan.problems.push_back("not a conversion group");
        return plan;
    }
    //  (A group without string rules: only with converted rules, see below.)
    if (!step.captureReplacements.empty()) problem("rules with captures");
    if (!step.insertions.empty()) problem("insertion rules");
    if (!step.deferred.empty()) problem("deferred rules");
    if (!step.prefixRuns.empty()) problem("prefix runs");
    if (!step.chainRevisitRules.empty()) problem("chain revisits");
    if (!step.markRunEditRules.empty()) problem("mark run edits");
    if (step.hasFrontLetters || step.frontLettersEnd) problem("front-letter revisits");
    //  The stages of the string rules: one-way overlaps in an acyclic graph
    //  (see DivRule::stage); others are not eligible.
    std::vector<unsigned> stageOf(step.stringRules.size(), 1);
    std::vector<std::vector<size_t>> stagePredecessors(step.stringRules.size());
    plan.stages = step.stringRules.empty() ? 0 : 1;
    if (step.stringRules.size() > 1) {
        const StringRuleOverlaps o = analyzeStringRuleOverlaps(step);
        if (!o.pairs.empty()) {
            const std::string counts = " (" + std::to_string(o.oneWay) + " one-way, " + std::to_string(o.twoWay) + " two-way, "
                                       + std::to_string(o.self) + " self)";
            if (o.twoWay > 0 || o.self > 0) {
                problem("string rule overlaps, two-way or self" + counts);
            } else if (!o.acyclic) {
                problem("string rule overlaps, cyclic" + counts);
            } else {
                plan.overlapPairs = o.oneWay;
                stagePredecessors = o.predecessors;
                for (size_t r : o.order) {
                    for (size_t q : o.predecessors[r]) stageOf[r] = std::max(stageOf[r], stageOf[q] + 1);
                    plan.stages = std::max(plan.stages, stageOf[r]);
                }
                if (plan.stages > options.maxStages) {
                    problem("string rule overlaps of more than " + std::to_string(options.maxStages) + " stages" + counts);
                }
            }
        }
    }

    CharSetAnalysis sets;
    std::set<UCD::codepoint_t> blocks;
    std::vector<std::pair<UCD::UnicodeSet, bool>> contextSets;   // (the set tested, explicit?) for the alphabet
    std::vector<UCD::UnicodeSet> throughSets;                     // the repeated sets of before items (§12 step 2)

    //  An item of a context: its set and whether it holds at the text boundary.
    auto classifyItem = [&](const UCD::UnicodeSet & set, bool boundary, bool before, DivContextItem & item) {
        item.runEnd = marksFilterRunEnd(set);
        item.outside = marksFilterRunOutside(set);
        if (item.outside && (before || boundary)) problem("before contexts of characters outside the filter");
        const UCD::UnicodeSet chars = item.outside ? set - UCD::UnicodeSet(FilterRunOutsideMark) : set;
        //  With the text boundary: tested as the negation of the complement.
        item.negated = boundary;
        item.set = boundary ? ~chars : chars;
        const size_t direct = blockCount(item.set, s), complement = blockCount(~item.set, s);
        if (std::min(direct, complement) <= options.maxSetBlocks) {
            //  A set given by its complement includes the characters of code 0,
            //  and so the pseudo-codepoint 0 of the zero bits beyond the end of
            //  the text: an after context would match there, where ICU's set
            //  (without the boundary, as [^X$]) does not.
            item.guardEnd = !before && !boundary && direct > complement;
            item.kind = DivContextItem::Kind::Explicit;
            contextSets.emplace_back(direct <= complement ? item.set : ~item.set, true);
        } else {
            item.kind = DivContextItem::Kind::External;
            item.guardEnd = !before && !boundary && item.set.contains(0);
            auto f = std::find(plan.externalSets.begin(), plan.externalSets.end(), item.set);
            item.external = f - plan.externalSets.begin();
            if (f == plan.externalSets.end()) plan.externalSets.push_back(item.set);
        }
    };

    //  A context, its items classified (the alphabet completed later).
    //  (converted: whether the before context is matched against the converted
    //  text; allowed only for single-character rules, phase 1.)
    auto classify = [&](RE * re, RE * engineBefore, bool before, DivContext & context, bool * converted = nullptr,
                        bool single = false) {
        std::vector<std::pair<UCD::UnicodeSet, bool>> items;
        std::string why;
        UCD::UnicodeSet through;
        if (!contextItems(re, sets, items, why, before ? &through : nullptr)) {
            problem(why);
            return;
        }
        if (items.empty()) return;
        if (before) {
            if (!options.convertedBefore) {
                problem("before contexts (no check of the converted text)");
            } else if (options.convertedBefore(step, engineBefore) && options.convertedContexts) {
                if (!single && !converted) {
                    problem("string rules with before contexts matched against the converted text");
                } else if (items.size() > 1) {
                    problem("before contexts of two or more items matched against the converted text");
                } else if (converted) {
                    *converted = true;
                }
            }
            //  (From the item next to the text outward.)
            std::reverse(items.begin(), items.end());
            //  (A repeated set next to the text: only for one item matched
            //  against the converted text, which the Converted kernel passes
            //  over, §12 step 2.)
            //  (A problem leaves the context without items: then not converted.)
            if (!through.empty() && !(converted && *converted && items.size() == 1)) {
                problem("contexts that are not fixed sequences of sets (a repeated set in a before context "
                        "not matched against the converted text, or of several items)");
                if (converted) *converted = false;
                return;
            }
            //  (In UTF-8 mode, the positions of the repeated set's characters
            //  are not those of their code units: not yet.)
            if (!through.empty() && options.utf8) {
                problem("a repeated set in a before context (UTF-8 mode)");
                if (converted) *converted = false;
                return;
            }
        }
        for (size_t i = 0; i + 1 < items.size(); i++) {
            if (items[i].second && items[i].first.empty()) {
                problem("the text boundary within a context");
                return;
            }
            //  (Beyond an inner item, a character must match: not the boundary.)
            items[i].second = false;
        }
        for (const auto & it : items) {
            context.items.emplace_back();
            classifyItem(it.first, it.second, before, context.items.back());
        }
        if (!through.empty()) {
            DivContextItem & item = context.items.front();
            item.through = through;
            const size_t direct = blockCount(through, s), complement = blockCount(~through, s);
            if (std::min(direct, complement) <= options.maxSetBlocks) {
                item.throughKind = DivContextItem::Kind::Explicit;
                contextSets.emplace_back(direct <= complement ? through : ~through, true);
            } else {
                item.throughKind = DivContextItem::Kind::External;
                auto f = std::find(plan.externalSets.begin(), plan.externalSets.end(), through);
                item.throughExternal = f - plan.externalSets.begin();
                if (f == plan.externalSets.end()) plan.externalSets.push_back(through);
            }
            throughSets.push_back(through);
        }
    };

    auto literal = [&](const std::u32string & result) {
        for (char32_t c : result) {
            if (c == StringRuleKept || c == StringRuleDeleted) return false;
        }
        return true;
    };

    //  The rules before splitting: (text, result, contexts, source).
    struct Source {
        std::vector<UCD::UnicodeSet> text;
        std::u32string result;
        DivContext before, after;
        bool converted = false;
        const ConversionRule * rule = nullptr;
        int stringRule = -1, subgroup = -1;
        int family = -1;                    // a covering family applied (DivRule::family)
        int synthetic = -1;                 // a rule completing a family: the source of a rule of it
    };
    std::vector<Source> sources;

    for (size_t r = 0; r < step.stringRules.size(); r++) {
        const StringRule & sr = step.stringRules[r];
        if (!sr.run.empty()) {problem("string rules with runs"); continue;}
        if (sr.kept || !literal(sr.replacement)) {problem("string rules keeping characters of their texts"); continue;}
        if (sr.selfOverlapping) problem("self-overlapping string rules");
        Source src;
        src.text = sr.text;
        bool ok = !src.text.empty();
        for (UCD::UnicodeSet & t : src.text) {
            t = t - UCD::UnicodeSet(StringRuleDeleted);
            ok &= !t.empty();
        }
        if (!ok) {problem("string rules with empty text positions"); continue;}
        for (const UCD::UnicodeSet & t : src.text) {
            if (marksFilterRunEnd(t)) {problem("texts at the end of a filtered run"); ok = false; break;}
        }
        if (options.utf8) {
            //  (The lengths of UTF-8 characters: 1, 2, 3, 4.)
            static const UCD::UnicodeSet lengths[] = {UCD::UnicodeSet(0, 0x7F), UCD::UnicodeSet(0x80, 0x7FF),
                                                      UCD::UnicodeSet(0x800, 0xFFFF), UCD::UnicodeSet(0x10000, 0x10FFFF)};
            for (size_t i = 1; i < src.text.size() && ok; i++) {
                unsigned n = 0;
                for (const UCD::UnicodeSet & l : lengths) n += src.text[i].intersects(l) ? 1 : 0;
                if (n > 1) {problem("text positions of characters of different UTF-8 lengths"); ok = false;}
            }
        }
        if (!ok) continue;
        src.result = sr.replacement;
        src.rule = sr.rule;
        src.stringRule = static_cast<int>(r);
        classify(sr.before, sr.engineBefore, true, src.before, &src.converted);
        classify(sr.after, nullptr, false, src.after);
        sources.push_back(std::move(src));
    }
    //  The families of the string rules with before contexts matched against
    //  the converted text (see DivConvertedFamily), and their sources.
    std::vector<std::vector<size_t>> familyMembers;
    {
        //  Where an item holds: the characters, and whether the text boundary.
        struct Space {
            UCD::UnicodeSet chars;
            bool boundary;
        };
        const UCD::UnicodeSet all(0, 0x10FFFF);
        auto holds = [&](const DivContext & context) {
            if (!context.present()) return Space{all, true};
            const DivContextItem & item = context.items[0];
            return item.negated ? Space{~item.set, true} : Space{item.set, false};
        };
        std::vector<bool> done(sources.size(), false);
        for (size_t i = 0; i < sources.size(); i++) {
            if (sources[i].stringRule < 0 || !sources[i].converted || done[i]) continue;
            familyMembers.emplace_back();
            std::vector<size_t> & members = familyMembers.back();
            for (size_t j = 0; j < sources.size(); j++) {
                if (sources[j].stringRule >= 0 && sources[j].text == sources[i].text) {
                    members.push_back(j);
                    done[j] = true;
                }
            }
            DivConvertedFamily f;
            for (size_t j : members) f.stringRules.push_back(sources[j].stringRule);
            for (size_t j : members) {
                const Source & m = sources[j];
                if (m.before.present() && !m.converted) f.reason = "a rule with a before context matched against the text itself";
                if (m.before.items.size() > 1) f.reason = "a before context of several items";
            }
            //  (An after context of several items: the rules with the same after
            //  context, and those without one, cover as a group.  Sufficient,
            //  not necessary.)
            bool several = false;
            for (size_t j : members) several |= sources[j].after.items.size() > 1;
            auto sameItems = [](const DivContext & a, const DivContext & b) {
                if (a.items.size() != b.items.size()) return false;
                for (size_t k = 0; k < a.items.size(); k++) {
                    if (!(a.items[k].set == b.items[k].set) || a.items[k].negated != b.items[k].negated) return false;
                }
                return true;
            };
            if (f.reason.empty() && several) {
                f.covering = true;
                for (size_t j : members) {
                    Space before{UCD::UnicodeSet(), false};
                    for (size_t q : members) {
                        if (sources[q].after.present() && !sameItems(sources[q].after, sources[j].after)) continue;
                        const Space b = holds(sources[q].before);
                        before.chars = before.chars + b.chars;
                        before.boundary |= b.boundary;
                    }
                    if (!(before.chars == all && before.boundary)) {
                        f.covering = false;
                        f.reason = "an after context of several items, the before contexts of its rules miss "
                                   + std::to_string((all - before.chars).count()) + " characters"
                                   + (before.boundary ? "" : " and the text boundary");
                        break;
                    }
                }
            } else if (f.reason.empty()) {
                //  The regions of the characters after the text that the after
                //  contexts distinguish, and the end of the text.
                std::vector<UCD::UnicodeSet> regions{all};
                for (size_t j : members) {
                    const Space a = holds(sources[j].after);
                    std::vector<UCD::UnicodeSet> refined;
                    for (const UCD::UnicodeSet & r : regions) {
                        const UCD::UnicodeSet in = r & a.chars, out = r - a.chars;
                        if (!in.empty()) refined.push_back(in);
                        if (!out.empty()) refined.push_back(out);
                    }
                    regions = std::move(refined);
                }
                auto check = [&](const std::function<bool (const Space &)> & afterHolds, const std::string & where) {
                    Space before{UCD::UnicodeSet(), false};
                    bool any = false;
                    for (size_t j : members) {
                        if (!afterHolds(holds(sources[j].after))) continue;
                        any = true;
                        const Space b = holds(sources[j].before);
                        before.chars = before.chars + b.chars;
                        before.boundary |= b.boundary;
                    }
                    if (!any || (before.chars == all && before.boundary)) return true;
                    const UCD::UnicodeSet missing = all - before.chars;
                    f.reason = where + ", the before contexts miss " + std::to_string(missing.count()) + " characters"
                               + (before.boundary ? "" : " and the text boundary");
                    return false;
                };
                f.covering = true;
                for (const UCD::UnicodeSet & r : regions) {
                    if (!check([&](const Space & a) {return r.subset(a.chars);}, "before some characters")) {f.covering = false; break;}
                }
                if (f.covering) f.covering = check([](const Space & a) {return a.boundary;}, "at the end of the text");
            }
            plan.convertedFamilies.push_back(std::move(f));
        }
    }
    std::vector<bool> subgroupConverted;
    for (size_t g = 0; g < step.subgroups.size(); g++) {
        const CharMapSubgroup & sg = step.subgroups[g];
        DivContext before, after;
        bool converted = false;
        classify(sg.before, sg.engineBefore, true, before, &converted, true);
        //  An after context that is not a fixed sequence of sets (repetitions,
        //  sets with strings, lookaheads): computed by the regular expression
        //  engine as the anchors it holds after (§12 step 3; in Unicode mode).
        std::vector<std::pair<UCD::UnicodeSet, bool>> fixedItems;
        std::string notFixed;
        if (sg.after && sg.engineAfter && !contextItems(sg.after, sets, fixedItems, notFixed)) {
            if (options.utf8) {
                problem("an after context computed by the regular expression engine (UTF-8 mode)");
            } else {
                DivContextItem item;
                item.kind = DivContextItem::Kind::Stream;
                item.external = plan.afterStreams.size();
                plan.afterStreams.push_back(sg.engineAfter);
                plan.afterStreamKeys.push_back(sg.sourceRules.empty() ? std::to_string(g) : printRule(sg.sourceRules.front()));
                after.items.push_back(item);
            }
        } else {
            classify(sg.after, nullptr, false, after);
        }
        plan.subgroupContexts.emplace_back(before, after);
        subgroupConverted.push_back(converted);
        for (const CharMapping & m : sg.mappings) {
            const UCD::UnicodeSet chars = m.chars - UCD::UnicodeSet(StringRuleDeleted);
            if (chars.empty()) continue;
            if (!literal(m.replacement)) {problem("single-character rules with marks in their results"); continue;}
            Source src;
            src.text = {chars};
            src.result = m.replacement;
            src.before = before;
            src.after = after;
            src.converted = converted;
            src.subgroup = static_cast<int>(g);
            sources.push_back(std::move(src));
        }
    }
    plan.sourceRules = sources.size();
    //  The repeated sets of before items (§12 step 2): a text of their
    //  characters gives their characters or nothing; another text, nothing or
    //  some character not in them (the item then tests its last such: the
    //  ones after it are passed over).  So the positions of their characters
    //  in the converted text are those of the text's, or within a result.
    for (const UCD::UnicodeSet & t : throughSets) {
        for (const Source & src : sources) {
            bool anyT = false, allT = true;
            for (const UCD::UnicodeSet & u : src.text) {
                anyT |= u.intersects(t);
                allT &= u.subset(t);
            }
            bool resultT = true;
            for (char32_t c : src.result) resultT &= t.contains(c);
            if (!((!anyT && (src.result.empty() || !resultT)) || (allT && resultT))) {
                problem("conversions changing the repeated set of a before context");
                break;
            }
        }
    }

    //  The earlier rules each source yields to.
    auto earlierOf = [&](const ConversionRule * e) {
        DivEarlier d;
        for (size_t r = 0; r < step.stringRules.size(); r++) {
            if (step.stringRules[r].rule == e) {
                d.stringRule = static_cast<int>(r);
                return d;
            }
        }
        for (size_t g = 0; g < step.subgroups.size(); g++) {
            const auto & of = step.subgroups[g].sourceRules;
            if (std::find(of.begin(), of.end(), e) == of.end()) continue;
            d.subgroup = static_cast<int>(g);
            const std::vector<PatternItem> text = patternItems(e->getSourceSide(Direction::Forward)->getText());
            if (!text.empty()) d.chars = text.front().chars;
            return d;
        }
        problem("rules yielding to rules outside the group");
        return d;
    };
    std::vector<std::vector<DivEarlier>> yields(sources.size());
    for (size_t i = 0; i < sources.size(); i++) {
        const Source & src = sources[i];
        const std::vector<const ConversionRule *> * earlier = nullptr;
        if (src.stringRule >= 0 && static_cast<size_t>(src.stringRule) < step.stringPriority.size()) {
            earlier = &step.stringPriority[src.stringRule];
        } else if (src.subgroup >= 0 && static_cast<size_t>(src.subgroup) < step.subgroupPriority.size()) {
            earlier = &step.subgroupPriority[src.subgroup];
        }
        if (earlier) {
            for (const ConversionRule * e : *earlier) {
                yields[i].push_back(earlierOf(e));
                const DivEarlier & d = yields[i].back();
                if (d.subgroup >= 0 && subgroupConverted[d.subgroup]) {
                    problem("rules yielding to rules with before contexts matched against the converted text");
                }
            }
        }
    }

    //  Completion (doc/ldml-divcode-plan.md §11, fallback 2): a family whose
    //  before contexts miss some converted characters (or the text boundary),
    //  for an after region, is completed by a synthetic rule for them, whose
    //  result is what the rules of the group give its text there.  That result
    //  is evaluated as ICU applies the rules, from the first position of the
    //  text: the rule matching at a position (its text, its after context on
    //  the text and the after region, its before context on the characters
    //  written so far, or on the missing characters before them), less those
    //  yielding to another rule matching there; none, the character is kept.
    //  The evaluation fails (the family is left as it is) where a test is not
    //  the same for all the characters of a region, a context has more than one
    //  item beyond the text, or a match extends beyond the text.
    if (DivCodeFamilies) {
        const UCD::UnicodeSet all(0, 0x10FFFF);
        //  Where an item holds: the characters, and whether the boundary.
        auto holdsSet = [&](const DivContextItem & item, UCD::UnicodeSet & chars, bool & boundary) {
            chars = item.negated ? ~item.set : item.set;
            boundary = item.negated;
        };
        for (size_t f = 0; f < plan.convertedFamilies.size(); f++) {
            DivConvertedFamily & family = plan.convertedFamilies[f];
            if (family.covering || familyMembers[f].empty()) continue;
            const std::vector<size_t> members = familyMembers[f];
            const Source & front = sources[members.front()];
            //  (A literal text, rules of one before item.)
            std::u32string text;
            bool simple = true;
            for (const UCD::UnicodeSet & t : front.text) {
                if (t.count() != 1) {simple = false; break;}
                text.push_back(t.front().first);
            }
            bool several = false;
            for (size_t j : members) {
                simple &= sources[j].before.items.size() <= 1 && (!sources[j].before.present() || sources[j].converted);
                several |= sources[j].after.items.size() > 1;
            }
            if (!simple) continue;
            //  The after regions, as the items after the text (the characters and
            //  whether the boundary), with the after context of the synthetic
            //  rule: the characters the first items of the after contexts of its
            //  rules distinguish, and the end of the text; with an after context
            //  of several items, each after context of its rules (as the coverage
            //  check groups them).
            struct Region {
                std::vector<std::pair<UCD::UnicodeSet, bool>> items;
                DivContext after;
            };
            std::vector<Region> regions;
            auto sameItems = [](const DivContext & a, const DivContext & b) {
                if (a.items.size() != b.items.size()) return false;
                for (size_t k = 0; k < a.items.size(); k++) {
                    if (!(a.items[k].set == b.items[k].set) || a.items[k].negated != b.items[k].negated) return false;
                }
                return true;
            };
            if (several) {
                for (size_t j : members) {
                    const DivContext & a = sources[j].after;
                    if (!a.present()) continue;
                    bool seen = false;
                    for (const Region & r : regions) seen |= sameItems(r.after, a);
                    if (seen) continue;
                    Region r;
                    r.after = a;
                    for (const DivContextItem & item : a.items) {
                        UCD::UnicodeSet c; bool cb;
                        holdsSet(item, c, cb);
                        r.items.emplace_back(c, cb);
                    }
                    regions.push_back(std::move(r));
                }
            } else {
                std::vector<UCD::UnicodeSet> sets{all};
                for (size_t j : members) {
                    if (!sources[j].after.present()) continue;
                    UCD::UnicodeSet a; bool ab;
                    holdsSet(sources[j].after.items[0], a, ab);
                    std::vector<UCD::UnicodeSet> refined;
                    for (const UCD::UnicodeSet & r : sets) {
                        if (!(r & a).empty()) refined.push_back(r & a);
                        if (!(r - a).empty()) refined.push_back(r - a);
                    }
                    sets = std::move(refined);
                }
                for (const UCD::UnicodeSet & r : sets) {
                    Region region;
                    region.items.emplace_back(r, false);
                    if (!(r == all)) {
                        region.after.items.emplace_back();
                        classifyItem(r, false, false, region.after.items.back());
                    }
                    regions.push_back(std::move(region));
                }
                Region end;
                end.items.emplace_back(UCD::UnicodeSet(), true);
                //  (Only beyond the text: negated, of all characters.)
                end.after.items.emplace_back();
                classifyItem(UCD::UnicodeSet(), true, false, end.after.items.back());
                regions.push_back(std::move(end));
            }
            //  Whether an item holds for all of (chars, boundary): 1, for none: 0,
            //  otherwise -1.
            auto test = [&](const DivContextItem & item, const UCD::UnicodeSet & chars, bool boundary) {
                UCD::UnicodeSet in; bool inBoundary;
                holdsSet(item, in, inBoundary);
                const bool some = !(chars & in).empty() || (boundary && inBoundary);
                const bool every = chars.subset(in) && (!boundary || inBoundary);
                return every ? 1 : some ? -1 : 0;
            };
            std::vector<Source> added;
            std::string why;
            for (const Region & region : regions) {
                //  The before contexts of the rules whose after contexts hold there.
                UCD::UnicodeSet covered; bool coveredBoundary = false; bool any = false;
                for (size_t j : members) {
                    const Source & m = sources[j];
                    if (m.after.present()) {
                        if (several ? !sameItems(m.after, region.after)
                                    : test(m.after.items[0], region.items[0].first, region.items[0].second) != 1) continue;
                    }
                    any = true;
                    if (!m.before.present()) {covered = all; coveredBoundary = true; continue;}
                    UCD::UnicodeSet b; bool bb;
                    holdsSet(m.before.items[0], b, bb);
                    covered = covered + b;
                    coveredBoundary |= bb;
                }
                if (!any || (covered == all && coveredBoundary)) continue;
                //  The cases to evaluate: the items after the text and the missing
                //  characters before it, split where a rule's context tells their
                //  characters apart (at most a few times).
                struct Case {
                    std::vector<std::pair<UCD::UnicodeSet, bool>> items;
                    UCD::UnicodeSet missing;
                    bool missingBoundary;
                };
                std::vector<Case> cases{{region.items, all - covered, !coveredBoundary}};
                size_t evaluated = 0;
                while (!cases.empty() && why.empty()) {
                if (++evaluated > 32) {why = "too many cases"; break;}
                const Case current = cases.back();
                cases.pop_back();
                const UCD::UnicodeSet & missing = current.missing;
                const bool missingBoundary = current.missingBoundary;
                //  (A split: of the missing characters (-1) or of item k after the
                //  text, by where an item of a rule holds.)
                int splitAt = -2;
                UCD::UnicodeSet splitChars; bool splitBoundary = false;
                auto split = [&](int at, const DivContextItem & item) {
                    splitAt = at;
                    holdsSet(item, splitChars, splitBoundary);
                };
                //  Evaluate the text there.
                std::u32string out;
                bool ok = true;
                size_t p = 0;
                while (p < text.size() && ok) {
                    std::vector<size_t> matching;
                    for (size_t i = 0; i < sources.size() && ok; i++) {
                        const Source & src = sources[i];
                        if (src.text.size() > text.size() - p) {
                            //  (A match beyond the text: only if it cannot match.)
                            bool can = true;
                            for (size_t k = 0; k + p < text.size() && can; k++) can = src.text[k].contains(text[p + k]);
                            if (can && !(src.text[text.size() - p] & current.items[0].first).empty()) {ok = false; why = "a match beyond the text";}
                            continue;
                        }
                        bool textMatches = true;
                        for (size_t k = 0; k < src.text.size() && textMatches; k++) textMatches = src.text[k].contains(text[p + k]);
                        if (!textMatches) continue;
                        //  The after context: within the text, then the region's
                        //  items; beyond them, it must hold whatever follows.
                        int a = 1;
                        //  (An after context computed by the engine: not evaluated here.)
                        if (src.after.present() && src.after.items[0].kind == DivContextItem::Kind::Stream) {
                            ok = false;
                            why = "an after context computed by the regular expression engine";
                            break;
                        }
                        for (size_t k = 0; k < src.after.items.size() && a == 1; k++) {
                            const size_t q = p + src.text.size() + k;
                            if (q < text.size()) {
                                a = test(src.after.items[k], UCD::UnicodeSet(text[q]), false);
                            } else if (q - text.size() < current.items.size()) {
                                const auto & it = current.items[q - text.size()];
                                a = test(src.after.items[k], it.first, it.second);
                                if (a < 0) split(static_cast<int>(q - text.size()), src.after.items[k]);
                            } else if (test(src.after.items[k], all, true) != 1) {
                                ok = false;
                                why = "an after context beyond the items of the region";
                            }
                        }
                        if (!ok || splitAt != -2) break;
                        //  The before context: the last character written, or the
                        //  missing characters (nothing written yet).
                        int b = 1;
                        if (src.before.present()) {
                            if (src.before.items.size() > 1) {ok = false; why = "a before context of several items"; break;}
                            if (out.empty()) {
                                b = test(src.before.items[0], missing, missingBoundary);
                                if (b < 0) {split(-1, src.before.items[0]); break;}
                            } else {
                                b = test(src.before.items[0], UCD::UnicodeSet(out.back()), false);
                            }
                        }
                        if (a == 1 && b == 1) matching.push_back(i);
                    }
                    if (!ok || splitAt != -2) break;
                    //  Less those yielding to another matching there.
                    std::vector<size_t> applies;
                    for (size_t i : matching) {
                        bool yielding = false;
                        for (size_t q : matching) {
                            if (q == i) continue;
                            for (const DivEarlier & e : yields[i]) {
                                if ((e.stringRule >= 0 && e.stringRule == sources[q].stringRule)
                                        || (e.subgroup >= 0 && e.subgroup == sources[q].subgroup && e.chars.contains(text[p]))) {
                                    yielding = true;
                                }
                            }
                        }
                        if (!yielding) applies.push_back(i);
                    }
                    if (applies.size() > 1) {ok = false; why = "more than one rule applies"; break;}
                    if (applies.empty()) {
                        out.push_back(text[p]);
                        p++;
                    } else {
                        out += sources[applies.front()].result;
                        p += sources[applies.front()].text.size();
                    }
                }
                if (!ok) break;
                if (splitAt != -2) {
                    //  The two cases: where the rule's item holds, and where not.
                    for (bool in : {true, false}) {
                        Case c = current;
                        auto & target = splitAt < 0 ? c.missing : c.items[splitAt].first;
                        bool & targetBoundary = splitAt < 0 ? c.missingBoundary : c.items[splitAt].second;
                        target = in ? (target & splitChars) : (target - splitChars);
                        targetBoundary = targetBoundary && (in == splitBoundary);
                        if (!target.empty() || targetBoundary) cases.push_back(std::move(c));
                    }
                    continue;
                }
                //  The synthetic rule: the missing characters before, the items
                //  of the case after.
                Source src;
                src.text = front.text;
                src.result = out;
                src.converted = true;
                src.synthetic = static_cast<int>(members.front());
                src.before.items.emplace_back();
                classifyItem(missing, missingBoundary, true, src.before.items.back());
                if (!(current.items.size() == 1 && current.items[0].first == all && !current.items[0].second)) {
                    for (const auto & it : current.items) {
                        src.after.items.emplace_back();
                        classifyItem(it.first, it.second, false, src.after.items.back());
                    }
                }
                added.push_back(std::move(src));
                }
                if (!why.empty()) break;
            }
            if (!why.empty()) {
                family.reason += "; not completed: " + why;
                continue;
            }
            //  (Yielding as the rules of the family do to the rules outside it.)
            std::vector<DivEarlier> outsideYields;
            for (const DivEarlier & e : yields[members.front()]) {
                if (e.stringRule >= 0 && std::find(family.stringRules.begin(), family.stringRules.end(), e.stringRule) != family.stringRules.end()) continue;
                outsideYields.push_back(e);
            }
            for (Source & src : added) {
                familyMembers[f].push_back(sources.size());
                yields.push_back(outsideYields);
                sources.push_back(std::move(src));
            }
            family.covering = true;
            family.completed = added.size();
        }
    }

    //  The covering families applied: their rules yield to the same rules
    //  outside them, and a rule yielding to one of them yields to all.  The
    //  string rules with converted before contexts outside them are problems.
    for (size_t f = 0; f < plan.convertedFamilies.size(); f++) {
        DivConvertedFamily & family = plan.convertedFamilies[f];
        if (!family.covering) continue;
        if (!DivCodeFamilies) {
            family.covering = false;
            family.reason = "families disabled (--divcode-families=0)";
            continue;
        }
        const std::vector<size_t> & members = familyMembers[f];
        auto inFamily = [&](int stringRule) {
            return std::find(family.stringRules.begin(), family.stringRules.end(), stringRule) != family.stringRules.end();
        };
        auto outside = [&](size_t i) {
            std::set<std::string> r;
            for (const DivEarlier & e : yields[i]) {
                if (e.stringRule >= 0 && inFamily(e.stringRule)) continue;
                std::string key = std::to_string(e.stringRule) + "," + std::to_string(e.subgroup) + ":";
                for (const auto & range : e.chars) key += std::to_string(range.first) + "-" + std::to_string(range.second) + ",";
                r.insert(key);
            }
            return r;
        };
        bool applied = true;
        for (size_t j : members) {
            if (outside(j) != outside(members.front())) {
                family.reason = "its rules yield to different rules";
                applied = false;
                break;
            }
        }
        for (size_t i = 0; i < sources.size() && applied; i++) {
            if (std::find(members.begin(), members.end(), i) != members.end()) continue;
            size_t n = 0;
            for (const DivEarlier & e : yields[i]) n += e.stringRule >= 0 && inFamily(e.stringRule) ? 1 : 0;
            if (n > 0 && n < members.size()) {
                family.reason = "a rule yields to some of its rules";
                applied = false;
            }
        }
        if (!applied) {
            family.covering = false;
            continue;
        }
        for (size_t j : members) sources[j].family = static_cast<int>(f);
    }
    for (const Source & src : sources) {
        if (src.stringRule >= 0 && src.converted && src.family < 0) problem("string rules with before contexts matched against the converted text");
    }
    //  A string rule whose before context is matched against the converted text
    //  may begin at a later position of the text of a string rule (itself
    //  included) where the overlaps (analyzeStringRuleOverlaps) have no such
    //  pair: they exclude it where its before context cannot match there in the
    //  text, but matched against the converted text it may, the rule containing it
    //  having applied (x { bb → c ; aab → ; on xaabb, where ICU applies no rule
    //  within aab).  Not eligible: the regular plan applies such matches only
    //  outside the applied matches of the others.
    for (const Source & src : sources) {
        if (src.stringRule < 0 || !src.converted || src.text.empty()) continue;
        const std::vector<size_t> & pre = stagePredecessors[static_cast<size_t>(src.stringRule)];
        bool within = false;
        for (const Source & other : sources) {
            if (other.stringRule < 0 || std::find(pre.begin(), pre.end(), static_cast<size_t>(other.stringRule)) != pre.end()) continue;
            for (size_t o = 1; o < other.text.size() && !within; o++) within = other.text[o].intersects(src.text.front());
            if (within) break;
        }
        if (within) {
            problem("a string rule with a before context matched against the converted text may begin within the text of a string rule");
            break;
        }
    }

    //  The alphabet: the blocks of the texts, of the explicit context sets (or
    //  their complements), and of the characters of earlier single-character
    //  rules.
    for (const Source & src : sources) {
        for (const UCD::UnicodeSet & t : src.text) {
            const auto b = blocksOf(t, s);
            blocks.insert(b.begin(), b.end());
        }
    }
    for (const auto & cs : contextSets) {
        const auto b = blocksOf(cs.first, s);
        blocks.insert(b.begin(), b.end());
    }
    for (const auto & ys : yields) {
        for (const DivEarlier & d : ys) {
            const auto b = blocksOf(d.chars, s);
            blocks.insert(b.begin(), b.end());
        }
    }
    plan.blocks.assign(blocks.begin(), blocks.end());
    while ((plan.blocks.size() >> plan.codeBits) != 0) plan.codeBits++;
    if (plan.codeBits > options.maxCodeBits) {
        problem("an alphabet of " + std::to_string(plan.blocks.size()) + " blocks (more than " + std::to_string(options.maxCodeBits)
                + " code bits)");
    }

    //  The rules, split by the blocks of their first sets, by leaf.
    plan.leaves.resize(plan.blocks.size());
    std::vector<DivRule> rules;
    for (size_t i = 0; i < sources.size(); i++) {
        const Source & src = sources[i];
        for (UCD::codepoint_t b : blocksOf(src.text.front(), s)) {
            DivRule d;
            d.rule = src.rule;
            d.stringRule = src.stringRule;
            d.subgroup = src.subgroup;
            d.anchor = plan.codeOf(b << s);
            d.text = src.text;
            d.text.front() = src.text.front() & blockRange(b, s);
            d.result = src.result;
            d.before = src.before;
            d.after = src.after;
            d.yieldsTo = yields[i];
            //  (A synthetic rule: the stage of the rules of its family.)
            const int stageRule = src.synthetic >= 0 ? sources[src.synthetic].stringRule : src.stringRule;
            d.stage = stageRule >= 0 ? stageOf[stageRule] : 0;
            //  (Phase 1: single-character rules; a string rule's flag is for its
            //  family, DivConvertedFamily.)
            d.converted = src.converted && src.stringRule < 0 && src.synthetic < 0;
            if (d.converted) plan.convertedRules++;
            d.family = src.family;
            if (d.family >= 0) plan.familyRules++;
            rules.push_back(std::move(d));
        }
    }
    for (size_t code = 1; code <= plan.blocks.size(); code++) {
        for (const DivRule & d : rules) {
            if (d.anchor != code) continue;
            plan.leaves[code - 1].push_back(plan.rules.size());
            plan.rules.push_back(d);
        }
    }

    //  The hash of the whole plan (the alphabet is named by the DivCode kernel).
    std::string blockList;
    for (UCD::codepoint_t b : plan.blocks) blockList += (blockList.empty() ? "x" : "_x") + hex(b);
    std::string content = "s" + std::to_string(s) + ";" + blockList + ";";
    for (const UCD::UnicodeSet & e : plan.externalSets) appendSet(content, e);
    for (const std::string & k : plan.afterStreamKeys) content += "{" + k + "}";
    auto appendOne = [&](const DivContextItem & item) {
        content += "|" + std::to_string(static_cast<int>(item.kind)) + (item.negated ? "!" : "") + (item.guardEnd ? "g" : "")
                   + (item.runEnd ? "r" : "") + (item.outside ? "o" : "");
        if (item.kind == DivContextItem::Kind::External) content += "e" + std::to_string(item.external);
        else if (item.kind == DivContextItem::Kind::Stream) content += "s" + std::to_string(item.external);
        else if (item.present()) appendSet(content, item.set);
        if (item.hasThrough()) {
            content += "*" + std::to_string(static_cast<int>(item.throughKind)) + "e" + std::to_string(item.throughExternal);
            appendSet(content, item.through);
        }
    };
    auto appendItem = [&](const DivContext & context) {
        content += "(";
        for (const DivContextItem & item : context.items) appendOne(item);
        content += ")";
    };
    for (const auto & sc : plan.subgroupContexts) {
        content += ";g";
        appendItem(sc.first);
        appendItem(sc.second);
    }
    for (const DivRule & d : plan.rules) {
        content += ";r" + std::to_string(d.stringRule) + "," + std::to_string(d.subgroup) + "," + std::to_string(d.anchor) + "@"
                   + std::to_string(d.stage) + (d.converted ? "c" : "") + (d.family >= 0 ? "f" + std::to_string(d.family) : "") + ":";
        for (const UCD::UnicodeSet & t : d.text) appendSet(content, t);
        content += ">";
        for (char32_t c : d.result) content += hex(c) + ",";
        appendItem(d.before);
        appendItem(d.after);
        for (const DivEarlier & e : d.yieldsTo) {
            content += "<" + std::to_string(e.stringRule) + "," + std::to_string(e.subgroup);
            appendSet(content, e.chars);
        }
    }
    plan.contentHash = hex(fnv1a(content), 16);
    classifyConverted(plan);
    //  A group without string rules is applied by DivCode only for its rules
    //  with before contexts matched against the converted text, which the
    //  Converted kernel resolves instead of rounds (doc/ldml-divcode-plan.md
    //  §12); otherwise its present plan applies it.
    if (step.stringRules.empty() && !plan.dependent()) problem("no string rules");

    for (const auto & p : problems) {
        plan.problems.push_back(p.first + (p.second > 1 ? " (" + std::to_string(p.second) + ")" : ""));
    }
    return plan;
}

}

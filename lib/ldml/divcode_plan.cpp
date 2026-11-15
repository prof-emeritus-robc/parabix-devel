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
#include "charset_analysis.h"

using namespace re;
using namespace llvm;

namespace ldml {

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
bool contextItems(RE * re, CharSetAnalysis & sets, std::vector<std::pair<UCD::UnicodeSet, bool>> & items, std::string & why) {
    if (re == nullptr) return true;
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) if (!contextItems(e, sets, items, why)) return false;
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
    if (step.stringRules.empty()) problem("no string rules");
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

    //  An item of a context: its set and whether it holds at the text boundary.
    auto classifyItem = [&](const UCD::UnicodeSet & set, bool boundary, bool before, DivContextItem & item) {
        item.runEnd = marksFilterRunEnd(set);
        //  With the text boundary: tested as the negation of the complement.
        item.negated = boundary;
        item.set = boundary ? ~set : set;
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
    auto classify = [&](RE * re, RE * engineBefore, bool before, DivContext & context) {
        std::vector<std::pair<UCD::UnicodeSet, bool>> items;
        std::string why;
        if (!contextItems(re, sets, items, why)) {
            problem(why);
            return;
        }
        if (items.empty()) return;
        if (before) {
            if (!options.convertedBefore) {
                problem("before contexts (no check of the converted text)");
            } else if (options.convertedBefore(step, engineBefore)) {
                problem("before contexts matched against the converted text");
            }
            //  (From the item next to the text outward.)
            std::reverse(items.begin(), items.end());
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
        const ConversionRule * rule = nullptr;
        int stringRule = -1, subgroup = -1;
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
        classify(sr.before, sr.engineBefore, true, src.before);
        classify(sr.after, nullptr, false, src.after);
        sources.push_back(std::move(src));
    }
    for (size_t g = 0; g < step.subgroups.size(); g++) {
        const CharMapSubgroup & sg = step.subgroups[g];
        DivContext before, after;
        classify(sg.before, sg.engineBefore, true, before);
        classify(sg.after, nullptr, false, after);
        plan.subgroupContexts.emplace_back(before, after);
        for (const CharMapping & m : sg.mappings) {
            const UCD::UnicodeSet chars = m.chars - UCD::UnicodeSet(StringRuleDeleted);
            if (chars.empty()) continue;
            if (!literal(m.replacement)) {problem("single-character rules with marks in their results"); continue;}
            Source src;
            src.text = {chars};
            src.result = m.replacement;
            src.before = before;
            src.after = after;
            src.subgroup = static_cast<int>(g);
            sources.push_back(std::move(src));
        }
    }
    plan.sourceRules = sources.size();

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
            for (const ConversionRule * e : *earlier) yields[i].push_back(earlierOf(e));
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
            d.stage = src.stringRule >= 0 ? stageOf[src.stringRule] : 0;
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
    auto appendOne = [&](const DivContextItem & item) {
        content += "|" + std::to_string(static_cast<int>(item.kind)) + (item.negated ? "!" : "") + (item.guardEnd ? "g" : "")
                   + (item.runEnd ? "r" : "");
        if (item.kind == DivContextItem::Kind::External) content += "e" + std::to_string(item.external);
        else if (item.present()) appendSet(content, item.set);
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
                   + std::to_string(d.stage) + ":";
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

    for (const auto & p : problems) {
        plan.problems.push_back(p.first + (p.second > 1 ? " (" + std::to_string(p.second) + ")" : ""));
    }
    return plan;
}

}

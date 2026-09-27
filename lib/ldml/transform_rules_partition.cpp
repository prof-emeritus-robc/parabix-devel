/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules.h>
#include <ldml/transform_rules_printer.h>
#include "charset_analysis.h"
#include <re/adt/adt.h>
#include <algorithm>
#include <map>
#include <set>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

//  Collect the characters of the strings of a set, including the strings of
//  the variables within it.
void collectSetStrings(const RE * re, std::set<codepoint_t> & literals) {
    if (re == nullptr) return;
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (const Seq * seq = dyn_cast<Seq>(a)) {
                for (const RE * e : *seq) {
                    if (const CC * cc = dyn_cast<CC>(e)) {
                        if (cc->size() == 1) literals.insert(lo_codepoint(cc->front()));
                    }
                }
            } else {
                collectSetStrings(a, literals);
            }
        }
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (!isFunctionCall(n)) collectSetStrings(n->getDefinition(), literals);
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        collectSetStrings(d->getLH(), literals);
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        collectSetStrings(x->getLH(), literals);
        collectSetStrings(x->getRH(), literals);
    }
}

//  Collect the characters occurring directly in a pattern or result: literal
//  characters and the characters of strings (including the strings of
//  variables within sets), but not the members of sets.  Other variables are
//  not followed: the definitions of the variables used are collected separately.
void collectLiterals(const RE * re, std::set<codepoint_t> & literals) {
    if (re == nullptr) return;
    if (const CC * cc = dyn_cast<CC>(re)) {
        if (cc->size() == 1 && lo_codepoint(cc->front()) == hi_codepoint(cc->front())) {
            literals.insert(lo_codepoint(cc->front()));
        }
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collectLiterals(e, literals);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        // Strings within sets (single characters within sets are members).
        collectSetStrings(alt, literals);
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        collectLiterals(rep->getRE(), literals);
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        collectLiterals(c->getCapturedRE(), literals);
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) collectLiterals(n->getDefinition(), literals);
    }
}

void collectLiterals(const RuleSide * side, std::set<codepoint_t> & literals) {
    collectLiterals(side->getBeforeContext(), literals);
    collectLiterals(side->getCompletedResult(), literals);
    collectLiterals(side->getResultToRevisit(), literals);
    collectLiterals(side->getAfterContext(), literals);
}

//  Collect the sets written inline in a pattern: the maximal set expressions
//  that are not variable references (whose definitions are rules themselves)
//  or single literal characters.
void collectInlineSets(const RE * re, std::vector<const RE *> & sets) {
    if (re == nullptr) return;
    if (const CC * cc = dyn_cast<CC>(re)) {
        if (cc->size() == 1 && lo_codepoint(cc->front()) == hi_codepoint(cc->front())) return;
    }
    if (const Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) collectInlineSets(n->getDefinition(), sets);
    } else if (CharSetAnalysis::isSet(re)) {
        sets.push_back(re);
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collectInlineSets(e, sets);
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        collectInlineSets(rep->getRE(), sets);
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        collectInlineSets(c->getCapturedRE(), sets);
    }
}

void collectInlineSets(const RuleSide * side, std::vector<const RE *> & sets) {
    collectInlineSets(side->getBeforeContext(), sets);
    collectInlineSets(side->getCompletedResult(), sets);
    collectInlineSets(side->getResultToRevisit(), sets);
    collectInlineSets(side->getAfterContext(), sets);
}

//  Collect the variables referenced in a pattern (not following definitions).
void collectVariables(const RE * re, std::set<const Name *> & names) {
    if (re == nullptr) return;
    if (const Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) {
            collectVariables(n->getDefinition(), names);
        } else {
            names.insert(n);
        }
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collectVariables(e, names);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) collectVariables(e, names);
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        collectVariables(rep->getRE(), names);
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        collectVariables(c->getCapturedRE(), names);
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        collectVariables(d->getLH(), names);
        collectVariables(d->getRH(), names);
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        collectVariables(x->getLH(), names);
        collectVariables(x->getRH(), names);
    }
}

void collectVariables(const RuleSide * side, std::set<const Name *> & names) {
    collectVariables(side->getBeforeContext(), names);
    collectVariables(side->getCompletedResult(), names);
    collectVariables(side->getResultToRevisit(), names);
    collectVariables(side->getAfterContext(), names);
}

bool isSingleton(const UCD::UnicodeSet & s) {
    return s.size() == 1 && lo_codepoint(s.front()) == hi_codepoint(s.front());
}

struct SetLess {
    bool operator()(const UCD::UnicodeSet & a, const UCD::UnicodeSet & b) const {
        return a.compare(b) < 0;
    }
};

// Partition the rules [begin, end) of a group.
CharacterClassPartition partitionGroup(const std::vector<Rule *> & rules, const size_t begin, const size_t end,
                                       const std::map<const Name *, size_t> & definitionIndex,
                                       std::set<std::string> & usedNames, std::map<std::string, unsigned> & generatedCount) {
    CharSetAnalysis analysis;
    std::set<codepoint_t> literals;
    struct VariableInfo {
        std::string name;
        UCD::UnicodeSet chars;
    };
    std::vector<VariableInfo> setVariables;           // variables defining sets, in order
    std::map<codepoint_t, std::string> charVariables; // single character variables
    std::vector<const RE *> inlineSets;               // sets written inline, in order
    // The variables of the group: those used by its conversion rules,
    // following the definitions of variables that are not sets.
    std::set<const Name *> used;
    for (size_t i = begin; i < end; i++) {
        if (const ConversionRule * c = dyn_cast<ConversionRule>(rules[i])) {
            collectVariables(c->getLeftSide(), used);
            collectVariables(c->getRightSide(), used);
        }
    }
    std::vector<const Name *> pending(used.begin(), used.end());
    while (!pending.empty()) {
        const Name * n = pending.back();
        pending.pop_back();
        if (n->getDefinition() == nullptr || CharSetAnalysis::isSet(n->getDefinition())) continue;
        std::set<const Name *> refs;
        collectVariables(n->getDefinition(), refs);
        for (const Name * r : refs) {
            if (used.insert(r).second) pending.push_back(r);
        }
    }
    std::vector<const Name *> variables(used.begin(), used.end());
    auto position = [&](const Name * n) {
        auto f = definitionIndex.find(n);
        return f == definitionIndex.end() ? rules.size() : f->second;
    };
    std::sort(variables.begin(), variables.end(), [&](const Name * a, const Name * b) {return position(a) < position(b);});
    for (const Name * n : variables) {
        RE * def = n->getDefinition();
        if (def == nullptr) continue;
        if (CharSetAnalysis::isSet(def)) {
            UCD::UnicodeSet chars = analysis.setOf(def, false);
            if (!chars.empty()) {
                if (isSingleton(chars)) {
                    charVariables.emplace(lo_codepoint(chars.front()), n->getName());
                }
                setVariables.push_back(VariableInfo{n->getName(), std::move(chars)});
            }
            collectSetStrings(def, literals);
        } else {
            collectInlineSets(def, inlineSets);
            collectLiterals(def, literals);
        }
    }
    for (size_t i = begin; i < end; i++) {
        if (const ConversionRule * c = dyn_cast<ConversionRule>(rules[i])) {
            collectLiterals(c->getLeftSide(), literals);
            collectLiterals(c->getRightSide(), literals);
            collectInlineSets(c->getLeftSide(), inlineSets);
            collectInlineSets(c->getRightSide(), inlineSets);
        }
    }
    // The distinct inline sets.
    CharacterClassPartition partition;
    partition.firstRule = begin;
    partition.lastRule = end - 1;
    std::map<UCD::UnicodeSet, size_t, SetLess> inlineIndex;
    std::vector<UCD::UnicodeSet> inlineChars;
    for (const RE * re : inlineSets) {
        UCD::UnicodeSet chars = analysis.setOf(re, false);
        if (chars.empty()) continue;
        auto f = inlineIndex.find(chars);
        if (f != inlineIndex.end()) {
            partition.inlineSets[f->second].occurrences++;
            continue;
        }
        inlineIndex.emplace(chars, partition.inlineSets.size());
        partition.inlineSets.push_back(CharacterClassPartition::InlineSet{printUnicodeSet(re), const_cast<RE *>(re), makeCC(chars), {}, 1});
        inlineChars.push_back(std::move(chars));
    }
    // The sets to be refined: the variable sets, the inline sets, and the
    // singleton sets of literal characters and single character variables.
    std::vector<const UCD::UnicodeSet *> sets;
    for (const VariableInfo & v : setVariables) sets.push_back(&v.chars);
    for (const UCD::UnicodeSet & s : inlineChars) sets.push_back(&s);
    std::set<codepoint_t> singletonChars(literals);
    for (const auto & cv : charVariables) singletonChars.insert(cv.first);
    std::vector<UCD::UnicodeSet> singletons;
    for (const codepoint_t c : singletonChars) singletons.emplace_back(c);
    for (const UCD::UnicodeSet & s : singletons) sets.push_back(&s);
    // Each class is a maximal set of characters contained in exactly the same
    // sets: sweep the interval boundaries of all sets, grouping the segments
    // between boundaries by the sets containing them.
    struct Event {
        codepoint_t point;
        bool enter;
        unsigned set;
    };
    std::vector<Event> events;
    for (unsigned k = 0; k < sets.size(); k++) {
        for (const auto & i : *sets[k]) {
            events.push_back(Event{lo_codepoint(i), true, k});
            if (hi_codepoint(i) < UCD::UNICODE_MAX) events.push_back(Event{hi_codepoint(i) + 1, false, k});
        }
    }
    std::sort(events.begin(), events.end(), [](const Event & a, const Event & b) {return a.point < b.point;});
    std::map<std::vector<unsigned>, size_t> classOfSignature;
    std::vector<UCD::UnicodeSet> classes;
    std::vector<std::vector<unsigned>> signatures;
    std::set<unsigned> active;
    for (size_t e = 0; e < events.size(); ) {
        const codepoint_t point = events[e].point;
        for (; e < events.size() && events[e].point == point; e++) {
            if (events[e].enter) active.insert(events[e].set);
            else active.erase(events[e].set);
        }
        if (active.empty()) continue;
        const codepoint_t next = (e < events.size()) ? events[e].point : UCD::UNICODE_MAX + 1;
        std::vector<unsigned> signature(active.begin(), active.end());
        auto f = classOfSignature.find(signature);
        size_t cls;
        if (f == classOfSignature.end()) {
            cls = classes.size();
            classOfSignature.emplace(signature, cls);
            classes.emplace_back();
            signatures.push_back(std::move(signature));
        } else {
            cls = f->second;
        }
        classes[cls].insert_range(point, next - 1);
    }
    // Order the classes by their first codepoints.
    std::vector<size_t> order(classes.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return lo_codepoint(classes[a].front()) < lo_codepoint(classes[b].front());
    });
    // The classes of each variable and inline set.
    const unsigned variableCount = setVariables.size();
    const unsigned inlineCount = inlineChars.size();
    std::vector<std::vector<size_t>> classesOfSet(variableCount + inlineCount);
    for (size_t i = 0; i < order.size(); i++) {
        for (const unsigned k : signatures[order[i]]) {
            if (k < variableCount + inlineCount) classesOfSet[k].push_back(i);
        }
    }
    // Name the classes.
    std::set<std::string> classNames;
    for (size_t i = 0; i < order.size(); i++) {
        const UCD::UnicodeSet & c = classes[order[i]];
        const std::vector<unsigned> & signature = signatures[order[i]];
        CharacterClass cls{"", false, false, false, "", nullptr, makeCC(c)};
        if (isSingleton(c)) {
            const codepoint_t cp = lo_codepoint(c.front());
            auto f = charVariables.find(cp);
            if (f != charVariables.end()) {
                cls.name = f->second;
                cls.existing = true;
            } else if (literals.count(cp)) {
                cls.literal = true;
            }
        }
        if (!cls.literal && cls.name.empty()) {
            // A variable whose set is exactly this class.
            for (const unsigned k : signature) {
                if (k < variableCount && classesOfSet[k].size() == 1 && classNames.count(setVariables[k].name) == 0) {
                    cls.name = setVariables[k].name;
                    cls.existing = true;
                    break;
                }
            }
        }
        if (!cls.literal && cls.name.empty()) {
            // An inline set that is exactly this class remains as written.
            for (const unsigned k : signature) {
                if (k >= variableCount && k < variableCount + inlineCount && classesOfSet[k].size() == 1) {
                    cls.inlineSet = true;
                    cls.text = partition.inlineSets[k - variableCount].text;
                    cls.inlineRE = partition.inlineSets[k - variableCount].re;
                    break;
                }
            }
        }
        if (!cls.literal && !cls.inlineSet && cls.name.empty()) {
            // A new name after the first variable containing the class.
            const std::string base = (signature.front() < variableCount) ? setVariables[signature.front()].name : "set";
            std::string name;
            do {
                name = base + "_" + std::to_string(++generatedCount[base]);
            } while (usedNames.count(name) != 0);
            cls.name = name;
            usedNames.insert(name);
        }
        if (!cls.name.empty()) classNames.insert(cls.name);
        partition.classes.push_back(cls);
    }
    for (unsigned k = 0; k < variableCount; k++) {
        partition.variables.push_back(CharacterClassPartition::VariableSet{setVariables[k].name, makeCC(setVariables[k].chars), classesOfSet[k]});
    }
    for (unsigned k = 0; k < inlineCount; k++) {
        partition.inlineSets[k].classes = classesOfSet[variableCount + k];
    }
    return partition;
}

} // end anonymous namespace

std::vector<CharacterClassPartition> partitionCharacterClasses(const std::vector<Rule *> & rules) {
    std::set<std::string> usedNames;
    std::map<const Name *, size_t> definitionIndex;
    for (size_t i = 0; i < rules.size(); i++) {
        if (const VariableDefinitionRule * v = dyn_cast<VariableDefinitionRule>(rules[i])) {
            usedNames.insert(v->getName());
            definitionIndex.emplace(v->getVariable(), i);
        }
    }
    std::map<std::string, unsigned> generatedCount;
    std::vector<CharacterClassPartition> partitions;
    for (size_t i = 0; i < rules.size(); ) {
        if (!isa<ConversionRule>(rules[i]) && !isa<VariableDefinitionRule>(rules[i])) {
            i++;
            continue;
        }
        size_t end = i;
        while (end < rules.size() && (isa<ConversionRule>(rules[end]) || isa<VariableDefinitionRule>(rules[end]))) end++;
        partitions.push_back(partitionGroup(rules, i, end, definitionIndex, usedNames, generatedCount));
        i = end;
    }
    return partitions;
}

}

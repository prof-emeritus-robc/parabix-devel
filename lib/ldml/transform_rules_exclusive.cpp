/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Mutually exclusive partitioning: rewriting the conversion rules of each
//  group in terms of the mutually exclusive character classes of the group.

#include <ldml/transform_rules.h>
#include "charset_analysis.h"
#include <re/adt/adt.h>
#include <map>
#include <set>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

// Does a set include the text boundary [$] (possibly through variables)?
bool hasTextBoundary(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && hasTextBoundary(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        if (includesTextBoundary(re)) return true;
        for (const RE * a : *alt) {
            if (hasTextBoundary(a)) return true;
        }
    }
    return false;
}

// The strings of a set (possibly through variables).
void collectStrings(RE * re, std::vector<RE *> & strings) {
    if (Alt * alt = dyn_cast<Alt>(re)) {
        for (RE * a : *alt) {
            if (isa<Seq>(a)) {
                strings.push_back(a);
            } else {
                collectStrings(a, strings);
            }
        }
    } else if (Name * n = dyn_cast<Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition()) collectStrings(n->getDefinition(), strings);
    } else if (Diff * d = dyn_cast<Diff>(re)) {
        collectStrings(d->getLH(), strings);
    } else if (Intersect * x = dyn_cast<Intersect>(re)) {
        collectStrings(x->getLH(), strings);
    }
}

// The characters of a set without its strings and text boundary.
RE * withoutStringsAndBoundary(RE * re) {
    if (Alt * alt = dyn_cast<Alt>(re)) {
        std::vector<RE *> members;
        for (RE * a : *alt) {
            if (!isa<Seq>(a) && !isa<Start>(a) && !isa<End>(a)) members.push_back(a);
        }
        if (members.size() == alt->size()) return re;
        if (members.size() == 1) return members[0];
        return Alt::Create(members.begin(), members.end());
    }
    return re;
}

//  The rewriting of one group.
class GroupRewriter {
public:
    GroupRewriter(const CharacterClassPartition & partition, const std::map<std::string, Name *> & variables,
                  std::set<std::string> & usedNames, unsigned group, MutuallyExclusiveStats & stats)
    : mPartition(partition), mUsedNames(usedNames), mGroup(group), mStats(stats) {
        for (const CharacterClass & c : partition.classes) {
            RE * denotation = nullptr;
            if (c.literal) {
                denotation = makeCC(lo_codepoint(c.chars->front()));
            } else if (c.inlineSet) {
                denotation = withoutStringsAndBoundary(c.inlineRE);
            } else if (c.existing) {
                denotation = variables.at(c.name);
            } else {
                Name * n = makeName(c.name, c.chars);
                mNewDefinitions.push_back(VariableDefinitionRule::Create(n));
                mStats.classDefinitions++;
                denotation = n;
            }
            mDenotations.push_back(denotation);
        }
    }

    // New definitions for the group: class variables, then variable copies.
    std::vector<Rule *> newDefinitions() const {
        std::vector<Rule *> defs(mNewDefinitions);
        defs.insert(defs.end(), mCopyDefinitions.begin(), mCopyDefinitions.end());
        return defs;
    }

    ConversionRule * rewrite(const ConversionRule * r) {
        mCaptures.clear();
        RuleSide * left = rewrite(r->getLeftSide());
        RuleSide * right = rewrite(r->getRightSide());
        // Point the references to the rewritten captures.
        left = remapReferences(left);
        right = remapReferences(right);
        return makeConversionRule(left, r->getDirection(), right);
    }

private:
    RuleSide * rewrite(const RuleSide * s) {
        return RuleSide::Create(rewrite(s->getBeforeContext()), rewrite(s->getCompletedResult()), s->hasCursor(),
                                rewrite(s->getResultToRevisit()), s->getCursorOffset(), rewrite(s->getAfterContext()));
    }

    RuleSide * remapReferences(const RuleSide * s) {
        return RuleSide::Create(remap(s->getBeforeContext()), remap(s->getCompletedResult()), s->hasCursor(),
                                remap(s->getResultToRevisit()), s->getCursorOffset(), remap(s->getAfterContext()));
    }

    // The union of the classes of a set, retaining its strings and text boundary.
    RE * rewriteSet(RE * set) {
        const UCD::UnicodeSet chars = mAnalysis.setOf(set, false);
        std::vector<RE *> elements;
        std::vector<size_t> classes;
        UCD::UnicodeSet covered;
        for (size_t i = 0; i < mPartition.classes.size(); i++) {
            const CC * c = mPartition.classes[i].chars;
            if (c->intersects(chars)) {
                elements.push_back(mDenotations[i]);
                classes.push_back(i);
                covered = covered + *c;
            }
        }
        if (!(covered == chars)) mStats.mismatches++;
        // A set that is exactly one class denoted by the set itself (an inline
        // set that is not divided, or the variable) remains as written.
        if (classes.size() == 1) {
            const CharacterClass & c = mPartition.classes[classes[0]];
            if (c.inlineSet || elements[0] == set) {
                return set;
            }
        }
        mStats.setsRewritten++;
        std::vector<RE *> strings;
        collectStrings(set, strings);
        const bool boundary = hasTextBoundary(set);
        if (elements.size() == 1 && strings.empty() && !boundary) {
            return elements[0];
        }
        // The classes remain distinct members of the union, ordered as in
        // parsed sets: sets and variables, strings, then characters.
        std::vector<RE *> members;
        for (RE * e : elements) {
            if (!isa<CC>(e)) members.push_back(e);
        }
        members.insert(members.end(), strings.begin(), strings.end());
        for (RE * e : elements) {
            if (isa<CC>(e)) members.push_back(e);
        }
        if (boundary) {
            members.push_back(makeStart());
            members.push_back(makeEnd());
        }
        return Alt::Create(members.begin(), members.end());
    }

    // A copy of a variable that is not a set, with the sets of its definition
    // rewritten, or the variable itself if its definition is unchanged.
    Name * rewriteVariable(Name * n) {
        auto f = mCopies.find(n);
        if (f != mCopies.end()) return f->second;
        RE * def = n->getDefinition();
        RE * rewritten = rewrite(def);
        Name * result = n;
        if (rewritten != def) {
            std::string name;
            unsigned k = 0;
            do {
                name = n->getName() + "_g" + std::to_string(mGroup) + (k ? "_" + std::to_string(k) : "");
                k++;
            } while (mUsedNames.count(name) != 0);
            mUsedNames.insert(name);
            result = makeName(name, rewritten);
            mCopyDefinitions.push_back(VariableDefinitionRule::Create(result));
            mStats.variableCopies++;
        }
        mCopies.emplace(n, result);
        return result;
    }

    RE * rewrite(RE * re) {
        if (re == nullptr) return nullptr;
        if (const CC * cc = dyn_cast<CC>(re)) {
            if (cc->size() == 1 && lo_codepoint(cc->front()) == hi_codepoint(cc->front())) return re;
        }
        if (Name * n = dyn_cast<Name>(re)) {
            if (isFunctionCall(n)) {
                RE * arg = rewrite(n->getDefinition());
                return (arg != n->getDefinition()) ? makeFunctionCall(getFunctionID(n), arg) : re;
            }
            if (n->getDefinition() == nullptr) return re;
            if (CharSetAnalysis::isSet(n->getDefinition())) {
                if (mAnalysis.setOf(n, false).empty()) return re;   // only strings
                return rewriteSet(n);
            }
            return rewriteVariable(n);
        }
        if (CharSetAnalysis::isSet(re)) {
            if (mAnalysis.setOf(re, false).empty()) return re;      // only strings or [$]
            return rewriteSet(re);
        }
        if (Seq * seq = dyn_cast<Seq>(re)) {
            std::vector<RE *> items;
            bool changed = false;
            for (RE * e : *seq) {
                items.push_back(rewrite(e));
                changed |= items.back() != e;
            }
            return changed ? makeSeq(items.begin(), items.end()) : re;
        }
        if (Rep * rep = dyn_cast<Rep>(re)) {
            RE * r = rewrite(rep->getRE());
            return (r != rep->getRE()) ? makeRep(r, rep->getLB(), rep->getUB()) : re;
        }
        if (Capture * c = dyn_cast<Capture>(re)) {
            Capture * rewritten = makeCapture(c->getName(), rewrite(c->getCapturedRE()));
            mCaptures.emplace(c, rewritten);
            return rewritten;
        }
        return re;
    }

    RE * remap(RE * re) {
        if (re == nullptr) return nullptr;
        if (Reference * ref = dyn_cast<Reference>(re)) {
            auto f = mCaptures.find(ref->getCapture());
            if (f == mCaptures.end()) return re;
            return makeReference(ref->getName(), f->second, ref->getInstance());
        }
        if (Name * n = dyn_cast<Name>(re)) {
            if (isFunctionCall(n)) {
                RE * arg = remap(n->getDefinition());
                return (arg != n->getDefinition()) ? makeFunctionCall(getFunctionID(n), arg) : re;
            }
            return re;
        }
        if (Seq * seq = dyn_cast<Seq>(re)) {
            std::vector<RE *> items;
            bool changed = false;
            for (RE * e : *seq) {
                items.push_back(remap(e));
                changed |= items.back() != e;
            }
            return changed ? makeSeq(items.begin(), items.end()) : re;
        }
        if (Rep * rep = dyn_cast<Rep>(re)) {
            RE * r = remap(rep->getRE());
            return (r != rep->getRE()) ? makeRep(r, rep->getLB(), rep->getUB()) : re;
        }
        if (Capture * c = dyn_cast<Capture>(re)) {
            // Captures are rebuilt in the first pass; references within them
            // (which do not occur in rules) are not remapped.
            return c;
        }
        return re;
    }

    const CharacterClassPartition & mPartition;
    std::set<std::string> & mUsedNames;
    const unsigned mGroup;
    MutuallyExclusiveStats & mStats;
    CharSetAnalysis mAnalysis;
    std::vector<RE *> mDenotations;             // the denotation of each class
    std::vector<Rule *> mNewDefinitions;        // class variables
    std::vector<Rule *> mCopyDefinitions;       // copies of variables that are not sets
    std::map<Name *, Name *> mCopies;
    std::map<const Capture *, Capture *> mCaptures;
};

} // end anonymous namespace

std::vector<Rule *> MutuallyExclusivePartitioning(const std::vector<Rule *> & rules, MutuallyExclusiveStats * stats) {
    MutuallyExclusiveStats localStats;
    MutuallyExclusiveStats & s = stats ? *stats : localStats;
    const std::vector<CharacterClassPartition> partitions = partitionCharacterClasses(rules);
    std::map<std::string, Name *> variables;
    std::set<std::string> usedNames;
    for (const Rule * r : rules) {
        if (const VariableDefinitionRule * v = dyn_cast<VariableDefinitionRule>(r)) {
            variables.emplace(v->getName(), v->getVariable());
            usedNames.insert(v->getName());
        }
    }
    for (const CharacterClassPartition & p : partitions) {
        for (const CharacterClass & c : p.classes) {
            if (!c.name.empty()) usedNames.insert(c.name);
        }
    }
    std::vector<Rule *> result;
    size_t next = 0;
    for (size_t g = 0; g < partitions.size(); g++) {
        const CharacterClassPartition & p = partitions[g];
        s.groups++;
        // The rules before the group.
        result.insert(result.end(), rules.begin() + next, rules.begin() + p.firstRule);
        GroupRewriter rewriter(p, variables, usedNames, static_cast<unsigned>(g + 1), s);
        std::vector<Rule *> conversions;
        for (size_t i = p.firstRule; i <= p.lastRule; i++) {
            if (isa<VariableDefinitionRule>(rules[i])) {
                result.push_back(rules[i]);
            } else {
                conversions.push_back(rewriter.rewrite(cast<ConversionRule>(rules[i])));
            }
        }
        const std::vector<Rule *> defs = rewriter.newDefinitions();
        result.insert(result.end(), defs.begin(), defs.end());
        result.insert(result.end(), conversions.begin(), conversions.end());
        next = p.lastRule + 1;
    }
    result.insert(result.end(), rules.begin() + next, rules.end());
    return result;
}

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  The independent components of a conversion group (see
 *  include/ldml/transform_rules.h, IndependentComponents).
 */

#include <ldml/transform_rules.h>
#include <ldml/transform_plan.h>
#include <llvm/Support/Casting.h>
#include <re/adt/adt.h>
#include <numeric>

using namespace llvm;
using namespace re;
using UCD::UnicodeSet;

namespace ldml {

namespace {

//  Whether a pattern has a function call, or a part whose characters its sets
//  do not give (a copied capture).
bool hasCallOrReference(const RE * re) {
    if (re == nullptr) return false;
    if (isFunctionCall(re) || isa<Reference>(re)) return true;
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * x : *seq) if (hasCallOrReference(x)) return true;
        return false;
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * x : *alt) if (hasCallOrReference(x)) return true;
        return false;
    }
    if (const Rep * rep = dyn_cast<Rep>(re)) return hasCallOrReference(rep->getRE());
    if (const Capture * c = dyn_cast<Capture>(re)) return hasCallOrReference(c->getCapturedRE());
    if (const Name * n = dyn_cast<Name>(re)) return hasCallOrReference(n->getDefinition());
    return false;
}

struct Footprint {
    UnicodeSet text, context, result;
    bool lengthChanging = false;
    bool hasContext = false;
};

Footprint footprint(const ConversionRule * rule) {
    Footprint f;
    const RuleSide * const source = rule->getSourceSide(Direction::Forward);
    const RuleSide * const result = rule->getResultSide(Direction::Forward);
    f.text = patternCharacters(source->getText());
    for (RE * c : {source->getBeforeContext(), source->getAfterContext()}) {
        if (c == nullptr) continue;
        f.hasContext = true;
        f.context = f.context + patternCharacters(c);
    }
    RE * const out = result ? result->getText() : nullptr;
    if (out && hasCallOrReference(out)) {
        f.result = UnicodeSet(0, UCD::UNICODE_MAX);
        f.lengthChanging = true;
    } else if (out) {
        f.result = patternCharacters(out);
    }
    //  The lengths: of the text, its items, none repeated; of the result, its
    //  characters.
    const std::vector<PatternItem> items = patternItems(source->getText());
    bool fixed = true;
    for (const PatternItem & item : items) fixed &= !item.repeated;
    const std::vector<PatternItem> outItems = out ? patternItems(out) : std::vector<PatternItem>{};
    if (!fixed || outItems.size() != items.size()) f.lengthChanging = true;
    for (const PatternItem & item : outItems) f.lengthChanging |= item.repeated;
    return f;
}

bool affects(const Footprint & a, const Footprint & b) {
    if (!(a.context & (b.text + b.result)).empty()) return true;
    if (!(a.text & b.result).empty()) return true;
    if (b.lengthChanging && a.hasContext) return true;
    return false;
}

}

std::vector<std::vector<size_t>> IndependentComponents(const std::vector<Rule *> & group) {
    std::vector<size_t> rules;
    std::vector<Footprint> f;
    for (size_t i = 0; i < group.size(); i++) {
        const ConversionRule * const rule = dyn_cast<ConversionRule>(group[i]);
        if (rule == nullptr || !appliesInDirection(rule, Direction::Forward)) continue;
        rules.push_back(i);
        f.push_back(footprint(rule));
    }
    std::vector<size_t> parent(rules.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    //  (Pairwise; the groups have at most a few thousand rules.)
    for (size_t i = 0; i < rules.size(); i++) {
        for (size_t j = i + 1; j < rules.size(); j++) {
            if (find(i) == find(j)) continue;
            if (!(f[i].text & f[j].text).empty() || affects(f[i], f[j]) || affects(f[j], f[i])) parent[find(i)] = find(j);
        }
    }
    std::vector<std::vector<size_t>> components;
    std::vector<size_t> index(rules.size(), SIZE_MAX);
    for (size_t i = 0; i < rules.size(); i++) {
        const size_t r = find(i);
        if (index[r] == SIZE_MAX) {
            index[r] = components.size();
            components.emplace_back();
        }
        components[index[r]].push_back(rules[i]);
    }
    return components;
}

}

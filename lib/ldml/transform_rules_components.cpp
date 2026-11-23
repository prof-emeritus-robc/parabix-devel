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
    const size_t n = rules.size();
    std::vector<size_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto unite = [&](size_t x, size_t y) {
        x = find(x);
        y = find(y);
        if (x != y) parent[x] = y;
    };
    //  The relations of the rules hold where their sets share a character.  The
    //  characters are divided into the elementary intervals of the bounds of all
    //  the sets, and each relation is resolved within each interval: rules a and
    //  b (a != b) are related there if a has the interval in one of its sets X
    //  and b in another Y (text and text; context and text or result; text and
    //  result).  All the rules having the interval in X or Y are then connected
    //  (the complete bipartite relation, without the pairs of a rule with
    //  itself, connects them unless they are a single rule).  (The pairwise
    //  comparison of the sets was quadratic: about 0.7 s for Hans-Hant.)
    std::vector<UCD::codepoint_t> bounds;
    auto addBounds = [&](const UnicodeSet & set) {
        for (const auto & range : set) {
            bounds.push_back(range.first);
            if (range.second < UCD::UNICODE_MAX) bounds.push_back(range.second + 1);
        }
    };
    for (const Footprint & x : f) {
        addBounds(x.text);
        addBounds(x.context);
        addBounds(x.result);
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    enum : unsigned char {Text = 1, Context = 2, Result = 4};
    //  For each interval, the rules having it in their sets, with the sets.
    std::vector<std::vector<std::pair<size_t, unsigned char>>> at(bounds.size());
    auto mark = [&](size_t r, const UnicodeSet & set, unsigned char kind) {
        for (const auto & range : set) {
            const size_t lo = std::lower_bound(bounds.begin(), bounds.end(), range.first) - bounds.begin();
            const size_t hi = std::upper_bound(bounds.begin(), bounds.end(), range.second) - bounds.begin();
            for (size_t k = lo; k < hi; k++) {
                auto & v = at[k];
                if (!v.empty() && v.back().first == r) v.back().second |= kind;
                else v.emplace_back(r, kind);
            }
        }
    };
    for (size_t r = 0; r < n; r++) {
        mark(r, f[r].text, Text);
        mark(r, f[r].context, Context);
        mark(r, f[r].result, Result);
    }
    //  Connect the rules of v having a kind of X or of Y, if some a has X and
    //  some b != a has Y.
    auto connect = [&](const std::vector<std::pair<size_t, unsigned char>> & v, unsigned char x, unsigned char y) {
        size_t first = SIZE_MAX, count = 0;
        bool hasX = false, hasY = false;
        for (const auto & e : v) {
            if (!(e.second & (x | y))) continue;
            hasX |= (e.second & x) != 0;
            hasY |= (e.second & y) != 0;
            if (count++ == 0) first = e.first;
        }
        if (count < 2 || !hasX || !hasY) return;
        for (const auto & e : v) {
            if (e.second & (x | y)) unite(e.first, first);
        }
    };
    for (const auto & v : at) {
        if (v.size() < 2) continue;
        connect(v, Text, Text);
        connect(v, Context, Text | Result);
        connect(v, Text, Result);
    }
    //  A rule with a context and another that changes the length.
    {
        size_t first = SIZE_MAX, count = 0;
        bool context = false, changing = false;
        for (size_t r = 0; r < n; r++) {
            if (!(f[r].hasContext || f[r].lengthChanging)) continue;
            context |= f[r].hasContext;
            changing |= f[r].lengthChanging;
            if (count++ == 0) first = r;
        }
        if (count >= 2 && context && changing) {
            for (size_t r = 0; r < n; r++) {
                if (f[r].hasContext || f[r].lengthChanging) unite(r, first);
            }
        }
    }
    std::vector<std::vector<size_t>> components;
    std::vector<size_t> index(n, SIZE_MAX);
    for (size_t i = 0; i < n; i++) {
        const size_t r = find(i);
        if (index[r] == SIZE_MAX) {
            index[r] = components.size();
            components.emplace_back();
        }
        components[index[r]].push_back(rules[i]);
    }
    return components;
}

std::vector<Rule *> SplitIndependentComponents(const std::vector<Rule *> & rules, size_t minRules, ComponentSplitStats * stats) {
    std::vector<Rule *> out;
    std::vector<Rule *> group;
    auto flush = [&]() {
        size_t conversions = 0;
        for (Rule * r : group) conversions += isa<ConversionRule>(r);
        std::vector<bool> first(group.size(), false);
        bool split = false;
        if (conversions > minRules) {
            const std::vector<std::vector<size_t>> components = IndependentComponents(group);
            size_t inFirst = 0;
            for (const std::vector<size_t> & c : components) {
                bool before = false;
                for (size_t i : c) {
                    before |= cast<ConversionRule>(group[i])->getSourceSide(Direction::Forward)->getBeforeContext() != nullptr;
                }
                if (!before) continue;
                for (size_t i : c) first[i] = true;
                inFirst += c.size();
            }
            //  (The rules without a forward direction stay in the first part.)
            for (size_t i = 0; i < group.size(); i++) {
                const ConversionRule * const c = dyn_cast<ConversionRule>(group[i]);
                if (c && !appliesInDirection(c, Direction::Forward)) first[i] = true;
            }
            split = inFirst > 0 && inFirst < conversions;
            if (split && stats) {
                stats->groups++;
                stats->rulesSetApart += conversions - inFirst;
            }
        }
        if (!split) {
            out.insert(out.end(), group.begin(), group.end());
        } else {
            for (size_t i = 0; i < group.size(); i++) {
                if (first[i] || isa<VariableDefinitionRule>(group[i])) out.push_back(group[i]);
            }
            out.push_back(makeTransformRule(TransformID("Null")));
            for (size_t i = 0; i < group.size(); i++) {
                if (!first[i] && isa<ConversionRule>(group[i])) out.push_back(group[i]);
                else if (isa<VariableDefinitionRule>(group[i])) out.push_back(group[i]);
            }
        }
        group.clear();
    };
    for (Rule * r : rules) {
        if (isa<TransformRule>(r) || isa<FilterRule>(r)) {
            flush();
            out.push_back(r);
        } else {
            group.push_back(r);
        }
    }
    flush();
    return out;
}

}

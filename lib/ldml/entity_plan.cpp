/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Entity plans of conversion groups (see include/ldml/entity_plan.h and
//  doc/ldml-entity-groups-plan.md).

#include <ldml/entity_plan.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <ldml/transform_rules.h>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include "charset_analysis.h"

using namespace llvm;
using namespace re;

namespace ldml {

static constexpr size_t NoEntity = std::numeric_limits<size_t>::max();

const char * kindName(Entity::Kind k) {
    switch (k) {
        case Entity::Kind::Property: return "property";
        case Entity::Kind::PropertySet: return "property_set";
        case Entity::Kind::PureSet: return "pure_set";
        case Entity::Kind::StringSet: return "string_set";
        case Entity::Kind::Union: return "union";
    }
    return "?";
}

UCD::UnicodeSet Entity::firstChars() const {
    UCD::UnicodeSet s = chars;
    for (const std::u32string & str : strings) s.insert(str.front());
    return s;
}

UCD::UnicodeSet Entity::allChars() const {
    UCD::UnicodeSet s = chars;
    for (const std::u32string & str : strings) {
        for (char32_t c : str) s.insert(c);
    }
    return s;
}

namespace {

//  The strings (of two or more characters) of a set, through alternations,
//  differences, intersections and variables.
bool fixedString(const RE * re, std::u32string & out) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        if (cc->count() != 1) return false;
        out.push_back(static_cast<char32_t>(cc->min_codepoint()));
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) {
            if (!fixedString(e, out)) return false;
        }
        return true;
    } else if (const Name * name = dyn_cast<Name>(re)) {
        if (isFunctionCall(name) || name->getDefinition() == nullptr) return false;
        return fixedString(name->getDefinition(), out);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        return alt->size() == 1 && fixedString(alt->front(), out);
    }
    return false;
}

std::set<std::u32string> setStrings(const RE * re) {
    std::set<std::u32string> strings;
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            const std::set<std::u32string> s = setStrings(a);
            strings.insert(s.begin(), s.end());
        }
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        std::u32string str;
        if (seq->size() > 1 && fixedString(seq, str)) strings.insert(str);
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        strings = setStrings(d->getLH());
        for (const std::u32string & str : setStrings(d->getRH())) strings.erase(str);
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        const std::set<std::u32string> r = setStrings(x->getRH());
        for (const std::u32string & str : setStrings(x->getLH())) if (r.count(str)) strings.insert(str);
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition()) strings = setStrings(n->getDefinition());
    }
    return strings;
}

void collectProperties(const RE * re, std::vector<const PropertyExpression *> & props) {
    if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        props.push_back(pe);
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (!isFunctionCall(n) && n->getDefinition()) collectProperties(n->getDefinition(), props);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) collectProperties(a, props);
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        collectProperties(d->getLH(), props);
        collectProperties(d->getRH(), props);
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        collectProperties(x->getLH(), props);
        collectProperties(x->getRH(), props);
    }
}

//  A variable denoting a set (through a resolved copy), or nullptr.
const Name * setVariable(const RE * re) {
    const Name * n = dyn_cast<Name>(re);
    if (n == nullptr || isFunctionCall(n) || n->getDefinition() == nullptr) return nullptr;
    if (!CharSetAnalysis::isSet(n->getDefinition())) return nullptr;
    return n;
}

//  An alternation of one set, [$v] for $v, or the RE itself.
const RE * unwrapped(const RE * re) {
    while (const Alt * alt = dyn_cast<Alt>(re)) {
        if (alt->size() != 1) break;
        re = alt->front();
    }
    return re;
}

std::string rangesKey(const UCD::UnicodeSet & s) {
    std::string k;
    for (const auto & range : s) {
        k += std::to_string(range.first) + "-" + std::to_string(range.second) + ",";
    }
    return k;
}

std::string encodeUTF8(const std::u32string & s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) {out += static_cast<char>(c); continue;}
        const unsigned n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        out += static_cast<char>((0xF00u >> n) | (c >> (6 * (n - 1))));
        for (unsigned i = n - 1; i-- > 0; ) out += static_cast<char>(0x80 | ((c >> (6 * i)) & 0x3F));
    }
    return out;
}

class GroupPlanner {
public:
    GroupPlanner(EntityGroupPlan & plan, const EntityOptions & options) : P(plan), mOptions(options) {}
    void addRule(const ConversionRule * c);
    void finish();
private:
    enum class Use {Text, Before, After};
    size_t entityOf(const RE * re, std::string & problem);
    size_t contentEntity(UCD::UnicodeSet chars, std::set<std::u32string> strings, bool boundary,
                         const RE * re, std::string text, std::string name);
    size_t addEntity(Entity && e);
    void markUse(size_t e, Use use);
    void flatten(RE * re, bool optional, std::vector<EntityItem> & out, EntityRule & r, Use use);
    UCD::UnicodeSet itemFirst(const EntityItem & item) const;
    UCD::UnicodeSet itemAll(const EntityItem & item) const;
    unsigned itemMin(const EntityItem & item) const;
    unsigned itemMax(const EntityItem & item) const;
    void classes();
    void overlaps();
    void beforeStates();
    bool sameItems(const std::vector<EntityItem> & a, const std::vector<EntityItem> & b) const;

    EntityGroupPlan & P;
    const EntityOptions & mOptions;
    CharSetAnalysis mSets;
    std::map<std::string, size_t> mByKey;
    unsigned mInline = 0;
};

size_t GroupPlanner::addEntity(Entity && e) {
    std::sort(e.lengths.begin(), e.lengths.end());
    e.lengths.erase(std::unique(e.lengths.begin(), e.lengths.end()), e.lengths.end());
    std::string key = std::string(kindName(e.kind)) + "|" + rangesKey(e.chars) + "|";
    for (const std::u32string & s : e.strings) key += encodeUTF8(s) + ",";
    key += e.boundary ? "|$|" : "||";
    for (size_t o : e.operands) key += std::to_string(o) + ",";
    if (e.kind == Entity::Kind::Property) key += "|" + e.text;
    auto f = mByKey.find(key);
    if (f != mByKey.end()) {
        Entity & existing = P.entities[f->second];
        if (e.variable && !existing.variable) {
            existing.name = e.name;
            existing.variable = true;
        }
        return f->second;
    }
    if (e.name.empty()) e.name = "inline_" + std::to_string(++mInline);
    P.entities.push_back(std::move(e));
    mByKey.emplace(key, P.entities.size() - 1);
    return P.entities.size() - 1;
}

size_t GroupPlanner::contentEntity(UCD::UnicodeSet chars, std::set<std::u32string> strings, bool boundary,
                                   const RE * re, std::string text, std::string name) {
    Entity e;
    e.kind = strings.empty() ? Entity::Kind::PureSet : Entity::Kind::StringSet;
    e.chars = std::move(chars);
    e.strings.assign(strings.begin(), strings.end());
    e.boundary = boundary;
    e.re = re;
    e.text = std::move(text);
    e.variable = !name.empty();
    e.name = std::move(name);
    if (!e.chars.empty()) e.lengths.push_back(1);
    for (const std::u32string & s : e.strings) e.lengths.push_back(static_cast<unsigned>(s.size()));
    if (e.lengths.empty()) e.lengths.push_back(1);
    return addEntity(std::move(e));
}

size_t GroupPlanner::entityOf(const RE * re, std::string & problem) {
    re = unwrapped(re);
    const Name * var = setVariable(re);
    const RE * body = var ? unwrapped(var->getDefinition()) : re;
    const std::string name = var ? "$" + originalVariable(var)->getName() : std::string();
    const std::string text = var ? name : printUnicodeSet(re);
    const bool boundary = mayIncludeTextBoundary(re);
    std::vector<const PropertyExpression *> props;
    collectProperties(re, props);
    const std::set<std::u32string> strings = setStrings(re);
    if (!props.empty()) {
        if (!strings.empty()) {
            problem = "a set with strings and properties: " + text;
            return NoEntity;
        }
        if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(body)) {
            Entity e;
            e.kind = Entity::Kind::Property;
            e.re = pe;
            e.text = printUnicodeSet(pe);
            e.name = var ? name : e.text;
            e.variable = var != nullptr;
            e.chars = mSets.setOf(pe, false);
            e.boundary = boundary;
            e.lengths.push_back(1);
            return addEntity(std::move(e));
        }
        Entity e;
        e.kind = Entity::Kind::PropertySet;
        e.re = re;
        e.text = printUnicodeSet(var ? var->getDefinition() : re);
        e.name = name;
        e.variable = var != nullptr;
        e.chars = mSets.setOf(re, false);
        e.boundary = boundary;
        e.lengths.push_back(1);
        for (const PropertyExpression * pe : props) {
            const size_t o = entityOf(pe, problem);
            if (o == NoEntity) return NoEntity;
            if (std::find(e.operands.begin(), e.operands.end(), o) == e.operands.end()) e.operands.push_back(o);
        }
        return addEntity(std::move(e));
    }
    if (strings.empty()) {
        return contentEntity(mSets.setOf(re, false), {}, boundary, re, text, name);
    }
    //  A union with a set with strings among its variables: derived.
    if (const Alt * alt = dyn_cast<Alt>(body)) {
        std::vector<size_t> operands;
        bool stringOperand = false;
        UCD::UnicodeSet restChars;
        std::set<std::u32string> restStrings;
        bool restBoundary = false;
        bool rest = false;
        bool other = false;
        for (const RE * a : *alt) {
            if (setVariable(a)) {
                const size_t o = entityOf(a, problem);
                if (o == NoEntity) return NoEntity;
                const Entity::Kind k = P.entities[o].kind;
                stringOperand |= k == Entity::Kind::StringSet || k == Entity::Kind::Union;
                if (std::find(operands.begin(), operands.end(), o) == operands.end()) operands.push_back(o);
            } else if (isBoundary(a)) {
                restBoundary = true;
            } else if (isa<CC>(a)) {
                restChars = restChars + *cast<CC>(a);
                rest = true;
            } else if (isa<Seq>(a)) {
                std::u32string s;
                if (!fixedString(a, s)) {other = true; break;}
                if (s.size() == 1) restChars.insert(s[0]); else restStrings.insert(s);
                rest = true;
            } else {
                other = true;
                break;
            }
        }
        if (!other && stringOperand && operands.size() + (rest ? 1 : 0) >= 2) {
            if (rest) {
                std::string restText = "(the rest of " + text + ")";
                const size_t o = contentEntity(restChars, restStrings, false, nullptr, restText, "");
                operands.push_back(o);
            }
            Entity e;
            e.kind = Entity::Kind::Union;
            e.re = re;
            e.text = var ? printUnicodeSet(var->getDefinition()) : text;
            e.name = name;
            e.variable = var != nullptr;
            e.boundary = boundary || restBoundary;
            e.operands = operands;
            std::set<std::u32string> all;
            for (size_t o : operands) {
                const Entity & op = P.entities[o];
                e.chars = e.chars + op.chars;
                all.insert(op.strings.begin(), op.strings.end());
                e.lengths.insert(e.lengths.end(), op.lengths.begin(), op.lengths.end());
            }
            e.strings.assign(all.begin(), all.end());
            return addEntity(std::move(e));
        }
    }
    return contentEntity(mSets.setOf(re, false), strings, boundary, re,
                         var ? printUnicodeSet(var->getDefinition()) : text, name);
}

void GroupPlanner::markUse(size_t e, Use use) {
    Entity & entity = P.entities[e];
    switch (use) {
        case Use::Text: entity.inText = true; break;
        case Use::Before: entity.inBefore = true; break;
        case Use::After: entity.inAfter = true; break;
    }
    const std::vector<size_t> operands = entity.operands;
    for (size_t o : operands) markUse(o, use);
}

void GroupPlanner::flatten(RE * re, bool optional, std::vector<EntityItem> & out, EntityRule & r, Use use) {
    if (re == nullptr) return;
    auto entityItem = [&](const RE * set) {
        std::string problem;
        const size_t e = entityOf(set, problem);
        if (e == NoEntity) {
            r.problems.push_back(problem);
            return;
        }
        markUse(e, use);
        EntityItem item;
        item.kind = EntityItem::Kind::Entity;
        item.entity = e;
        item.optional = optional;
        out.push_back(item);
    };
    if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) flatten(e, optional, out, r, use);
    } else if (isBoundary(re) || isa<Start>(re) || isa<End>(re)) {
        EntityItem item;
        item.kind = EntityItem::Kind::Boundary;
        item.optional = optional;
        out.push_back(item);
    } else if (CC * cc = dyn_cast<CC>(re)) {
        if (cc->empty()) {
            r.problems.push_back("an empty set");
        } else if (cc->count() == 1) {
            EntityItem item;
            item.kind = EntityItem::Kind::Literal;
            item.literal = static_cast<char32_t>(cc->min_codepoint());
            item.optional = optional;
            out.push_back(item);
        } else {
            entityItem(cc);
        }
    } else if (Name * name = dyn_cast<Name>(re)) {
        if (isFunctionCall(name)) {
            r.problems.push_back("a function call");
        } else if (name->getDefinition() == nullptr) {
            r.problems.push_back("an undefined variable $" + name->getName());
        } else if (CharSetAnalysis::isSet(name->getDefinition())) {
            entityItem(name);
        } else {
            flatten(name->getDefinition(), optional, out, r, use);
        }
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        if (rep->getLB() == 0 && rep->getUB() == 1) {
            std::vector<EntityItem> body;
            flatten(rep->getRE(), true, body, r, use);
            if (body.size() != 1) {
                r.problems.push_back("an optional sequence: " + printPattern(rep));
            } else {
                out.push_back(body.front());
            }
        } else {
            r.problems.push_back("a repetition: " + printPattern(rep));
        }
    } else if (isa<Capture>(re)) {
        r.problems.push_back("a segment (capture)");
    } else if (isa<Reference>(re)) {
        r.problems.push_back("a reference to a segment");
    } else if (CharSetAnalysis::isSet(re)) {
        entityItem(re);
    } else if (Alt * alt = dyn_cast<Alt>(re); alt && alt->size() == 1) {
        flatten(alt->front(), optional, out, r, use);
    } else {
        r.problems.push_back("an unsupported item: " + printPattern(re));
    }
}

UCD::UnicodeSet GroupPlanner::itemFirst(const EntityItem & item) const {
    switch (item.kind) {
        case EntityItem::Kind::Literal: return UCD::UnicodeSet(item.literal);
        case EntityItem::Kind::Entity: return P.entities[item.entity].firstChars();
        case EntityItem::Kind::Boundary: return UCD::UnicodeSet();
    }
    return UCD::UnicodeSet();
}

UCD::UnicodeSet GroupPlanner::itemAll(const EntityItem & item) const {
    switch (item.kind) {
        case EntityItem::Kind::Literal: return UCD::UnicodeSet(item.literal);
        case EntityItem::Kind::Entity: return P.entities[item.entity].allChars();
        case EntityItem::Kind::Boundary: return UCD::UnicodeSet();
    }
    return UCD::UnicodeSet();
}

unsigned GroupPlanner::itemMin(const EntityItem & item) const {
    if (item.optional || item.kind == EntityItem::Kind::Boundary) return 0;
    if (item.kind == EntityItem::Kind::Literal) return 1;
    return P.entities[item.entity].lengths.front();
}

unsigned GroupPlanner::itemMax(const EntityItem & item) const {
    if (item.kind == EntityItem::Kind::Boundary) return 0;
    if (item.kind == EntityItem::Kind::Literal) return 1;
    return P.entities[item.entity].lengths.back();
}

void GroupPlanner::addRule(const ConversionRule * c) {
    EntityRule r;
    r.rule = c;
    r.text = printRule(c);
    const RuleSide * source = c->getSourceSide(Direction::Forward);
    const RuleSide * result = c->getResultSide(Direction::Forward);
    flatten(source->getBeforeContext(), false, r.before, r, Use::Before);
    flatten(source->getText(), false, r.items, r, Use::Text);
    flatten(source->getAfterContext(), false, r.after, r, Use::After);
    for (const EntityItem & item : r.items) {
        if (item.kind == EntityItem::Kind::Boundary) r.problems.push_back("the text boundary within a text");
        r.minLength += itemMin(item);
        r.maxLength += itemMax(item);
    }
    if (r.items.empty()) {
        r.problems.push_back("an empty text (an insertion rule)");
    } else if (r.minLength == 0) {
        r.problems.push_back("a text that may be empty");
    }
    if (r.maxLength > mOptions.maxTextLength) {
        r.problems.push_back("a text longer than " + std::to_string(mOptions.maxTextLength) + " characters");
    }
    for (const EntityItem & item : r.before) {
        if (item.kind == EntityItem::Kind::Entity) {
            const Entity::Kind k = P.entities[item.entity].kind;
            if (k == Entity::Kind::StringSet || k == Entity::Kind::Union) {
                r.problems.push_back("a set with strings in a before context");
            }
        }
    }
    if (r.before.size() > 1) r.problems.push_back("a before context of several items");
    if (result->hasCursor()) {
        std::u32string revisit;
        const RE * toRevisit = result->getResultToRevisit();
        if (result->getCursorOffset() != 0 || (toRevisit && !(fixedString(toRevisit, revisit) && revisit.empty()))) {
            r.problems.push_back("a cursor (a result to revisit)");
        }
    }
    if (result->getText() && !fixedString(result->getText(), r.result)) {
        r.problems.push_back("a result that is not a fixed string");
    }
    P.rules.push_back(std::move(r));
}

bool GroupPlanner::sameItems(const std::vector<EntityItem> & a, const std::vector<EntityItem> & b) const {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].kind != b[i].kind || a[i].optional != b[i].optional) return false;
        if (a[i].kind == EntityItem::Kind::Literal && a[i].literal != b[i].literal) return false;
        if (a[i].kind == EntityItem::Kind::Entity && a[i].entity != b[i].entity) return false;
    }
    return true;
}

void GroupPlanner::classes() {
    std::set<std::string> streamKeys;
    std::vector<UCD::UnicodeSet> mux;
    std::set<std::string> muxKeys;
    auto addMux = [&](const UCD::UnicodeSet & s) {
        if (s.empty()) return;
        if (muxKeys.insert(rangesKey(s)).second) mux.push_back(s);
    };
    auto addStream = [&](const UCD::UnicodeSet & s) {
        if (s.empty()) return;
        streamKeys.insert(rangesKey(s));
        addMux(s);
    };
    auto items = [&](const std::vector<EntityItem> & v) {
        for (const EntityItem & item : v) {
            if (item.kind == EntityItem::Kind::Literal) addStream(UCD::UnicodeSet(item.literal));
        }
    };
    for (const EntityRule & r : P.rules) {
        items(r.before);
        items(r.items);
        items(r.after);
    }
    for (const Entity & e : P.entities) {
        if (!(e.inText || e.inBefore || e.inAfter)) continue;
        if (e.kind == Entity::Kind::PureSet) {
            addStream(e.chars);
        } else if (e.kind == Entity::Kind::StringSet) {
            addMux(e.chars);
            for (const std::u32string & s : e.strings) {
                for (char32_t c : s) addMux(UCD::UnicodeSet(c));
            }
        }
    }
    P.streamClasses = static_cast<unsigned>(streamKeys.size());
    P.muxClasses = static_cast<unsigned>(mux.size());
    //  The exclusive classes: the partition of the union of the classes.
    std::vector<UCD::UnicodeSet> parts;
    for (const UCD::UnicodeSet & s : mux) {
        std::vector<UCD::UnicodeSet> next;
        UCD::UnicodeSet rest = s;
        for (const UCD::UnicodeSet & p : parts) {
            const UCD::UnicodeSet in = p & s;
            const UCD::UnicodeSet out = p - s;
            if (!in.empty()) next.push_back(in);
            if (!out.empty()) next.push_back(out);
            rest = rest - p;
        }
        if (!rest.empty()) next.push_back(rest);
        parts = std::move(next);
    }
    P.exclusiveClasses = static_cast<unsigned>(parts.size());
    unsigned bits = 0;
    while ((1u << bits) < P.exclusiveClasses + 1) bits++;
    P.codeBits = bits;
    P.multiplexed = P.streamClasses > mOptions.muxThreshold;
}

//  The character-level automaton of a rule's text: transitions on sets of
//  characters, from node 0 (the start), with each item's single characters as
//  one transition and its strings as chains; optional items may be passed
//  over.  Possessive matching is not modelled (the test is conservative).
struct TextAutomaton {
    struct Edge {
        size_t from;
        UCD::UnicodeSet chars;
        size_t to;
    };
    std::vector<Edge> edges;
    std::set<size_t> accepting;
    size_t nodes = 1;
};

void GroupPlanner::overlaps() {
    const size_t n = P.rules.size();
    std::vector<TextAutomaton> automata(n);
    std::vector<UCD::UnicodeSet> all(n);
    for (size_t i = 0; i < n; i++) {
        const EntityRule & r = P.rules[i];
        TextAutomaton & A = automata[i];
        std::set<size_t> frontier{0};
        for (const EntityItem & item : r.items) {
            all[i] = all[i] + itemAll(item);
            UCD::UnicodeSet singles;
            std::vector<std::u32string> strings;
            if (item.kind == EntityItem::Kind::Literal) {
                singles.insert(item.literal);
            } else if (item.kind == EntityItem::Kind::Entity) {
                singles = P.entities[item.entity].chars;
                strings = P.entities[item.entity].strings;
            }
            const size_t end = A.nodes++;
            for (size_t f : frontier) {
                if (!singles.empty()) A.edges.push_back({f, singles, end});
                for (const std::u32string & str : strings) {
                    size_t at = f;
                    for (size_t k = 0; k < str.size(); k++) {
                        const size_t to = k + 1 == str.size() ? end : A.nodes++;
                        A.edges.push_back({at, UCD::UnicodeSet(str[k]), to});
                        at = to;
                    }
                }
            }
            if (item.optional) frontier.insert(end); else frontier = {end};
        }
        A.accepting = frontier;
    }
    //  Within a match of R (at an offset of 1 or more): the pairs of
    //  consecutive characters, the characters at its last position, and all
    //  its characters there.  At the start of R': its pairs of first
    //  characters, and the first characters of its matches of one character.
    struct Pair {
        UCD::UnicodeSet first;
        UCD::UnicodeSet second;
    };
    std::vector<std::vector<Pair>> innerPairs(n), firstPairs(n);
    std::vector<UCD::UnicodeSet> innerChars(n), lastChars(n), firstChars(n), singleFirst(n);
    for (size_t i = 0; i < n; i++) {
        const TextAutomaton & A = automata[i];
        for (const TextAutomaton::Edge & e1 : A.edges) {
            if (e1.from == 0) {
                firstChars[i] = firstChars[i] + e1.chars;
                if (A.accepting.count(e1.to)) singleFirst[i] = singleFirst[i] + e1.chars;
                for (const TextAutomaton::Edge & e2 : A.edges) {
                    if (e2.from == e1.to) firstPairs[i].push_back({e1.chars, e2.chars});
                }
                continue;
            }
            innerChars[i] = innerChars[i] + e1.chars;
            if (A.accepting.count(e1.to)) lastChars[i] = lastChars[i] + e1.chars;
            for (const TextAutomaton::Edge & e2 : A.edges) {
                if (e2.from == e1.to) innerPairs[i].push_back({e1.chars, e2.chars});
            }
        }
    }
    auto mayBeginWithin = [&](size_t i, size_t j) {
        //  A match of j of one character, or beginning at the last position of i.
        if (!(singleFirst[j] & innerChars[i]).empty()) return true;
        if (!(firstChars[j] & lastChars[i]).empty()) return true;
        for (const Pair & a : innerPairs[i]) {
            for (const Pair & b : firstPairs[j]) {
                if (!(a.first & b.first).empty() && !(a.second & b.second).empty()) return true;
            }
        }
        return false;
    };
    P.mayBeginWithin.assign(n, {});
    P.mayBeginWithinPruned.assign(n, {});
    for (size_t i = 0; i < n; i++) {
        if (P.rules[i].maxLength < 2) continue;
        for (size_t j = 0; j < n; j++) {
            if (!mayBeginWithin(i, j)) continue;
            P.mayBeginWithin[i].push_back(j);
            const EntityRule & rj = P.rules[j];
            bool pruned = false;
            if (rj.before.size() == 1) {
                const EntityItem & b = rj.before.front();
                if (b.kind == EntityItem::Kind::Boundary) {
                    pruned = true;
                } else {
                    pruned = (itemAll(b) & all[i]).empty();
                }
            }
            if (!pruned) P.mayBeginWithinPruned[i].push_back(j);
        }
    }
    //  Cycles and depth of the pruned graph (an edge i → j: j may begin
    //  within i).
    std::vector<std::vector<bool>> reach(n, std::vector<bool>(n, false));
    for (size_t i = 0; i < n; i++) {
        for (size_t j : P.mayBeginWithinPruned[i]) reach[i][j] = true;
    }
    for (size_t k = 0; k < n; k++) {
        for (size_t i = 0; i < n; i++) {
            if (!reach[i][k]) continue;
            for (size_t j = 0; j < n; j++) {
                if (reach[k][j]) reach[i][j] = true;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (reach[i][i]) P.cycleRules.push_back(i);
    }
    //  The longest path, with each set of mutually reachable rules as one node.
    std::vector<int> depth(n, -1);
    std::function<unsigned (size_t)> depthOf = [&](size_t j) -> unsigned {
        if (depth[j] >= 0) return static_cast<unsigned>(depth[j]);
        depth[j] = 0;
        unsigned d = 0;
        for (size_t i = 0; i < n; i++) {
            if (!reach[i][j] || (reach[j][i])) continue;
            d = std::max(d, depthOf(i) + 1);
        }
        //  (Within a cycle: the rules covering any of them.)
        depth[j] = static_cast<int>(d);
        return d;
    };
    P.depth = 0;
    for (size_t j = 0; j < n; j++) {
        bool covered = false;
        for (size_t i = 0; i < n; i++) covered |= reach[i][j];
        if (covered) P.depth = std::max(P.depth, std::max(1u, depthOf(j)));
    }
}

void GroupPlanner::beforeStates() {
    UCD::UnicodeSet textChars;
    for (const EntityRule & r : P.rules) {
        for (const EntityItem & item : r.items) textChars = textChars + itemAll(item);
    }
    std::map<size_t, size_t> byEntity;
    for (size_t i = 0; i < P.rules.size(); i++) {
        const EntityRule & r = P.rules[i];
        if (r.before.size() != 1) continue;
        const EntityItem & b = r.before.front();
        size_t key = NoEntity;
        if (b.kind == EntityItem::Kind::Entity) {
            key = b.entity;
        } else if (b.kind == EntityItem::Kind::Literal) {
            continue;       // (a character: not a state of the kind of §5.3)
        }
        auto f = byEntity.find(key);
        if (f == byEntity.end()) {
            EntityGroupPlan::BeforeState s;
            s.entity = key;
            P.beforeStates.push_back(s);
            f = byEntity.emplace(key, P.beforeStates.size() - 1).first;
        }
        P.beforeStates[f->second].rules.push_back(i);
    }
    for (EntityGroupPlan::BeforeState & s : P.beforeStates) {
        const UCD::UnicodeSet C = s.entity == NoEntity ? UCD::UnicodeSet() : P.entities[s.entity].chars;
        //  (a)
        s.textDisjoint = (C & textChars).empty();
        if (!s.textDisjoint) s.notes.push_back("characters of texts in the set: " + std::to_string((C & textChars).count()));
        //  (b)
        s.resultsOutside = true;
        for (size_t i = 0; i < P.rules.size(); i++) {
            const EntityRule & r = P.rules[i];
            if (!r.result.empty() && C.contains(r.result.back())) {
                s.resultsOutside = false;
                s.notes.push_back("rule " + std::to_string(i + 1) + " ends its result with a character of the set");
            }
        }
        //  (c) The rules with empty results that may apply under the state (no
        //  before context, or this one) are single positions, or are always
        //  preceded by an earlier rule of the state with a non-empty result
        //  matching the same text (as $Boundary {ʔ? $SchwaVowel} → أ precedes
        //  $SchwaVowel → ;).
        s.singleDeletions = true;
        for (size_t i = 0; i < P.rules.size(); i++) {
            const EntityRule & r = P.rules[i];
            if (!r.result.empty() || r.maxLength <= 1) continue;
            const bool underState = r.before.empty() ||
                (r.before.size() == 1 && r.before.front().kind == EntityItem::Kind::Entity && r.before.front().entity == s.entity);
            if (!underState) continue;
            if (r.items.empty()) continue;
            const UCD::UnicodeSet rFirst = itemFirst(r.items.front());
            bool shadowed = false;
            for (size_t j = 0; j < i && !shadowed; j++) {
                const EntityRule & e = P.rules[j];
                if (e.result.empty() || !e.after.empty()) continue;
                const bool eState = e.before.empty() ||
                    (e.before.size() == 1 && e.before.front().kind == EntityItem::Kind::Entity && e.before.front().entity == s.entity);
                if (!eState) continue;
                size_t k = 0;
                while (k < e.items.size() && e.items[k].optional && (itemFirst(e.items[k]) & rFirst).empty()) k++;
                const std::vector<EntityItem> rest(e.items.begin() + k, e.items.end());
                if (sameItems(rest, r.items)) shadowed = true;
            }
            if (!shadowed) {
                s.singleDeletions = false;
                s.notes.push_back("rule " + std::to_string(i + 1) + " deletes up to " + std::to_string(r.maxLength) +
                                  " characters under the state");
            } else {
                s.notes.push_back("rule " + std::to_string(i + 1) + " (a deletion of up to " + std::to_string(r.maxLength) +
                                  " characters) is preceded under the state by a rule with a result");
            }
        }
    }
}

void GroupPlanner::finish() {
    for (size_t i = 0; i < P.rules.size(); i++) {
        for (const std::string & p : P.rules[i].problems) {
            P.problems.push_back("rule " + std::to_string(i + 1) + ": " + p);
        }
    }
    classes();
    overlaps();
    beforeStates();
    for (const EntityGroupPlan::BeforeState & s : P.beforeStates) {
        if (!(s.textDisjoint && s.resultsOutside && s.singleDeletions)) {
            const std::string name = s.entity == NoEntity ? "[$]" : P.entities[s.entity].name;
            P.contextLoop = true;
            P.contextLoopReasons.push_back("before context " + name);
        }
    }
}

} // namespace

std::string entityUTF8(const std::u32string & s) {return encodeUTF8(s);}

std::vector<EntityGroupPlan> planEntityGroups(const std::vector<Rule *> & forwardRules, const EntityOptions & options) {
    std::vector<EntityGroupPlan> plans;
    std::vector<const ConversionRule *> group;
    size_t number = 0;
    auto flush = [&] {
        if (group.empty()) return;
        plans.emplace_back();
        EntityGroupPlan & plan = plans.back();
        plan.number = ++number;
        GroupPlanner planner(plan, options);
        for (const ConversionRule * c : group) planner.addRule(c);
        planner.finish();
        group.clear();
    };
    for (const Rule * r : forwardRules) {
        if (const ConversionRule * c = dyn_cast<ConversionRule>(r)) {
            group.push_back(c);
        } else if (isa<TransformRule>(r) || isa<FilterRule>(r)) {
            flush();
        }
    }
    flush();
    return plans;
}

}

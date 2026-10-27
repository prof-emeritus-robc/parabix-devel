/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  The expansion of function calls on captures of single characters (see
//  ExpandFunctionCalls in transform_rules.h).

#include <ldml/transform_rules.h>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include <re/transforms/re_transformer.h>
#include "charset_analysis.h"

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

//  A set of single characters (no strings, possibly through variables).
bool singleCharacters(const RE * re) {
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        return seq->size() == 1 && singleCharacters(seq->front());
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) if (!singleCharacters(a)) return false;
        return true;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && singleCharacters(n->getDefinition());
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        return singleCharacters(d->getLH()) && singleCharacters(d->getRH());
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        return singleCharacters(x->getLH()) && singleCharacters(x->getRH());
    }
    return CharSetAnalysis::isSetExpression(re);
}

//  The first function call (of a result) whose argument is a reference to a
//  capture, or nullptr.
const Name * findCall(const RE * re) {
    if (re == nullptr) return nullptr;
    if (const Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) {
            const RE * arg = n->getDefinition();
            if (const Seq * seq = dyn_cast_or_null<Seq>(arg); seq && seq->size() == 1) arg = seq->front();
            return isa_and_nonnull<Reference>(arg) ? n : nullptr;
        }
        return nullptr;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (const Name * n = findCall(e)) return n;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (const Name * n = findCall(e)) return n;
    }
    return nullptr;
}

//  The reference that is the argument of a call.
Reference * argumentOf(const Name * call) {
    RE * arg = call->getDefinition();
    if (Seq * seq = dyn_cast<Seq>(arg)) arg = seq->front();
    return cast<Reference>(arg);
}

//  The capture of the given name (within a text), or nullptr.
const Capture * findCapture(const RE * re, const std::string & name) {
    if (const Capture * c = dyn_cast<Capture>(re)) {
        if (c->getName() == name) return c;
        return findCapture(c->getCapturedRE(), name);
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (const Capture * c = findCapture(e, name)) return c;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (const Capture * c = findCapture(e, name)) return c;
    }
    //  (Not within a repetition, whose captures have several values.)
    return nullptr;
}

//  The text with the pattern of a capture restricted to a set.
//  (Or replaced by the set, if keep is false: the capture is no longer referred to.)
class RestrictCapture final : public RE_Transformer {
public:
    RestrictCapture(const std::string & name, RE * set, bool keep)
    : RE_Transformer("RestrictCapture"), mName(name), mSet(set), mKeep(keep) {}
    RE * transformCapture(Capture * c) override {
        if (c->getName() == mName) return mKeep ? makeCapture(mName, mSet) : mSet;
        return RE_Transformer::transformCapture(c);
    }
private:
    const std::string mName;
    RE * const mSet;
    const bool mKeep;
};

//  Does a result refer to a capture (by a reference or a function call)?
bool refersTo(const RE * re, const std::string & name) {
    if (re == nullptr) return false;
    if (const Reference * r = dyn_cast<Reference>(re)) return r->getName() == name;
    if (const Name * n = dyn_cast<Name>(re)) return isFunctionCall(n) && refersTo(n->getDefinition(), name);
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) if (refersTo(e, name)) return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) if (refersTo(e, name)) return true;
    }
    return false;
}

//  The result with a function call replaced.
class ReplaceCall final : public RE_Transformer {
public:
    ReplaceCall(const Name * call, RE * replacement) : RE_Transformer("ReplaceCall"), mCall(call), mReplacement(replacement) {}
    RE * transformName(Name * n) override {
        if (n == mCall) return mReplacement;
        return n;
    }
private:
    const Name * const mCall;
    RE * const mReplacement;
};

RE * literal(const std::u32string & s) {
    std::vector<RE *> items;
    for (char32_t ch : s) items.push_back(makeCC(static_cast<UCD::codepoint_t>(ch), &cc::Unicode));
    return makeSeq(items.begin(), items.end());
}

class FunctionCallExpansion {
public:
    FunctionCallExpansion(const FunctionEvaluator & evaluate, FunctionCallStats * stats) : mEvaluate(evaluate), mStats(stats) {}
    //  The rules replacing a rule (the rule itself if it has no call to expand).
    void expand(Rule * r, std::vector<Rule *> & out, bool counted = false) {
        ConversionRule * const c = dyn_cast<ConversionRule>(r);
        if (c == nullptr || c->getDirection() != Direction::Forward) {
            out.push_back(r);
            return;
        }
        const RuleSide * const source = c->getSourceSide(Direction::Forward);
        const RuleSide * const result = c->getResultSide(Direction::Forward);
        const Name * call = findCall(result->getCompletedResult());
        if (call == nullptr) call = findCall(result->getResultToRevisit());
        if (call == nullptr) {
            out.push_back(r);
            return;
        }
        if (mStats && !counted) mStats->candidates++;
        auto fail = [&](const std::string & reason) {
            if (mStats) mStats->notExpanded.emplace_back(r, reason);
            out.push_back(r);
        };
        Reference * const argument = argumentOf(call);
        const std::string name = argument->getName();
        const Capture * const capture = findCapture(source->getText(), name);
        if (capture == nullptr) return fail("the argument is not a capture of the text");
        if (!singleCharacters(capture->getCapturedRE())) return fail("the argument is not a capture of single characters");
        const UCD::UnicodeSet chars = mSets.setOf(capture->getCapturedRE(), false);
        std::vector<std::pair<UCD::UnicodeSet, std::u32string>> groups;
        UCD::UnicodeSet unchanged;
        std::string reason;
        if (!mEvaluate(TransformID(call->getName()), chars, groups, unchanged, reason)) return fail(reason);
        //  A rule for each group of characters with the same value, and for the
        //  characters left unchanged (referring to the capture).
        auto rule = [&](const UCD::UnicodeSet & set, RE * replacement) {
            ReplaceCall replace(call, replacement);
            RE * const completed = result->getCompletedResult() ? replace.transformRE(result->getCompletedResult()) : nullptr;
            RE * const revisit = result->getResultToRevisit() ? replace.transformRE(result->getResultToRevisit()) : nullptr;
            const bool referred = refersTo(completed, name) || refersTo(revisit, name);
            RE * const text = RestrictCapture(name, makeCC(set, &cc::Unicode), referred).transformRE(source->getText());
            RuleSide * const newSource = makeRuleSide(source->getBeforeContext(), text, source->getAfterContext());
            RuleSide * const newResult = RuleSide::Create(result->getBeforeContext(), completed, result->hasCursor(), revisit,
                                                          result->getCursorOffset(), result->getAfterContext());
            //  (Further calls are expanded in turn.)
            expand(makeConversionRule(newSource, Direction::Forward, newResult), out, true);
        };
        if (mStats && !counted) mStats->expanded++;
        const size_t before = out.size();
        for (const auto & g : groups) {
            if (!g.first.empty()) rule(g.first, literal(g.second));
        }
        if (!unchanged.empty()) rule(unchanged, argument);
        if (mStats && !counted) mStats->rules += out.size() - before;
    }
private:
    const FunctionEvaluator & mEvaluate;
    FunctionCallStats * const mStats;
    CharSetAnalysis mSets;
};

}

std::vector<Rule *> ExpandFunctionCalls(const std::vector<Rule *> & rules, const FunctionEvaluator & evaluate, FunctionCallStats * stats) {
    FunctionCallExpansion expansion(evaluate, stats);
    std::vector<Rule *> out;
    for (Rule * r : rules) expansion.expand(r, out);
    return out;
}

}

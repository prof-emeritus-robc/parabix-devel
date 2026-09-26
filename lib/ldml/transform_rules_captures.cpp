/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules.h>
#include <re/adt/adt.h>
#include <map>
#include <set>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

bool isNullableCapture(const Capture * c) {
    if (const Rep * rep = dyn_cast<Rep>(c->getCapturedRE())) {
        return rep->getLB() == 0;
    }
    return false;
}

void findNullableCaptures(RE * re, bool withinRep, bool includeRepeated, std::vector<Capture *> & found) {
    if (re == nullptr) return;
    if (Capture * c = dyn_cast<Capture>(re)) {
        if (isNullableCapture(c) && (includeRepeated || !withinRep)) found.push_back(c);
        findNullableCaptures(c->getCapturedRE(), withinRep, includeRepeated, found);
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) findNullableCaptures(e, withinRep, includeRepeated, found);
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        findNullableCaptures(rep->getRE(), true, includeRepeated, found);
    }
}

// A single character: a single codepoint, or a variable defined as one.
bool isSingleCharacter(const RE * re) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        return cc->size() == 1 && lo_codepoint(cc->front()) == hi_codepoint(cc->front());
    }
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && isSingleCharacter(n->getDefinition());
    }
    return false;
}

void collectCaptures(RE * re, std::set<const Capture *> & captures) {
    if (Capture * c = dyn_cast<Capture>(re)) {
        captures.insert(c);
        collectCaptures(c->getCapturedRE(), captures);
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) collectCaptures(e, captures);
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        collectCaptures(rep->getRE(), captures);
    }
}

//  Rewrites a rule, either deleting a target capture or giving its
//  repetition a lower bound of 1.  In the latter case, if the capture is
//  then of a single character, the capture and its references are replaced
//  by that character.  All captures of the source side are renumbered in
//  order and the references of the result side updated.
class CaptureRewriter {
public:
    enum class Mode {Delete, NonNull};
    CaptureRewriter(Capture * target, Mode mode) : mTarget(target), mMode(mode) {
        if (mode == Mode::Delete) collectCaptures(target, mDeleted);
    }
    ConversionRule * rewrite(const ConversionRule * r) {
        const RuleSide * src = r->getLeftSide();
        const RuleSide * res = r->getRightSide();
        // Captures are numbered in order of their opening parentheses.
        RE * before = nonEmpty(rewriteSource(src->getBeforeContext()));
        RE * text = rewriteSource(src->getText());
        RE * after = nonEmpty(rewriteSource(src->getAfterContext()));
        RuleSide * source = RuleSide::Create(before, text, false, nullptr, 0, after);
        RuleSide * result = RuleSide::Create(nonEmpty(rewriteResult(res->getBeforeContext())),
                                             rewriteResult(res->getCompletedResult()),
                                             res->hasCursor(),
                                             rewriteResult(res->getResultToRevisit()),
                                             res->getCursorOffset(),
                                             nonEmpty(rewriteResult(res->getAfterContext())));
        return makeConversionRule(source, Direction::Forward, result);
    }
private:
    static RE * nonEmpty(RE * context) {
        return (context == nullptr || isEmptySeq(context)) ? nullptr : context;
    }
    RE * rewriteSource(RE * re);
    RE * rewriteResult(RE * re);

    Capture * const mTarget;
    const Mode mMode;
    std::set<const Capture *> mDeleted;
    std::map<const Capture *, Capture *> mRenumbered;
    RE * mReplacement = nullptr;    // the single character replacing the target
    unsigned mCaptureCount = 0;
};

RE * CaptureRewriter::rewriteSource(RE * re) {
    if (re == nullptr) return nullptr;
    if (Capture * c = dyn_cast<Capture>(re)) {
        if (c == mTarget && mMode == Mode::Delete) {
            return makeSeq();
        }
        RE * captured = c->getCapturedRE();
        if (c == mTarget) {
            Rep * rep = cast<Rep>(captured);
            captured = makeRep(rep->getRE(), 1, rep->getUB());
            if (isSingleCharacter(captured)) {
                mReplacement = captured;
                return captured;
            }
        }
        const std::string name = std::to_string(++mCaptureCount);
        Capture * renumbered = makeCapture(name, rewriteSource(captured));
        mRenumbered.emplace(c, renumbered);
        return renumbered;
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> items;
        bool changed = false;
        for (RE * e : *seq) {
            items.push_back(rewriteSource(e));
            changed |= items.back() != e;
        }
        return changed ? makeSeq(items.begin(), items.end()) : re;
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        RE * r = rewriteSource(rep->getRE());
        return (r != rep->getRE()) ? makeRep(r, rep->getLB(), rep->getUB()) : re;
    }
    return re;
}

RE * CaptureRewriter::rewriteResult(RE * re) {
    if (re == nullptr) return nullptr;
    if (Reference * ref = dyn_cast<Reference>(re)) {
        if (mDeleted.count(ref->getCapture()) != 0) {
            return makeSeq();
        }
        if (ref->getCapture() == mTarget && mReplacement) {
            return mReplacement;
        }
        auto f = mRenumbered.find(ref->getCapture());
        if (f == mRenumbered.end()) return re;
        return makeReference(f->second->getName(), f->second, ref->getInstance());
    } else if (Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(n)) {
            RE * arg = rewriteResult(n->getDefinition());
            return (arg != n->getDefinition()) ? makeFunctionCall(getFunctionID(n), arg) : re;
        }
        return re;
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        std::vector<RE *> items;
        bool changed = false;
        for (RE * e : *seq) {
            items.push_back(rewriteResult(e));
            changed |= items.back() != e;
        }
        return changed ? makeSeq(items.begin(), items.end()) : re;
    }
    return re;
}

void eliminate(ConversionRule * r, std::vector<Rule *> & result) {
    const std::vector<Capture *> nullable = findNullableCaptures(r->getLeftSide());
    if (nullable.empty()) {
        result.push_back(r);
        return;
    }
    // Split on the first nullable capture; the new rules are split further
    // on any remaining nullable captures.
    Capture * target = nullable.front();
    eliminate(CaptureRewriter(target, CaptureRewriter::Mode::NonNull).rewrite(r), result);
    eliminate(CaptureRewriter(target, CaptureRewriter::Mode::Delete).rewrite(r), result);
}

} // end anonymous namespace

std::vector<Capture *> findNullableCaptures(const RuleSide * side, bool includeRepeated) {
    std::vector<Capture *> found;
    findNullableCaptures(side->getBeforeContext(), false, includeRepeated, found);
    findNullableCaptures(side->getText(), false, includeRepeated, found);
    findNullableCaptures(side->getAfterContext(), false, includeRepeated, found);
    return found;
}

std::vector<Rule *> NullableCaptureElimination(const std::vector<Rule *> & rules) {
    std::vector<Rule *> result;
    for (Rule * r : rules) {
        ConversionRule * c = dyn_cast<ConversionRule>(r);
        if (c && c->getDirection() == Direction::Forward) {
            eliminate(c, result);
        } else {
            result.push_back(r);
        }
    }
    return result;
}

}

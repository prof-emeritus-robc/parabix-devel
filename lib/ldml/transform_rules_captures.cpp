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

// An optional item: a repetition with a lower bound of 0, or a variable
// defined as one.
bool isOptionalItem(const RE * re) {
    if (const Rep * rep = dyn_cast<Rep>(re)) {
        return rep->getLB() == 0;
    }
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && isOptionalItem(n->getDefinition());
    }
    return false;
}

// The optional item with a lower bound of 1.
RE * presentForm(RE * item) {
    if (Name * n = dyn_cast<Name>(item)) {
        return presentForm(n->getDefinition());
    }
    Rep * rep = cast<Rep>(item);
    return makeRep(rep->getRE(), 1, rep->getUB());
}

// The optional items of a nullable capture: either the captured RE itself,
// or all the items of a captured sequence of optional items.  Empty if
// the capture is not nullable in this sense.
std::vector<RE *> optionalItems(const Capture * c) {
    RE * captured = c->getCapturedRE();
    if (isOptionalItem(captured)) return {captured};
    std::vector<RE *> items;
    if (const Seq * seq = dyn_cast<Seq>(captured)) {
        for (RE * e : *seq) {
            if (!isOptionalItem(e)) return {};
            items.push_back(e);
        }
    }
    return items;
}

void findNullableCaptures(RE * re, bool withinRep, bool includeRepeated, std::vector<Capture *> & found) {
    if (re == nullptr) return;
    if (Capture * c = dyn_cast<Capture>(re)) {
        if (!optionalItems(c).empty() && (includeRepeated || !withinRep)) found.push_back(c);
        findNullableCaptures(c->getCapturedRE(), withinRep, includeRepeated, found);
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) findNullableCaptures(e, withinRep, includeRepeated, found);
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        findNullableCaptures(rep->getRE(), true, includeRepeated, found);
    }
}

// A fixed string: a single codepoint, a variable defined as a fixed string,
// or a nonempty sequence of fixed strings.
bool isFixedString(const RE * re) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        return cc->size() == 1 && lo_codepoint(cc->front()) == hi_codepoint(cc->front());
    }
    if (const Name * n = dyn_cast<Name>(re)) {
        return !isFunctionCall(n) && n->getDefinition() && isFixedString(n->getDefinition());
    }
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) {
            if (!isFixedString(e)) return false;
        }
        return !seq->empty();
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

//  Rewrites a rule, replacing the content of a target capture.  If the new
//  content is empty, the capture is deleted and its references replaced by
//  the empty string; if it is a fixed string, the capture and its
//  references are replaced by that string.  References to deleted
//  captures are replaced by the empty string.  All captures of the source
//  side are renumbered in order and the references of the result side updated.
class CaptureRewriter {
public:
    CaptureRewriter(Capture * target, RE * newContent, std::set<const Capture *> deleted)
    : mTarget(target), mNewContent(newContent), mDeleted(std::move(deleted)) {
        if (isEmptySeq(newContent)) {
            mDeleted.insert(target);
        } else if (isFixedString(newContent)) {
            mReplacement = newContent;
        }
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
    RE * const mNewContent;
    std::set<const Capture *> mDeleted;
    RE * mReplacement = nullptr;    // the fixed string replacing the target
    std::map<const Capture *, Capture *> mRenumbered;
    unsigned mCaptureCount = 0;
};

RE * CaptureRewriter::rewriteSource(RE * re) {
    if (re == nullptr) return nullptr;
    if (Capture * c = dyn_cast<Capture>(re)) {
        RE * captured = c->getCapturedRE();
        if (c == mTarget) {
            if (mDeleted.count(c) != 0) return makeSeq();
            if (mReplacement) return mReplacement;
            captured = mNewContent;
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
    // Split on the first nullable capture into a rule for each combination
    // of its optional items being present or absent, from all present to
    // none present, the first item varying slowest.   The new rules are
    // split further on any remaining nullable captures.
    Capture * target = nullable.front();
    const std::vector<RE *> items = optionalItems(target);
    const size_t k = items.size();
    for (size_t absent = 0; absent < (size_t{1} << k); absent++) {
        std::vector<RE *> content;
        std::set<const Capture *> deleted;
        for (size_t i = 0; i < k; i++) {
            if ((absent >> (k - 1 - i)) & 1) {
                collectCaptures(items[i], deleted);
            } else {
                content.push_back(presentForm(items[i]));
            }
        }
        RE * newContent = makeSeq(content.begin(), content.end());
        eliminate(CaptureRewriter(target, newContent, std::move(deleted)).rewrite(r), result);
    }
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

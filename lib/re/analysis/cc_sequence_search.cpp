#include <re/analysis/cc_sequence_search.h>
#include <re/adt/adt.h>
#include <re/printer/re_printer.h>
#include <llvm/Support/raw_ostream.h>
#include <climits>

using namespace llvm;
namespace re {

typedef uint64_t stateVector_t;

//  Bit i of a state vector marks that the first i CCs of the sequence may have
//  been matched ending at the current position (bit 0 is always set).  States
//  are over-approximated, so the count of positions at which the full sequence
//  is matched is an upper bound on the occurrences in any matched string.

class ccSequenceSearchObject {
public:
    ccSequenceSearchObject(std::vector<CC *> & CC_seq) :
        mCCseq(CC_seq),
        mInitState(1),
        mFinalState(CC_seq.size() < 63 ? (stateVector_t{1} << CC_seq.size()) : 0),
        mCount(0),
        mFailed(false) {}

    int search(RE * re);

private:
    stateVector_t search_from_state(RE * re, stateVector_t v);
    stateVector_t repeat(RE * repeated, stateVector_t v, int64_t times, bool optional);
    stateVector_t advance(stateVector_t matched, stateVector_t v);
    void addCount(int64_t k);
    void fail() {mFailed = true;}

    std::vector<CC *> mCCseq;
    const stateVector_t mInitState;
    const stateVector_t mFinalState;
    int64_t mCount;
    bool mFailed;
};

void ccSequenceSearchObject::addCount(int64_t k) {
    mCount += k;
    if (mCount > INT_MAX) fail();
}

//  Consume one character matching the sequence positions in matched.
stateVector_t ccSequenceSearchObject::advance(stateVector_t matched, stateVector_t v) {
    const stateVector_t next = ((matched & v) << 1) | mInitState;
    if (next & mFinalState) {
        addCount(1);
    }
    return next & ~mFinalState;
}

//  Apply repeated the given number of times (times < 0: unboundedly often),
//  optionally, i.e., each iteration may also be skipped.
stateVector_t ccSequenceSearchObject::repeat(RE * repeated, stateVector_t v, int64_t times, bool optional) {
    for (int64_t done = 0; (times < 0) || (done < times); ) {
        const int64_t before = mCount;
        stateVector_t next = search_from_state(repeated, v);
        if (optional) next |= v;
        if (mFailed) return v;
        ++done;
        if (next == v) {
            // Further iterations start from the same state and add the same count.
            const int64_t perIteration = mCount - before;
            if (perIteration > 0) {
                if ((times < 0) || (times - done > INT_MAX)) {
                    fail();  // unbounded number of occurrences
                } else {
                    addCount(perIteration * (times - done));
                }
            }
            return v;
        }
        v = next;
    }
    return v;
}

stateVector_t ccSequenceSearchObject::search_from_state(RE * re, stateVector_t v) {
    if (mFailed) return v;
    if (const Name * n = dyn_cast<Name>(re)) {
        RE * defn = n->getDefinition();
        if (defn == nullptr) {
            fail();
            return v;
        }
        return search_from_state(defn, v);
    } else if (Capture * c = dyn_cast<Capture>(re)) {
        return search_from_state(c->getCapturedRE(), v);
    } else if (isa<Reference>(re)) {
        fail();  // the referenced text is unknown
        return v;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * s : *seq) {
            v = search_from_state(s, v);
        }
        return v;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        // Occurrences in the alternatives are summed: an upper bound for any one of them.
        stateVector_t rslt = 0;
        for (RE * a : *alt) {
            rslt |= search_from_state(a, v);
        }
        return rslt;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        const auto lb = rep->getLB();
        const auto ub = rep->getUB();
        v = repeat(rep->getRE(), v, lb, false);
        if (ub == Rep::UNBOUNDED_REP) {
            return repeat(rep->getRE(), v, -1, true);
        }
        return repeat(rep->getRE(), v, ub - lb, true);
    } else if (isa<Assertion>(re)) {
        // Zero-width; ignoring the assertion only over-approximates the states.
        return v;
    } else if (const PropertyExpression * pe = dyn_cast<const PropertyExpression>(re)) {
        if (pe->getKind() == PropertyExpression::Kind::Boundary) {
            return v;  // zero-width
        }
        RE * resolved = pe->getResolvedRE();
        if (LLVM_LIKELY(resolved != nullptr)) {
            return search_from_state(resolved, v);
        }
        fail();
        return v;
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        // A difference matches a subset of its left operand.
        return search_from_state(diff->getLH(), v);
    } else if (const Intersect * ix = dyn_cast<Intersect>(re)) {
        // An intersection matches a subset of either operand.
        return search_from_state(ix->getLH(), v);
    } else if (isa<Start>(re)) {
        return mInitState;
    } else if (isa<End>(re)) {
        return v;
    } else if (isa<Any>(re)) {
        return advance(mFinalState - 1, v);
    } else if (const CC * cc = dyn_cast<CC>(re)) {
        stateVector_t CC_matches = 0;
        for (unsigned i = 0; i < mCCseq.size(); i++) {
            CC_matches |= static_cast<stateVector_t>(cc->intersects(*mCCseq[i])) << i;
        }
        return advance(CC_matches, v);
    } else if (!isa<Range, Group, Permute, Interleavable>(re)) {
        UnexpectedRE("CC_Sequence_Search", re);
    }
    fail();
    return v;
}

int ccSequenceSearchObject::search(RE * re) {
    if (mCCseq.empty() || (mFinalState == 0)) {
        return -1;  // empty or too long for the state vector
    }
    search_from_state(re, mInitState);
    return mFailed ? -1 : static_cast<int>(mCount);
}

int CC_Sequence_Search(std::vector<CC *> & CC_seq, RE * re) {
    return ccSequenceSearchObject(CC_seq).search(re);
}
}

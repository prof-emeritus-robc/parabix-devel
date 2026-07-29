#include <re/adt/adt.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/expand_permutes.h>
#include <re/printer/re_printer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace re {



class ExpandPermutes final : public RE_Transformer {
public:
    ExpandPermutes(const cc::Alphabet * lengthAlpha) : 
        RE_Transformer("ExpandPermutes"), mLengthAlphabet(lengthAlpha) {}
    RE * transformPermute(Permute * p) override;
private:
    const cc::Alphabet * mLengthAlphabet;
};

RE * ExpandPermutes::transformPermute(Permute * p) {
    auto rg =  getLengthRange(p, mLengthAlphabet);
    if (rg.first != rg.second) {
        llvm::report_fatal_error("Variable length permutation terms are prohibited.");
    }
    unsigned total_length = rg.first;
    unsigned total_size = 0;
    for (auto perm : *p) {
        if (Interleavable * s = dyn_cast<Interleavable>(perm)) {
            total_size += s->size();
        } else {
            total_size += 1;
        }
    }
    std::vector<RE *> elems(total_size);
    std::vector<RE *> clauses(total_size + 1);
    unsigned i = 0;
    RE * anyOne = makeAny(mLengthAlphabet);
    for (auto perm : *p) {
        if (Interleavable * s = dyn_cast<Interleavable>(perm)) {
            std::vector<RE *> prior;
            unsigned rem_lgth = total_length;
            RE * repeatable = anyOne;
            for (auto term : * s) {
                unsigned term_lgth = getLengthRange(term, mLengthAlphabet).first;
                rem_lgth = rem_lgth - term_lgth;
                if (!prior.empty()) {
                    RE * anyPrior = makeAlt(prior.begin(), prior.end());
                    repeatable = makeSeq({anyOne, makeNegativeLookBehindAssertion(anyPrior)});
                }
                RE * constraint = makeLookBehindAssertion(makeSeq({term, makeRep(repeatable, 0, rem_lgth)}));
                prior.push_back(term);
                elems[i] = term;
                clauses[i+1] = constraint;
                i++;
            }
        } else {
            unsigned term_lgth = getLengthRange(perm, mLengthAlphabet).first;
            unsigned rem_lgth = total_length - term_lgth;
            RE * constraint = makeLookBehindAssertion(makeSeq({perm, makeRep(anyOne, 0, rem_lgth)}));
            elems[i] = perm;
            clauses[i+1] = constraint;
            i++;
        }
    }
    clauses[0] = makeRep(makeAlt(elems.begin(), elems.end()), elems.size(), elems.size());
    return makeSeq(clauses.begin(), clauses.end());
}

RE * expandPermutes(RE * r, const cc::Alphabet * lengthAlpha) {
    return ExpandPermutes(lengthAlpha).transformRE(r);
}

}

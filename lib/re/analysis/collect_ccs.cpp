#include <re/analysis/collect_ccs.h>

#include <re/adt/adt.h>
#include <re/alphabet/alphabet.h>
#include <re/analysis/re_inspector.h>

#include <boost/container/flat_set.hpp>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace re {

struct CC_Collector final : public RE_Inspector {

    CC_Collector(re::NameProcessingMode m)
    : RE_Inspector(m, InspectionMode::IgnoreNonUnique)
    , mUnionCC(makeCC()) {

    }
    void inspectAssertion(Assertion * a) final {
        // assertions cannot add any characters to
        // matched strings.
    }

    void inspectDiff(Diff * d) final {
        inspectRE(d->getLH());
    }

    void inspectIntersect(Intersect * ix) final {
        inspectRE(ix->getLH());
    }

    void inspectCC(CC * cc) final {
        if (mUnionCC->empty()) {
            mUnionCC = cc;
        } else {
            mUnionCC = makeCC(cc, mUnionCC);
        }
    }

    CC * mUnionCC;
};

CC * unionCC(RE * re, re::NameProcessingMode m) {
    CC_Collector collector(m);
    collector.inspectRE(re);
    return collector.mUnionCC;
}

struct SetCollector final : public RE_Inspector {

    SetCollector(const cc::Alphabet * alphabet, re::NameProcessingMode m, CC_Set & ccs)
    : RE_Inspector(m, InspectionMode::IgnoreNonUnique)
    , alphabet(alphabet)
    , ccs(ccs) {

    }

    void inspectCC(CC * cc) final {
        if (LLVM_LIKELY(cc->getAlphabet() == alphabet)) {
            ccs.push_back(cc);
        }
    }

private:
    const cc::Alphabet * const alphabet;
    CC_Set & ccs;
};


CC_Set collectCCs(RE * const re, const cc::Alphabet & a, re::NameProcessingMode m) {
    CC_Set ccs;
    SetCollector collector(&a, m, ccs);
    collector.inspectRE(re);
    return ccs;
}




}

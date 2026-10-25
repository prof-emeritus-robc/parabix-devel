#include <re/analysis/collect_ccs.h>

#include <re/adt/adt.h>
#include <re/alphabet/alphabet.h>
#include <re/analysis/re_inspector.h>
#include <ucd/core/unicode_set.h>

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

    void inspectString(String * s) final {
        inspectStringAsCCs(s);
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

    void inspectString(String * s) final {
        if (alphabet == &cc::Unicode) inspectStringAsCCs(s);
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




bool matchableCodepoints(RE * re, UCD::UnicodeSet & chars) {
    if (CC * const cc = dyn_cast<CC>(re)) {
        if (cc->getAlphabet() != &cc::Unicode) return false;
        chars = chars + *cc;
        return true;
    } else if (const String * const s = dyn_cast<String>(re)) {
        for (const char32_t c : *s) chars.insert(c);
        return true;
    } else if (isa<Any>(re)) {
        chars = UCD::UnicodeSet(0, UCD::UNICODE_MAX);
        return true;
    } else if (isa<Start>(re) || isa<End>(re) || isa<Reference>(re)) {
        // (A reference matches the text of its capture.)
        return true;
    } else if (Seq * const seq = dyn_cast<Seq>(re)) {
        for (RE * e : *seq) if (!matchableCodepoints(e, chars)) return false;
        return true;
    } else if (Alt * const alt = dyn_cast<Alt>(re)) {
        for (RE * e : *alt) if (!matchableCodepoints(e, chars)) return false;
        return true;
    } else if (Rep * const rep = dyn_cast<Rep>(re)) {
        return matchableCodepoints(rep->getRE(), chars);
    } else if (Assertion * const a = dyn_cast<Assertion>(re)) {
        return matchableCodepoints(a->getAsserted(), chars);
    } else if (Diff * const d = dyn_cast<Diff>(re)) {
        return matchableCodepoints(d->getLH(), chars);
    } else if (Intersect * const x = dyn_cast<Intersect>(re)) {
        return matchableCodepoints(x->getLH(), chars);
    } else if (Capture * const c = dyn_cast<Capture>(re)) {
        return matchableCodepoints(c->getCapturedRE(), chars);
    } else if (Group * const g = dyn_cast<Group>(re)) {
        return matchableCodepoints(g->getRE(), chars);
    } else if (Name * const n = dyn_cast<Name>(re)) {
        return n->getDefinition() && matchableCodepoints(n->getDefinition(), chars);
    }
    return false;
}

}

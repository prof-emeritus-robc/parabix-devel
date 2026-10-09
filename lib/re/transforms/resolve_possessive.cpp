#include <re/transforms/re_star_normal.h>
#include <re/transforms/re_transformer.h>
#include <re/adt/adt.h>
#include <re/analysis/nullable.h>
#include <llvm/ADT/SmallVector.h>

using namespace llvm;

namespace re {

class ResolvePossessive final : public RE_Transformer {
public:
    ResolvePossessive() : RE_Transformer("ResolvePossessive") {}
    RE * transformRep(Rep * rep) override;
};

RE * ResolvePossessive::transformRep(Rep * rep) {
    RE * e0 = rep->getRE();
    int lb = rep->getLB();
    int ub = rep->getUB();
    Rep::Kind k = rep->getKind();
    RE * e = transform(e0);
    if (k == Rep::Kind::Possessive) {
        if (ub == Rep::UNBOUNDED_REP) {
            return Seq::Create({Rep::Create(e, lb, ub),
                                Assertion::Create(e, Assertion::Kind::LookAhead,
                                                     Assertion::Sense::Negative)});
        } else if (lb == ub) {
            return Rep::Create(e, ub, ub);
        } else /* if (lb < ub) */{
            return Alt::Create({Seq::Create({makeRep(e, lb, ub-1),
                                             Assertion::Create(e, Assertion::Kind::LookAhead,
                                                                  Assertion::Sense::Negative)}),
                                Rep::Create(e, ub, ub)});
        }
    }
    if (e == e0) return rep;
    return Rep::Create(e, lb, ub);
}

RE * resolvePossessiveQuantifiers(RE * re) {
    return ResolvePossessive().transformRE(re);
}

}

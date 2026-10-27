#include <re/adt/adt.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/resolve_ranges.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace re {

class RangeResolver final : public RE_Transformer {
public:
    RangeResolver() : RE_Transformer("RangeResolver") {}
protected:
    RE * transformRange(Range * r) override;
};

RE * RangeResolver::transformRange(Range * r) {
    RE * lo = r->getLo();
    if (Name * n = dyn_cast<Name>(lo)) {
        lo = n->getDefinition();
    }
    RE * hi = r->getHi();
    if (Name * n = dyn_cast<Name>(hi)) {
        hi = n->getDefinition();
    }
    if (lo && hi && isa<CC>(lo) && isa<CC>(hi)) {
        CC * const cc_lo = cast<CC>(lo);
        CC * const cc_hi = cast<CC>(hi);
        if (LLVM_LIKELY((cc_lo->count() == 1) && (cc_hi->count() == 1))) {
            const auto lo_val = cc_lo->at(0);
            const auto hi_val = cc_hi->at(0);
            if (LLVM_LIKELY(lo_val <= hi_val)) {
                return makeCC(lo_val, hi_val, dyn_cast<CC>(hi)->getAlphabet());
            }
        }
    }
    report_fatal_error("Error in Range resolution - undefined or invalid bounds");
}

RE * resolveRanges(RE * r) {
    return RangeResolver().transformRE(r);
}

}

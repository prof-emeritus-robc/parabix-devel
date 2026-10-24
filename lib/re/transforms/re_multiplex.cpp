#include <re/transforms/re_multiplex.h>

#include <functional>
#include <re/adt/adt.h>
#include <re/alphabet/alphabet.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_transformer.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace re {


struct CC_multiplexer final : public RE_Transformer {
public:
    CC_multiplexer(const cc::MultiplexedAlphabet * mpx) :
        RE_Transformer("Multiplex_" + mpx->getName()),
        mMultiplexedAlphabet(mpx) {}
    RE * transformCC(CC * cc) override {
        if (cc->getAlphabet() == mMultiplexedAlphabet->getSourceAlphabet()) {
            return mMultiplexedAlphabet->transformCC(cc);
        }
        return cc;
    };
    RE * transformString(String * s) override {
        if (mMultiplexedAlphabet->getSourceAlphabet() == &cc::Unicode) {
            return transformStringAsSeq(s);
        }
        return s;
    }
private:
    const cc::MultiplexedAlphabet * const mMultiplexedAlphabet;
};

RE * transformCCs(const cc::MultiplexedAlphabet * const mpx, RE * re, NameTransformationMode mode) {
    return CC_multiplexer(mpx).transformRE(re, mode);
}

}

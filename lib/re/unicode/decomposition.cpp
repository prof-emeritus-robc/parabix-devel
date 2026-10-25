/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/unicode/decomposition.h>

#include <string>
#include <vector>
#include <llvm/Support/Casting.h>
#include <re/adt/adt.h>
#include <re/transforms/re_transformer.h>
#include <ucd/core/unicode_set.h>
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>
#include <ucd/data/PropertyValueAliases.h>


using namespace llvm;
using namespace re;

namespace UCD {
    
// The string of the singleton Unicode CCs and Strings of s from position, and
// the number of elements of s that it spans.
static inline std::u32string getStringPiece(Seq * s, unsigned position, unsigned & elements) {
    unsigned pos = position;
    unsigned size = s->size();
    std::u32string rslt;
    while (pos < size) {
        RE * const e = (*s)[pos];
        if (String * const str = dyn_cast<String>(e)) {
            // The singleton CCs of a String continue the string piece.
            rslt.append(str->begin(), str->end());
        } else if (CC * const cc = dyn_cast<CC>(e)) {
            if (cc->empty()) break;
            if (cc->getAlphabet() != &cc::Unicode) break;
            codepoint_t lo = lo_codepoint(cc->front());
            codepoint_t hi = hi_codepoint(cc->back());
            if (lo != hi) // not a singleton CC; end of the string piece.
                break;
            rslt.push_back(lo);
        } else {
            break;
        }
        pos++;
    }
    elements = pos - position;
    return rslt;
}

class NFD_Transformer final : public re::RE_Transformer {
public:
    /* Transforme an RE so that all string pieces and character classes
     are converted to NFD form (or NFKD form if the Compatible option
     is used.  The options may also including case folding.  Example:
     NFD_Transformer(CaseFold | NFKD).transformRE(r);
    */
    NFD_Transformer(DecompositionOptions opt, NFD_Engine & engine);
protected:
    re::RE * transformCC(re::CC * cc) override;
    re::RE * transformString(re::String * s) override;
    re::RE * transformSeq(re::Seq * seq) override;
    re::RE * transformGroup(re::Group * g) override;
private:
    NFD_Engine & mEngine;
    DecompositionOptions mOptions;
};
    
NFD_Transformer::NFD_Transformer(DecompositionOptions opt, NFD_Engine & engine) :
RE_Transformer("toNFD"),
mEngine(engine),
mOptions(opt) {
}

RE * NFD_Transformer::transformGroup(Group * g) {
    re::Group::Mode mode = g->getMode();
    re::Group::Sense sense = g->getSense();
    auto r = g->getRE();
    DecompositionOptions opts;
    if (mode == re::Group::Mode::CaseInsensitiveMode) {
        if (sense == re::Group::Sense::On) {
            opts = static_cast<DecompositionOptions>(mOptions | CaseFold);
        } else {
            opts = static_cast<DecompositionOptions>(mOptions & ~CaseFold);
        }
    } else if (mode == re::Group::Mode::CompatibilityMode) {
        if (sense == re::Group::Sense::On) {
            opts = static_cast<DecompositionOptions>(mOptions | NFKD);
        } else {
            opts = static_cast<DecompositionOptions>(mOptions & ~NFKD);
        }
    } else {
        opts = mOptions;
    }
    RE * t;
    if (opts == mOptions) {
        t = transform(r);
    } else {
        t = toNFD(r, opts);
    }
    if (t == r) return g;
    return makeGroup(mode, t, sense);
}

RE * NFD_Transformer::transformCC(CC * cc) {
    if (cc->getAlphabet() != &cc::Unicode) return cc;
    std::vector<RE *> alts;
    CC * finalCC = cc;
    for (const interval_t i : *cc) {
        for (codepoint_t cp = lo_codepoint(i); cp <= hi_codepoint(i); cp++) {
            std::u32string decomp;
            mEngine.NFD_append1(decomp, cp);
            if (decomp.size() == 1) {
                finalCC = makeCC(finalCC, makeCC(decomp[0]));
            } else {
                alts.push_back(u32string2re(decomp));
            }
        }
    }
    if (!finalCC->empty()) alts.push_back(finalCC);
    return makeAlt(alts.begin(), alts.end());
}

// A String is the Seq of its singleton CCs: of one CC (as a Seq of one CC is
// that CC), or a string piece.
RE * NFD_Transformer::transformString(String * s) {
    if (s->size() <= 1) return transformStringAsSeq(s);
    std::u32string stringPiece = s->getString();
    std::u32string decomposed;
    mEngine.NFD_append(decomposed, stringPiece);
    if (decomposed == stringPiece) return s;
    return makeString(decomposed);
}

RE * NFD_Transformer::transformSeq(Seq * seq) {
    // find and process all string pieces
    unsigned size = seq->size();
    if (size == 0) return seq;
    std::vector<RE *> list;
    unsigned i = 0;
    bool unchanged = true;
    while (i < size) {
        unsigned elements;
        std::u32string stringPiece = getStringPiece(seq, i, elements);
        if (stringPiece.size() > 0) {
            std::u32string s;
            mEngine.NFD_append(s, stringPiece);
            if (s != stringPiece) unchanged = false;
            list.push_back(u32string2re(s));
            i += elements;
        } else {
            RE * r = (*seq)[i];
            RE * t = transform(r);
            if (t != r) unchanged = false;
            list.push_back(t);
            i++;
        }
    }
    if (unchanged) return seq;
    return makeSeq(list.begin(), list.end());
}

RE * toNFD(RE * re, const DecompositionOptions opt) {
    NFD_Engine engine(opt);
    return NFD_Transformer(opt, engine).transformRE(re);
}

} // end namespace UCD

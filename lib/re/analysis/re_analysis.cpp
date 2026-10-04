#include <re/analysis/re_analysis.h>

#include <limits.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <re/adt/adt.h>
#include <re/printer/re_printer.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/analysis/cc_sequence_search.h>
#include <re/analysis/validation.h>
#include <re/transforms/remove_nullable.h>
#include <re/transforms/to_utf8.h>
#include <ucd/core/unicode_set.h>
#include <ucd/utf/utf_encoder.h>
#include <ucd/data/PropertyObjectTable.h>
#include <util/small_flat_set.hpp>

using namespace llvm;

namespace re {

std::pair<int, int> getLengthRange(const RE * re, const cc::Alphabet * indexAlphabet) {
    // Each position of a multiplexed alphabet is one unit of its source alphabet.
    if (const auto * mpx = dyn_cast_or_null<cc::MultiplexedAlphabet>(indexAlphabet)) {
        indexAlphabet = mpx->getSourceAlphabet();
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        std::pair<int, int> range = std::make_pair(INT_MAX, 0);
        for (const RE * a : *alt) {
            auto a_range = getLengthRange(a, indexAlphabet);
            range.first = std::min<int>(range.first, a_range.first);
            range.second = std::max<int>(range.second, a_range.second);
        }
        return range;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        std::pair<int, int> range = std::make_pair(0, 0);
        for (const RE * re : *seq) {
            auto tmp = getLengthRange(re, indexAlphabet);
            if (LLVM_LIKELY(tmp.first < (INT_MAX - range.first))) {
                range.first += tmp.first;
            } else {
                range.first = INT_MAX;
            }
            if (LLVM_LIKELY(tmp.second < (INT_MAX - range.second))) {
                range.second += tmp.second;
            } else {
                range.second = INT_MAX;
            }
        }
        return range;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        auto range = getLengthRange(rep->getRE(), indexAlphabet);
        if (LLVM_LIKELY(rep->getLB() != Rep::UNBOUNDED_REP && range.first < INT_MAX)) {
            range.first *= rep->getLB();
        } else {
            range.first = INT_MAX;
        }
        if (LLVM_LIKELY(rep->getUB() != Rep::UNBOUNDED_REP && range.second < INT_MAX)) {
            range.second *= rep->getUB();
        } else {
            range.second = INT_MAX;
        }
        return range;
    } else if (isa<Assertion>(re) || isa<Start>(re) || isa<End>(re)) {
        return std::make_pair(0, 0);
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        // The range is determined by the first operand only.
        return getLengthRange(diff->getLH(), indexAlphabet);
    } else if (const Intersect * i = dyn_cast<Intersect>(re)) {
        const auto r1 = getLengthRange(i->getLH(), indexAlphabet);
        const auto r2 = getLengthRange(i->getRH(), indexAlphabet);
        // The matched string cannot be shorter than the largest of the min lengths
        // nor can it be longer than the smallest of the max lengths.
        return std::make_pair(std::max(r1.first, r2.first), std::min(r1.second, r2.second));
    } else if (const CC * cc = dyn_cast<CC>(re)) {
        auto alphabet = cc->getAlphabet();
        if (const cc::MultiplexedAlphabet * a = dyn_cast<cc::MultiplexedAlphabet>(alphabet)) {
            alphabet = a->getSourceAlphabet();
        }
        if (isa<cc::CodeUnitAlphabet>(alphabet)) return std::make_pair(1, 1);
        if (indexAlphabet == alphabet) return std::make_pair(1, 1);
        if ((indexAlphabet == &cc::UTF8) && (alphabet == &cc::Unicode)) {
            UTF_Encoder UTF8_Encoder(8);
            return std::make_pair(UTF8_Encoder.encoded_length(lo_codepoint(cc->front())),
                                  UTF8_Encoder.encoded_length(hi_codepoint(cc->back())));
        }
        return std::make_pair(1, INT_MAX);
    } else if (const Any * a = dyn_cast<Any>(re)) {
        if (indexAlphabet == a->getAlphabet()) return std::make_pair(1, 1);
        if (indexAlphabet == &cc::UTF8) {
            return std::make_pair(1, 4);
        }
        return std::make_pair(1, INT_MAX);
    } else if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        if (pe->getKind() == PropertyExpression::Kind::Boundary) {
            return std::make_pair(0, 0);
        }
        if (indexAlphabet == &cc::Unicode) return std::make_pair(1, 1);
        RE * resolved = pe->getResolvedRE();
        if (resolved) return getLengthRange(resolved, indexAlphabet);
        if (indexAlphabet == &cc::UTF8) {
            return std::make_pair(1, 4);
        }
        return std::make_pair(1, INT_MAX);
    } else if (const Permute * p = dyn_cast<Permute>(re)) {
        std::pair<int, int> range = std::make_pair(0, 0);
        for (const RE * term : *p) {
            auto tmp = getLengthRange(term, indexAlphabet);
            if (LLVM_LIKELY(tmp.first < (INT_MAX - range.first))) {
                range.first += tmp.first;
            } else {
                range.first = INT_MAX;
            }
            if (LLVM_LIKELY(tmp.second < (INT_MAX - range.second))) {
                range.second += tmp.second;
            } else {
                range.second = INT_MAX;
            }
        }
        return range;
    } else if (const Interleavable * s = dyn_cast<Interleavable>(re)) {
        std::pair<int, int> range = std::make_pair(0, 0);
        for (const RE * term : *s) {
            auto tmp = getLengthRange(term, indexAlphabet);
            if (LLVM_LIKELY(tmp.first < (INT_MAX - range.first))) {
                range.first += tmp.first;
            } else {
                range.first = INT_MAX;
            }
            if (LLVM_LIKELY(tmp.second < (INT_MAX - range.second))) {
                range.second += tmp.second;
            } else {
                range.second = INT_MAX;
            }
        }
        return range;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        RE * defn = n->getDefinition();
        if (defn) return getLengthRange(defn, indexAlphabet);
        return std::make_pair(0, INT_MAX);
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return getLengthRange(c->getCapturedRE(), indexAlphabet);
    } else if (const Reference * r = dyn_cast<Reference>(re)) {
        return getLengthRange(r->getCapture(), indexAlphabet);
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return getLengthRange(g->getRE(), indexAlphabet);
    } else if (const Range * rg = dyn_cast<Range>(re)) {
        // A single character between the two endpoints.
        const auto lo = getLengthRange(rg->getLo(), indexAlphabet);
        const auto hi = getLengthRange(rg->getHi(), indexAlphabet);
        return std::make_pair(std::min(lo.first, hi.first), std::max(lo.second, hi.second));
    }
    UnexpectedRE("getLengthRange", re);
}

struct LookaheadLengthInspector : public RE_Inspector {
    LookaheadLengthInspector(const cc::Alphabet * lengthAlpha) : RE_Inspector(), 
        mLengthAlphabet(lengthAlpha), mMaxLA(0) {}

    void inspectAssertion(Assertion * a) override {
        if (a->getKind() == Assertion::Kind::LookAhead) {
            auto r = getLengthRange(a->getAsserted(), mLengthAlphabet);
            mMaxLA = std::max(mMaxLA, static_cast<unsigned>(r.second));
        }
    }

    void inspectPropertyExpression(PropertyExpression * pe) override {
        if (pe->getKind() == PropertyExpression::Kind::Boundary) {
            if (pe->getPropertyIdentifier() == "w") {
                // Level 2 word boundaries have lookahead up to 2 Unicode cps. 
                if (mLengthAlphabet == &cc::UTF8) {
                    mMaxLA = std::max(mMaxLA, 8U);
                } else {
                    mMaxLA = std::max(mMaxLA, 2U);
                }
            } else {
                if (mLengthAlphabet == &cc::UTF8) {
                    mMaxLA = std::max(mMaxLA, 4U);
                } else {
                    mMaxLA = std::max(mMaxLA, 1U);
                }
            }
        }
    }

    const cc::Alphabet * mLengthAlphabet;
    unsigned mMaxLA;
};

unsigned maxLookaheadLength(const RE * re, const cc::Alphabet * lengthAlpha) {
    LookaheadLengthInspector visitor(lengthAlpha);
    visitor.inspectRE(const_cast<RE *>(re));
    return visitor.mMaxLA;
}

CC * resolveToCC(RE * r) {
    if (r == nullptr) return nullptr;
    if (CC * cc = dyn_cast<CC>(r)) {
        return cc;
    } else if (PropertyExpression * pe = dyn_cast<PropertyExpression>(r)) {
        return resolveToCC(pe->getResolvedRE());
    } else if (Name * n = dyn_cast<Name>(r)) {
        return resolveToCC(n->getDefinition());
    } else if (Capture * c = dyn_cast<Capture>(r)) {
        return resolveToCC(c->getCapturedRE());
    } else if (Reference * ref = dyn_cast<Reference>(r)) {
        return resolveToCC(ref->getCapture());
    }
    return nullptr;
}

std::pair<RE *, RE *> ParseUniquePrefix(RE * r) {
    if (Seq * seq = dyn_cast<Seq>(r)) {
        if (seq->size() < 2) {
            // No parse possible.
            return std::make_pair(makeSeq(), r);
        }
        // A start symbol (^) is always an unambiguous prefix.
        if (isa<Start>(seq->front())) {
            return std::make_pair(seq->front(),
                                  makeSeq(seq->begin()+1, seq->end()));
        }
        // For ambiguity testing, we need to know if an initial
        // CC sequence can be matched other than at the beginning
        // of the RE, i.e., by any suffix.
        RE * suffix1 = makeSeq(seq->begin()+1, seq->end());
        // Extend the prefix one CC at a time until it cannot occur
        // anywhere within the suffix (leaving a nonempty suffix).
        std::vector<CC *> prefixCCs;
        for (unsigned i = 0; i < seq->size() - 1; ++i) {
            RE * item = (*seq)[i];
            if (CC * cc1 = resolveToCC(item)) {
                prefixCCs.push_back(cc1);
                if (CC_Sequence_Search(prefixCCs, suffix1) == 0) {
                    // Unambiguous prefix found!
                    return std::make_pair(makeSeq(seq->begin(), seq->begin()+i+1),
                                          makeSeq(seq->begin()+i+1, seq->end()));
                }
                continue;
            }
            //  We don't have an expression resolving to a CC.
            //  But if we have a zerowidth item, we can simply
            //  skip it and continue to look for a CC sequence.
            if (isa<Assertion>(item)) {
                continue;
            } else if (PropertyExpression * pe = dyn_cast<PropertyExpression>(item)) {
                if (pe->getKind() == PropertyExpression::Kind::Boundary) {
                    continue;
                }
            }
            //  Otherwise we report that a unique CC sequence prefix cannot be found.
            return std::make_pair(makeSeq(), r);
        }
    }
    return std::make_pair(makeSeq(), r);
}


int minMatchLength(const RE * re) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        int minAltLength = INT_MAX;
        for (RE * re : *alt) {
            minAltLength = std::min(minAltLength, minMatchLength(re));
        }
        return minAltLength;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        int minSeqLength = 0;
        for (RE * re : *seq) {
            minSeqLength += minMatchLength(re);
        }
        return minSeqLength;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        if (rep->getLB() == 0) return 0;
        else return (rep->getLB()) * minMatchLength(rep->getRE());
    } else if (isa<Assertion>(re)) {
        return 0;
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return minMatchLength(diff->getLH());
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        return std::min(minMatchLength(e->getLH()), minMatchLength(e->getRH()));
    } else if (isa<Any>(re)) {
        return 1;
    } else if (isa<CC>(re)) {
        return 1;
    } else if (const Name * n = dyn_cast<Name>(re)) {
        // An undefined (external) name has an unknown length.
        const RE * const defn = n->getDefinition();
        return defn ? minMatchLength(defn) : 0;
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return minMatchLength(c->getCapturedRE());
    } else if (const Reference * r = dyn_cast<Reference>(re)) {
        return minMatchLength(r->getCapture());
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return minMatchLength(g->getRE());
    } else if (isa<Range>(re)) {
        return 1;
    } else if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        return (pe->getKind() == PropertyExpression::Kind::Codepoint) ? 1 : 0;
    } else if (const Permute * p = dyn_cast<Permute>(re)) {
        int minLength = 0;
        for (RE * term : *p) {
            minLength += minMatchLength(term);
        }
        return minLength;
    } else if (const Interleavable * s = dyn_cast<Interleavable>(re)) {
        int minLength = 0;
        for (RE * term : *s) {
            minLength += minMatchLength(term);
        }
        return minLength;
    } else if (isa<Start, End>(re)) {
        return 0;
    }
    UnexpectedRE("minMatchLength", re);
}


struct FixedUTF8Validator : public RE_Validator {
    FixedUTF8Validator() : RE_Validator("FixedUTF8Validator") {}

    bool validateCC(const CC * cc) override {
        auto alphabet = cc->getAlphabet();
        if (const cc::MultiplexedAlphabet * a = dyn_cast<cc::MultiplexedAlphabet>(alphabet)) {
            alphabet = a->getSourceAlphabet();
        }
        if (alphabet == &cc::Unicode) {
            UTF_Encoder UTF8_Encoder(8);
            auto min_lgth = UTF8_Encoder.encoded_length(lo_codepoint(cc->front()));
            auto max_lgth = UTF8_Encoder.encoded_length(hi_codepoint(cc->back()));
            return min_lgth == max_lgth;
        }
        return (alphabet == &cc::UTF8) || (alphabet == &cc::Byte);
    }

    bool validateAny(const Any * a) override {
        return a->getAlphabet() == &cc::UTF8;
    }

    bool validateName(const Name * name) override {
        return false;
    }

    bool validatePropertyExpression(const PropertyExpression * pe) override {
        return false;
    }
};

bool validateFixedUTF8(const RE * r) {
    return FixedUTF8Validator().validateRE(r);
}

struct ReferenceFree : public RE_Validator {
    ReferenceFree() : RE_Validator("ReferenceFree") {}

    bool validateReference(const Reference * ref) override {
        return false;
    }
};

bool hasReference(const RE * r) {
    return !ReferenceFree().validateRE(r);
}

struct AssertionFree : public RE_Validator {
    AssertionFree() : RE_Validator("AssertionFree") {}

    bool validateAssertion(const Assertion * a) override {
        return false;
    }
};

bool hasAssertion(const RE * r) {
    return !AssertionFree().validateRE(r);
}

struct PropertyReferenceFree : public RE_Validator {
    PropertyReferenceFree() : RE_Validator("PropertyReferenceFree") {}

    bool validateReference(const Reference * ref) override {
        UCD::property_t p = ref->getReferencedProperty();
        return p != UCD::identity;
    }
};

bool hasPropertyReference(const RE * r) {
    return !PropertyReferenceFree().validateRE(r);
}

struct ByteTestComplexity {

    void gatherTests(RE * re);

    UCD::UnicodeSet equalityTests;
    UCD::UnicodeSet lessThanTests;
    unsigned testCount;
    unsigned testLimit;
};

void ByteTestComplexity::gatherTests(RE * re) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        for (const auto range : *cc) {
            const auto lo = re::lo_codepoint(range);
            const auto hi = re::hi_codepoint(range);
            if (lo == hi) {
                if (!equalityTests.contains(lo)) {
                    equalityTests.insert(lo);
                    testCount++;
                }
            } else {
                if (lo > 0) {
                    if (!lessThanTests.contains(lo)) {
                        lessThanTests.insert(lo);
                        testCount++;
                    }
                }
                if (hi < 0xFF) {
                    if (!lessThanTests.contains(hi+1)) {
                        lessThanTests.insert(hi+1);
                        testCount++;
                    }
                }
            }
            if (testCount > testLimit) return;
        }
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (RE * const defn = n->getDefinition()) {
            gatherTests(defn);
        }
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (RE * item : *alt) {
            gatherTests(item);
        }
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (RE * item : *seq) {
            gatherTests(item);
        }
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        gatherTests(a->getAsserted());
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        gatherTests(rep->getRE());
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        gatherTests(diff->getLH());
        gatherTests(diff->getRH());
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        gatherTests(e->getLH());
        gatherTests(e->getRH());
    } else if (const Group * g = dyn_cast<Group>(re)) {
        gatherTests(g->getRE());
    } else if (const Range * rg = dyn_cast<Range>(re)) {
        gatherTests(rg->getLo());
        gatherTests(rg->getHi());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        gatherTests(c->getCapturedRE());
    } else if (const Reference * r = dyn_cast<Reference>(re)) {
        gatherTests(r->getCapture());
    } else if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        if (RE * resolved = pe->getResolvedRE()) {
            gatherTests(resolved);
        }
    } else if (const Permute * p = dyn_cast<Permute>(re)) {
        for (RE * term : *p) {
            gatherTests(term);
        }
    } else if (const Interleavable * s = dyn_cast<Interleavable>(re)) {
        for (RE * term : *s) {
            gatherTests(term);
        }
    } else if (!isa<Any, Start, End>(re)) {
        UnexpectedRE("ByteTestComplexity::gatherTests", re);
    }
}

bool byteTestsWithinLimit(RE * re, unsigned limit) {
    ByteTestComplexity btc_object;
    btc_object.testCount = 0;
    btc_object.testLimit = limit;
    btc_object.gatherTests(re);
    return btc_object.testCount <= btc_object.testLimit;
}

bool hasEndAnchor(const RE * re) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * re : *alt) {
            if (!hasEndAnchor(re)) {
                return false;
            }
        }
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        return (!seq->empty()) && hasEndAnchor(seq->back());
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        return hasEndAnchor(rep->getRE());
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return hasEndAnchor(diff->getLH());
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        return hasEndAnchor(e->getLH()) && hasEndAnchor(e->getRH());
    } else if (isa<End>(re)) {
        return true;
    } else if (isa<Any, Assertion, CC, Name, PropertyExpression, Capture, Reference,
                   Group, Start, Range, Permute, Interleavable>(re)) {
        return false;
    }
    UnexpectedRE("hasEndAnchor", re);
}

class EndFreeValidator : public RE_Validator {
public:
    EndFreeValidator() : RE_Validator("EndFreeValidator", NameProcessingMode::ProcessDefinition) {}
    bool validateEnd(const End * e) override {return false;}
};

bool anyEndAnchor(const RE * re) {
    return !EndFreeValidator().validateRE(re);
}



//  Track the UTF-8 decoding state through re: pending is the number of
//  continuation bytes still expected, and chars counts completed characters.
//  Every string matched by re must drive the state identically; otherwise
//  (or for anything other than UTF-8 code unit CCs) return false.
static bool trackUTF8Characters(const RE * re, unsigned & pending, unsigned & chars) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        if ((cc->getAlphabet() != &cc::UTF8) || cc->empty()) return false;
        const auto lo = lo_codepoint(cc->front());
        const auto hi = hi_codepoint(cc->back());
        if (pending > 0) {
            if ((lo < 0x80) || (hi > 0xBF)) return false;
            if (--pending == 0) ++chars;
        } else if (hi <= 0x7F) {
            ++chars;
        } else if ((lo >= 0xC0) && (hi <= 0xDF)) {
            pending = 1;
        } else if ((lo >= 0xE0) && (hi <= 0xEF)) {
            pending = 2;
        } else if ((lo >= 0xF0) && (hi <= 0xF7)) {
            pending = 3;
        } else {
            return false;
        }
        return true;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) {
            if (!trackUTF8Characters(e, pending, chars)) return false;
        }
        return true;
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        if (alt->empty()) return false;
        bool first = true;
        unsigned altPending = 0, altChars = 0;
        for (const RE * e : *alt) {
            unsigned p = pending, c = chars;
            if (!trackUTF8Characters(e, p, c)) return false;
            if (first) {
                altPending = p; altChars = c; first = false;
            } else if ((p != altPending) || (c != altChars)) {
                return false;
            }
        }
        pending = altPending;
        chars = altChars;
        return true;
    }
    return false;
}

//  Does every string matched by re encode exactly one character in UTF-8?
bool isUTF8EncodedCharacter(const RE * re) {
    unsigned pending = 0, chars = 0;
    return trackUTF8Characters(re, pending, chars) && (pending == 0) && (chars == 1);
}

# define End_Lookahead 1
unsigned grepOffset(const RE * re) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        unsigned altOffset = 0;
        for (const RE * re : *alt) {
            altOffset = std::max(altOffset, grepOffset(re));
        }
        return altOffset;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        if (seq->empty()) return 1;
        for (auto i = seq->rbegin(); i != seq->rend(); ++i) {
            unsigned o = grepOffset(*i);
            if (!isa<Assertion>(*i) && !isa<End>(*i)) return o;
            if (o == 1) return o;
        }
        return 1;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        if (rep->getUB() == Rep::UNBOUNDED_REP) return 1;
        return grepOffset(rep->getRE());
    } else if (isa<Start>(re)) {
        return 1;
    } else if (isa<End>(re)) {
        return 1 - End_Lookahead;
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        if (a->getKind() == Assertion::Kind::LookBehind) {
            return grepOffset(a->getAsserted());
        }
        // A single character lookahead is compiled in place, leaving the
        // marker on the following position (which, for a negative lookahead,
        // may be past the end of the data).  Longer lookaheads are named
        // externals (LookAheadNamer, using the same measure) and zero-width
        // ones leave the marker where it was.
        if (getLengthRange(a->getAsserted(), &cc::Unicode).second == 1) {
            return 1;
        }
        return 0;
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        return grepOffset(diff->getLH());
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        return std::min(grepOffset(e->getLH()), grepOffset(e->getRH()));
    } else if (isa<Any>(re)) {
        return 0;
    } else if (isa<CC>(re)) {
        return 0;
    } else if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        if (pe->getKind() == PropertyExpression::Kind::Boundary) {
            return 1;
        }
        return 0;
    } else if (const Group * g = dyn_cast<Group>(re)) {
        return grepOffset(g->getRE());
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return grepOffset(n->getDefinition());
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return grepOffset(c->getCapturedRE());
    } else if (isa<Range, Reference, Permute, Interleavable>(re)) {
        return 0;
    }
    UnexpectedRE("grepOffset", re);
}

}

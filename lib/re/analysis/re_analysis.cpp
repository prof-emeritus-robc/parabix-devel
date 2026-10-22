#include <re/analysis/re_analysis.h>

#include <limits.h>
#include <algorithm>
#include <vector>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <re/adt/adt.h>
#include <re/printer/re_printer.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/analysis/cc_sequence_search.h>
#include <re/analysis/validation.h>
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

//  The offset of the match marker after re (0: at the last code unit of the
//  match; 1: at the next character), given the offset of the marker before
//  it, following RE_Block_Compiler: a sequence folds the marker through its
//  items, an alternation aligns its branches to the furthest (AlignMarkers),
//  and zero-width assertions leave the marker where it is, other than a
//  lookahead of one character, which is compiled in place and leaves the
//  marker on the following position (for a negative lookahead, possibly past
//  the end of the data).  Longer lookaheads are named externals (LookAheadNamer,
//  using the same measure), which leave the marker in place.
static unsigned offsetFrom(const RE * re, unsigned in, bool indexedUTF8) {
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        if (alt->empty()) return 0;     // (matching nothing: the marker is cleared)
        unsigned altOffset = 0;
        for (const RE * a : *alt) {
            altOffset = std::max(altOffset, offsetFrom(a, in, indexedUTF8));
        }
        return altOffset;
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        unsigned offset = in;
        for (const RE * e : *seq) {
            offset = offsetFrom(e, offset, indexedUTF8);
        }
        return offset;
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        if (rep->getUB() == Rep::UNBOUNDED_REP) return 1;
        if (rep->getUB() == 0) return in;
        const unsigned body = offsetFrom(rep->getRE(), in, indexedUTF8);
        return rep->getLB() == 0 ? std::max(in, body) : body;
    } else if (isa<Start>(re)) {
        return 1;
    } else if (isa<End>(re)) {
        //  End is compiled with a lookahead, leaving the marker in place.
        return in;
    } else if (const Assertion * a = dyn_cast<Assertion>(re)) {
        const RE * asserted = a->getAsserted();
        if (a->getKind() == Assertion::Kind::LookBehind) {
            //  The lookbehind is compiled from everywhere (at the next code unit).
            return std::max(in, offsetFrom(asserted, 1, indexedUTF8));
        }
        if (isa<End>(asserted)) return in;
        //  With UTF-8 indexing, a lookahead of one UTF-8 encoded character is
        //  also compiled in place (and not named by LookAheadNamer).
        if (indexedUTF8 && isUTF8EncodedCharacter(asserted)) return 1;
        const auto range = getLengthRange(asserted, &cc::Unicode);
        if (range.second == 0) return std::max(in, offsetFrom(asserted, 1, indexedUTF8));
        if (range.second == 1) return 1;
        return in;
    } else if (const Diff * diff = dyn_cast<Diff>(re)) {
        //  The difference of an empty sequence and a zero-width assertion (e.g. not
        //  a grapheme boundary) is compiled at the next character.
        if (isEmptySeq(diff->getLH())) return 1;
        return offsetFrom(diff->getLH(), in, indexedUTF8);
    } else if (const Intersect * e = dyn_cast<Intersect>(re)) {
        if (isEmptySeq(e->getLH()) || isEmptySeq(e->getRH())) return 1;
        return std::min(offsetFrom(e->getLH(), in, indexedUTF8), offsetFrom(e->getRH(), in, indexedUTF8));
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
        return offsetFrom(g->getRE(), in, indexedUTF8);
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() ? offsetFrom(n->getDefinition(), in, indexedUTF8) : 0;
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return offsetFrom(c->getCapturedRE(), in, indexedUTF8);
    } else if (isa<Range, Reference, Permute, Interleavable>(re)) {
        return 0;
    }
    UnexpectedRE("grepOffset", re);
}

//  The offset of the final match marker: the compiler begins with the marker
//  at the next character of every position.
unsigned grepOffset(const RE * re, bool indexedUTF8) {
    return offsetFrom(re, 1, indexedUTF8);
}

CC * resolveCharClass(RE * r) {
    if (CC * cc = resolveToCC(r)) return cc;
    if (const Any * a = dyn_cast<Any>(r)) {
        return makeCC(0, UCD::UNICODE_MAX, a->getAlphabet());
    } else if (const Alt * alt = dyn_cast<Alt>(r)) {
        CC * u = nullptr;
        for (RE * e : *alt) {
            CC * cc = resolveCharClass(e);
            if ((cc == nullptr) || (u && (u->getAlphabet() != cc->getAlphabet()))) return nullptr;
            u = u ? makeCC(u, cc) : cc;
        }
        return u;
    } else if (const Diff * d = dyn_cast<Diff>(r)) {
        CC * lh = resolveCharClass(d->getLH());
        CC * rh = resolveCharClass(d->getRH());
        if (lh && rh && (lh->getAlphabet() == rh->getAlphabet())) return subtractCC(lh, rh);
    } else if (const Intersect * x = dyn_cast<Intersect>(r)) {
        CC * lh = resolveCharClass(x->getLH());
        CC * rh = resolveCharClass(x->getRH());
        if (lh && rh && (lh->getAlphabet() == rh->getAlphabet())) return intersectCC(lh, rh);
    }
    return nullptr;
}

CC * firstCharClass(RE * r) {
    if (CC * cc = resolveCharClass(r)) return cc;
    if (const Seq * seq = dyn_cast<Seq>(r)) {
        if (seq->empty() || (getLengthRange(seq->front(), &cc::Unicode).first == 0)) return nullptr;
        return firstCharClass(seq->front());
    } else if (const Rep * rep = dyn_cast<Rep>(r)) {
        if (rep->getLB() == 0) return nullptr;
        return firstCharClass(rep->getRE());
    } else if (const Alt * alt = dyn_cast<Alt>(r)) {
        CC * u = nullptr;
        for (RE * e : *alt) {
            CC * cc = firstCharClass(e);
            if ((cc == nullptr) || (u && (u->getAlphabet() != cc->getAlphabet()))) return nullptr;
            u = u ? makeCC(u, cc) : cc;
        }
        return u;
    } else if (const Name * n = dyn_cast<Name>(r)) {
        if (n->getDefinition()) return firstCharClass(n->getDefinition());
    } else if (const Capture * c = dyn_cast<Capture>(r)) {
        return firstCharClass(c->getCapturedRE());
    } else if (const Group * g = dyn_cast<Group>(r)) {
        return firstCharClass(g->getRE());
    }
    return nullptr;
}

//  The elements of r as a sequence, seeing through nested sequences and
//  single alternatives (which unsimplified REs may contain).
static void flattenSeq(RE * r, std::vector<RE *> & elems) {
    if (const Alt * alt = dyn_cast<Alt>(r)) {
        if (alt->size() == 1) {
            flattenSeq(alt->front(), elems);
            return;
        }
    } else if (const Seq * seq = dyn_cast<Seq>(r)) {
        for (RE * e : *seq) {
            flattenSeq(e, elems);
        }
        return;
    }
    elems.push_back(r);
}

bool parseStringClass(RE * r, std::vector<std::vector<CC *>> & strings) {
    std::vector<RE *> alternatives;
    std::vector<RE *> pending{r};
    while (!pending.empty()) {
        RE * a = pending.back();
        pending.pop_back();
        if (const Name * n = dyn_cast<Name>(a)) {
            if (n->getDefinition() == nullptr) return false;
            pending.push_back(n->getDefinition());
        } else if (const Alt * alt = dyn_cast<Alt>(a)) {
            for (RE * e : *alt) pending.push_back(e);
        } else {
            alternatives.push_back(a);
        }
    }
    bool multi = false;
    for (RE * a : alternatives) {
        std::vector<RE *> items;
        flattenSeq(a, items);
        std::vector<CC *> str;
        for (RE * item : items) {
            CC * const cc = resolveCharClass(item);
            if ((cc == nullptr) || cc->empty()) return false;
            str.push_back(cc);
        }
        if (str.empty()) return false;
        multi |= (str.size() > 1);
        strings.push_back(std::move(str));
    }
    return multi;
}

// Can the strings s (at offset i) and t (at offset 0) match the same text
// over the k positions starting at offset i of s and 0 of t?
static bool overlaps(const std::vector<CC *> & s, size_t i, const std::vector<CC *> & t, size_t k) {
    for (size_t j = 0; j < k; ++j) {
        if (!s[i + j]->intersects(*t[j])) return false;
    }
    return true;
}

bool isRepeatableStringClass(const std::vector<std::vector<CC *>> & strings) {
    for (const auto & s : strings) {
        for (const auto & t : strings) {
            // (A) No proper suffix of s matches a proper prefix of t.
            const size_t m = std::min(s.size(), t.size());
            for (size_t k = 1; k < m; ++k) {
                if (overlaps(s, s.size() - k, t, k)) return false;
            }
            // (B) No occurrence of t lies strictly within s.
            for (size_t i = 1; i + t.size() < s.size(); ++i) {
                if (overlaps(s, i, t, t.size())) return false;
            }
        }
    }
    return true;
}

bool parseLookaheadChain(RE * body, const cc::Alphabet * lengthAlpha, std::vector<LookaheadSegment> & segments,
                         bool requireStar) {
    segments.clear();
    std::vector<RE *> elems;
    flattenSeq(body, elems);
    // Group the elements into star segments and maximal fixed segments.
    std::vector<RE *> fixed;
    auto closeFixed = [&]() -> bool {
        if (fixed.empty()) return true;
        RE * const F = makeSeq(fixed.begin(), fixed.end());
        fixed.clear();
        const auto range = getLengthRange(F, lengthAlpha);
        if ((range.first != range.second) || (range.first < 1) || (grepOffset(F) != 0)) return false;
        segments.push_back(LookaheadSegment{false, false, nullptr, F, 0, range.first});
        return true;
    };
    bool hasStar = false;
    for (unsigned i = 0; i < elems.size(); ++i) {
        RE * const e = elems[i];
        if (const Rep * rep = dyn_cast<Rep>(e)) {
            if (rep->getUB() == Rep::UNBOUNDED_REP) {
                CC * const X = resolveCharClass(rep->getRE());
                std::vector<std::vector<CC *>> strings;
                if (X == nullptr && rep->getLB() <= 1 && parseStringClass(rep->getRE(), strings)) {
                    // A string class (see isRepeatableStringClass).
                    if (!isRepeatableStringClass(strings) || !closeFixed()) return false;
                    CC * first = makeCC();
                    CC * later = makeCC();
                    for (const auto & str : strings) {
                        first = makeCC(first, str[0]);
                        for (size_t k = 1; k < str.size(); ++k) later = makeCC(later, str[k]);
                    }
                    LookaheadSegment seg{true, false, makeCC(first, later), rep->getRE(), rep->getLB(), 0};
                    seg.strings = std::move(strings);
                    seg.first = first;
                    segments.push_back(std::move(seg));
                    hasStar = true;
                    continue;
                }
                if ((X == nullptr) || !closeFixed()) return false;
                segments.push_back(LookaheadSegment{true, false, X, rep->getRE(), rep->getLB(), 0});
                hasStar = true;
                continue;
            }
        }
        // One-character lookaheads (?=Y) or (?!Y), which must all hold: a
        // zero-width assertion segment, within the chain or at its end.
        if (isa<Assertion>(e)) {
            if ((fixed.empty() && segments.empty()) || !closeFixed()) return false;
            CC * holds = nullptr;
            bool negated = true;
            std::vector<Assertion *> assertions;
            unsigned j = i;
            for (; j < elems.size() && isa<Assertion>(elems[j]); ++j) {
                Assertion * const la = cast<Assertion>(elems[j]);
                if (la->getKind() != Assertion::Kind::LookAhead) return false;
                CC * const Y = resolveCharClass(la->getAsserted());
                if (Y == nullptr) return false;
                const bool neg = la->getSense() == Assertion::Sense::Negative;
                CC * const h = neg ? subtractCC(makeCC(0, 0x10FFFF, Y->getAlphabet()), Y) : Y;
                if (holds && (holds->getAlphabet() != h->getAlphabet())) return false;
                holds = holds ? intersectCC(holds, h) : h;
                negated &= neg;
                assertions.push_back(la);
            }
            const bool final = (j == elems.size());
            LookaheadSegment seg{false, final, holds, e, 0, 0};
            seg.assertion = true;
            seg.negated = negated;
            seg.assertions = std::move(assertions);
            segments.push_back(std::move(seg));
            if (final) break;
            i = j - 1;
            continue;
        }
        // The end of the text directly after a star or an assertion segment
        // (with no fixed segment to include it) is a segment of its own, which
        // must be the last.
        if (isa<End>(e) && fixed.empty() && !segments.empty() && (segments.back().star || segments.back().assertion)) {
            if (i != elems.size() - 1) return false;
            segments.push_back(LookaheadSegment{false, true, nullptr, e, 0, 0});
            continue;
        }
        fixed.push_back(e);
    }
    if (!closeFixed() || (requireStar && !hasStar) || segments.back().star) return false;
    // From the right, each star class is disjoint from the characters that
    // can begin the rest of the body, so that its runs are maximal.  (Runs end
    // before the end of the text anyway, so the end segment allows any star
    // class.)  A star of a class X that is not disjoint is a star of the
    // string class of the one-character strings of X, which requires no
    // disjointness (the rest may hold at any end of an occurrence): X{lb,} is
    // then X{lb-1} X+ for lb > 1.
    CC * restFirst = nullptr;
    std::vector<size_t> extended;   // the converted stars with lb > 1
    for (auto i = segments.rbegin(); i != segments.rend(); ++i) {
        if (i->end) {
            restFirst = i->cc ? i->cc : makeCC();
        } else if (i->assertion) {
            // The rest begins with a character at which the assertions hold.
            if (i->cc->getAlphabet() != restFirst->getAlphabet()) return false;
            restFirst = intersectCC(restFirst, i->cc);
        } else if (i->star) {
            if (i->cc->getAlphabet() != restFirst->getAlphabet()) return false;
            if (i->strings.empty() && !intersectCC(i->cc, restFirst)->empty()) {
                i->strings = {{i->cc}};
                i->first = i->cc;
                if (i->lb > 1) extended.push_back(segments.rend() - i - 1);
            }
            if (i->lb == 0) {
                restFirst = makeCC(i->first ? i->first : i->cc, restFirst);
            } else {
                restFirst = i->first ? i->first : i->cc;
            }
        } else {
            restFirst = firstCharClass(i->re);
            if (restFirst == nullptr) return false;
        }
    }
    // Insert the fixed X{lb-1} before each converted X{lb,} (from the right,
    // so that the recorded positions remain valid).
    for (size_t k : extended) {
        LookaheadSegment & seg = segments[k];
        RE * const fixedRE = makeRep(seg.re, seg.lb - 1, seg.lb - 1);
        const auto range = getLengthRange(fixedRE, lengthAlpha);
        if (range.first != range.second) return false;
        LookaheadSegment fixedSeg{false, false, nullptr, fixedRE, 0, range.first};
        seg.lb = 1;
        segments.insert(segments.begin() + k, fixedSeg);
    }
    return true;
}

bool hasUniquePrefix(RE * body) {
    std::vector<RE *> elems;
    flattenSeq(body, elems);
    RE * prefix, * suffix;
    std::tie(prefix, suffix) = ParseUniquePrefix(makeSeq(elems.begin(), elems.end()));
    return !isEmptySeq(prefix) && !isEmptySeq(suffix);
}

struct LookaheadChainFree : public RE_Validator {
    LookaheadChainFree() : RE_Validator("LookaheadChainFree") {}

    bool validateAssertion(const Assertion * a) override {
        if (a->getKind() == Assertion::Kind::LookAhead) {
            std::vector<LookaheadSegment> segments;
            if (!hasUniquePrefix(a->getAsserted()) &&
                    parseLookaheadChain(a->getAsserted(), &cc::Unicode, segments)) return false;
        }
        return validate(a->getAsserted());
    }
};

bool hasLookaheadChain(const RE * r) {
    return !LookaheadChainFree().validateRE(r);
}


namespace {

// The characters of a character class item (CC, Any or a combination), or
// false if r is not one.
bool classChars(const RE * r, UCD::UnicodeSet & chars) {
    if (const CC * const cc = dyn_cast<CC>(r)) {
        if (cc->getAlphabet() != &cc::Unicode) return false;
        chars = chars + *cc;
        return true;
    } else if (isa<Any>(r)) {
        chars = chars + UCD::UnicodeSet(0, UCD::UNICODE_MAX);
        return true;
    } else if (isa<Diff>(r) || isa<Intersect>(r) || isa<Name>(r) ||
               (isa<PropertyExpression>(r) && (cast<PropertyExpression>(r)->getKind() == PropertyExpression::Kind::Codepoint))) {
        // (Properties as resolved, and names of classes.)
        CC * const cc = resolveCharClass(const_cast<RE *>(r));
        if ((cc == nullptr) || (cc->getAlphabet() != &cc::Unicode)) return false;
        chars = chars + *cc;
        return true;
    }
    return false;
}

bool zeroWidth(const RE * r);

// The strings matched by r, as sequences of character classes, ignoring its
// assertions (so possibly more strings than it matches), at most limit of
// them; false if r is not bounded or has too many.
using ClassSequences = std::vector<std::vector<UCD::UnicodeSet>>;

bool classSequences(const RE * r, ClassSequences & out, size_t limit) {
    out.clear();
    UCD::UnicodeSet chars;
    if (classChars(r, chars)) {
        out.push_back({chars});
        return true;
    }
    if (zeroWidth(r)) {
        out.push_back({});
        return true;
    }
    auto product = [&](const ClassSequences & a, const ClassSequences & b, ClassSequences & ab) {
        ab.clear();
        if (a.size() * b.size() > limit) return false;
        for (const auto & x : a) {
            for (const auto & y : b) {
                ab.push_back(x);
                ab.back().insert(ab.back().end(), y.begin(), y.end());
            }
        }
        return true;
    };
    if (const Seq * seq = dyn_cast<Seq>(r)) {
        out.push_back({});
        for (const RE * e : *seq) {
            ClassSequences es, next;
            if (!classSequences(e, es, limit) || !product(out, es, next)) return false;
            out = std::move(next);
        }
        return true;
    } else if (const Alt * alt = dyn_cast<Alt>(r)) {
        for (const RE * e : *alt) {
            ClassSequences es;
            if (!classSequences(e, es, limit)) return false;
            out.insert(out.end(), es.begin(), es.end());
            if (out.size() > limit) return false;
        }
        return true;
    } else if (const Rep * rep = dyn_cast<Rep>(r)) {
        if (rep->getUB() == Rep::UNBOUNDED_REP) return false;
        ClassSequences body, k_copies{{}};
        if ((rep->getUB() > 0) && !classSequences(rep->getRE(), body, limit)) return false;
        for (int k = 0; k <= rep->getUB(); k++) {
            if (k >= rep->getLB()) {
                out.insert(out.end(), k_copies.begin(), k_copies.end());
                if (out.size() > limit) return false;
            }
            if (k < rep->getUB()) {
                ClassSequences next;
                if (!product(k_copies, body, next)) return false;
                k_copies = std::move(next);
            }
        }
        return true;
    } else if (const Capture * c = dyn_cast<Capture>(r)) {
        return classSequences(c->getCapturedRE(), out, limit);
    } else if (const Group * g = dyn_cast<Group>(r)) {
        return classSequences(g->getRE(), out, limit);
    }
    return false;
}

// For the strings of a bounded RE: can a match end at a position of another
// match other than its last (aligning the last class of a with each class of
// b before its last, and the classes of a before it with those of b)?
bool classSequencesEndWithin(const ClassSequences & strings) {
    for (const auto & a : strings) {
        if (a.empty()) continue;
        for (const auto & b : strings) {
            for (size_t j = 0; j + 1 < b.size(); j++) {
                bool overlap = true;
                for (size_t t = 0; overlap && (t < a.size()) && (t <= j); t++) {
                    overlap = a[a.size() - 1 - t].intersects(b[j - t]);
                }
                if (overlap) return true;
            }
        }
    }
    return false;
}

// Is r zero-width (an assertion, Start or End)?
bool zeroWidth(const RE * r) {
    if (isa<Assertion>(r) || isa<Start>(r) || isa<End>(r)) return true;
    if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(r)) {
        return pe->getKind() == PropertyExpression::Kind::Boundary;
    }
    return false;
}

// Can r match a nonempty string?
bool consumes(const RE * r) {
    return getLengthRange(r, &cc::Unicode).second > 0;
}

// The characters at the positions of the matches of r (the characters of its
// assertions are not matched).  False if unknown.
bool matchedChars(const RE * r, UCD::UnicodeSet & chars) {
    if (classChars(r, chars)) return true;
    if (zeroWidth(r)) return true;
    if (const Seq * seq = dyn_cast<Seq>(r)) {
        for (const RE * e : *seq) if (!matchedChars(e, chars)) return false;
        return true;
    } else if (const Alt * alt = dyn_cast<Alt>(r)) {
        for (const RE * e : *alt) if (!matchedChars(e, chars)) return false;
        return true;
    } else if (const Rep * rep = dyn_cast<Rep>(r)) {
        return (rep->getUB() == 0) || matchedChars(rep->getRE(), chars);
    } else if (const Capture * c = dyn_cast<Capture>(r)) {
        return matchedChars(c->getCapturedRE(), chars);
    } else if (const Group * g = dyn_cast<Group>(r)) {
        return matchedChars(g->getRE(), chars);
    } else if (const Name * n = dyn_cast<Name>(r)) {
        return n->getDefinition() && matchedChars(n->getDefinition(), chars);
    }
    return false;
}

// The characters that can be at the last position of a match of r, and
// whether r can match the empty string (so that what precedes it can be last).
bool lastChars(const RE * r, UCD::UnicodeSet & chars, bool & empty) {
    empty = false;
    if (classChars(r, chars)) return true;
    if (zeroWidth(r)) {
        empty = true;
        return true;
    }
    if (const Seq * seq = dyn_cast<Seq>(r)) {
        empty = true;
        for (auto i = seq->rbegin(); empty && (i != seq->rend()); ++i) {
            if (!lastChars(*i, chars, empty)) return false;
        }
        return true;
    } else if (const Alt * alt = dyn_cast<Alt>(r)) {
        for (const RE * e : *alt) {
            bool e_empty;
            if (!lastChars(e, chars, e_empty)) return false;
            empty |= e_empty;
        }
        return true;
    } else if (const Rep * rep = dyn_cast<Rep>(r)) {
        if (rep->getUB() == 0) {
            empty = true;
            return true;
        }
        if (!lastChars(rep->getRE(), chars, empty)) return false;
        empty |= (rep->getLB() == 0);
        return true;
    } else if (const Capture * c = dyn_cast<Capture>(r)) {
        return lastChars(c->getCapturedRE(), chars, empty);
    } else if (const Group * g = dyn_cast<Group>(r)) {
        return lastChars(g->getRE(), chars, empty);
    } else if (const Name * n = dyn_cast<Name>(r)) {
        return n->getDefinition() && lastChars(n->getDefinition(), chars, empty);
    }
    return false;
}

// The characters that can be at a position of a match of r followed by
// another position of the match (beforeLast), or preceded by one (after its
// first).
bool innerChars(const RE * r, UCD::UnicodeSet & chars, bool beforeLast) {
    // A single character is both the first and the last position.
    UCD::UnicodeSet single;
    if (classChars(r, single) || zeroWidth(r)) return true;
    if (const Seq * seq = dyn_cast<Seq>(r)) {
        std::vector<const RE *> items(seq->begin(), seq->end());
        if (!beforeLast) std::reverse(items.begin(), items.end());
        for (size_t i = 0; i < items.size(); ++i) {
            // An item followed (or preceded) by an item that can consume a
            // character: all its characters; otherwise, its own inner ones.
            bool later = false;
            for (size_t j = i + 1; !later && (j < items.size()); ++j) later = consumes(items[j]);
            if (later ? !matchedChars(items[i], chars) : !innerChars(items[i], chars, beforeLast)) return false;
        }
        return true;
    } else if (const Alt * alt = dyn_cast<Alt>(r)) {
        for (const RE * e : *alt) if (!innerChars(e, chars, beforeLast)) return false;
        return true;
    } else if (const Rep * rep = dyn_cast<Rep>(r)) {
        if (rep->getUB() == 0) return true;
        // (A repetition that may repeat: an earlier copy is followed by a later one.)
        if ((rep->getUB() == Rep::UNBOUNDED_REP) || (rep->getUB() > 1)) return matchedChars(rep->getRE(), chars);
        return innerChars(rep->getRE(), chars, beforeLast);
    } else if (const Capture * c = dyn_cast<Capture>(r)) {
        return innerChars(c->getCapturedRE(), chars, beforeLast);
    } else if (const Group * g = dyn_cast<Group>(r)) {
        return innerChars(g->getRE(), chars, beforeLast);
    } else if (const Name * n = dyn_cast<Name>(r)) {
        return n->getDefinition() && innerChars(n->getDefinition(), chars, beforeLast);
    }
    return false;
}

}

bool matchEndsUnique(const RE * re) {
    // A bounded RE: the precise test on its strings (with its assertions
    // ignored, a superset of its matches).
    ClassSequences strings;
    if (classSequences(re, strings, 64)) {
        return !classSequencesEndWithin(strings);
    }
    UCD::UnicodeSet beforeLast, afterFirst;
    if (!innerChars(re, beforeLast, true) || !innerChars(re, afterFirst, false)) return false;
    std::vector<const RE *> alts;
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        alts.assign(alt->begin(), alt->end());
    } else {
        alts.push_back(re);
    }
    for (const RE * a : alts) {
        const RE * last = a;
        if (const Seq * seq = dyn_cast<Seq>(a)) {
            if (seq->empty()) return false;
            last = seq->back();
        }
        if (isa<End>(last)) continue;
        if (const Assertion * as = dyn_cast<Assertion>(last)) {
            if ((as->getKind() == Assertion::Kind::LookAhead) && isa<End>(as->getAsserted()) &&
                    (as->getSense() == Assertion::Sense::Positive)) {
                continue;
            }
            UCD::UnicodeSet Y;
            if ((as->getKind() == Assertion::Kind::LookAhead) && (as->getSense() == Assertion::Sense::Negative) &&
                    classChars(as->getAsserted(), Y) && afterFirst.subset(Y)) {
                continue;
            }
        }
        UCD::UnicodeSet lasts;
        bool empty;
        if (!lastChars(a, lasts, empty) || empty || lasts.intersects(beforeLast)) return false;
    }
    return true;
}

}

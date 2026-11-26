/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/string_set.h>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <kernel/core/kernel_builder.h>
#include <re/adt/adt.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/transforms/resolve_possessive.h>
#include <llvm/Support/ErrorHandling.h>

using namespace pablo;
using namespace llvm;

namespace kernel {

static void appendSet(std::string & out, const UCD::UnicodeSet & s) {
    out += "[";
    for (const auto & range : s) {
        out += std::to_string(range.first);
        if (range.second != range.first) out += "-" + std::to_string(range.second);
        out += ",";
    }
    out += "]";
}

std::string StringSetSpec::signature() const {
    std::string s = backward ? "bwd" : "fwd";
    for (const Term & t : terms) {
        s += ";";
        for (const UCD::UnicodeSet & set : t.seq) appendSet(s, set);
        for (const auto & g : t.guards) {
            s += "!";
            for (const UCD::UnicodeSet & set : g) appendSet(s, set);
        }
    }
    return s;
}

StringSetSpec makeStringSetSpec(const UCD::UnicodeSet & chars, const std::vector<std::u32string> & strings,
                                bool backward, const std::function<UCD::UnicodeSet (const UCD::UnicodeSet &)> & code) {
    auto coded = [&](const UCD::UnicodeSet & s) {return code ? code(s) : s;};
    auto codedText = [&](const std::u32string & text) {
        std::vector<UCD::UnicodeSet> seq;
        for (char32_t c : text) seq.push_back(coded(UCD::UnicodeSet(static_cast<UCD::codepoint_t>(c))));
        return seq;
    };
    StringSetSpec spec;
    spec.backward = backward;
    std::vector<re::RE *> alts;
    if (!chars.empty()) alts.push_back(re::makeCC(UCD::UnicodeSet(chars), &cc::Unicode));
    for (const std::u32string & s : strings) {
        std::vector<re::RE *> items;
        for (char32_t c : s) items.push_back(re::makeCC(static_cast<UCD::codepoint_t>(c), &cc::Unicode));
        alts.push_back(re::makeSeq(items.begin(), items.end()));
    }
    re::UnambiguousStrings u;
    if (strings.empty() || !re::unambiguousStringClass(re::makeAlt(alts.begin(), alts.end()), re::PossessiveChoice::Longest,
                                                        backward, u, std::numeric_limits<size_t>::max())) {
        if (!strings.empty()) report_fatal_error("makeStringSetSpec: not a class of literal strings");
        spec.terms.push_back(StringSetSpec::Term{{coded(chars)}, {}});
    } else {
        if (!u.others.empty()) spec.terms.push_back(StringSetSpec::Term{{coded(u.others)}, {}});
        for (const re::UnambiguousStrings::Literal & l : u.literals) {
            StringSetSpec::Term t;
            t.seq = codedText(l.text);
            for (const std::u32string & g : l.guards) t.guards.push_back(codedText(g));
            spec.terms.push_back(std::move(t));
        }
    }
    for (const StringSetSpec::Term & t : spec.terms) spec.lengths.push_back(static_cast<unsigned>(t.seq.size()));
    std::sort(spec.lengths.begin(), spec.lengths.end());
    spec.lengths.erase(std::unique(spec.lengths.begin(), spec.lengths.end()), spec.lengths.end());
    return spec;
}

//  The furthest position ahead (forward) that a term reads.
static unsigned lookahead(const StringSetSpec & spec) {
    if (spec.backward) return 0;
    unsigned la = 0;
    for (const StringSetSpec::Term & t : spec.terms) {
        unsigned guard = 0;
        for (const auto & g : t.guards) guard = std::max<unsigned>(guard, static_cast<unsigned>(g.size()));
        la = std::max<unsigned>(la, static_cast<unsigned>(t.seq.size()) + guard - 1);
    }
    return la;
}

static std::string basisSignature(const StreamSet * basis) {
    return std::to_string(basis->getNumElements()) + "x" + std::to_string(basis->getFieldWidth());
}

StringSetKernel::StringSetKernel(LLVMTypeSystemInterface & ts, StringSetSpec spec, StreamSet * Basis, StreamSet * Lengths)
: PabloKernel(ts, std::string("StringSet_") + (spec.backward ? "bwd_" : "fwd_") +
              getStringHash(basisSignature(Basis) + ":" + spec.signature()) + "_" + basisSignature(Basis),
              {}, {Binding{"lengths", Lengths}})
, mSpec(std::move(spec))
, mSignature(basisSignature(Basis) + ":" + mSpec.signature()) {
    if (LLVM_UNLIKELY(Lengths->getNumElements() != mSpec.lengths.size())) {
        report_fatal_error("StringSetKernel: one output stream per length is required");
    }
    const unsigned la = lookahead(mSpec);
    if (la > 0) {
        mInputStreamSets.push_back(Binding{"basis", Basis, FixedRate(), LookAhead(la)});
    } else {
        mInputStreamSets.push_back(Binding{"basis", Basis});
    }
}

void StringSetKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const std::vector<PabloAST *> basis = getInputStreamSet("basis");
    PabloAST * const zeroes = pb.createZeroes();
    //  A CC compiler for the basis looked ahead by each offset (the sets are
    //  compiled as sets of codepoints of the Unicode alphabet over the basis,
    //  padded to 21 bits).
    std::map<unsigned, std::unique_ptr<cc::Parabix_CC_Compiler_Builder>> compilers;
    std::map<std::pair<unsigned, std::string>, PabloAST *> memo;
    auto classAt = [&](const UCD::UnicodeSet & set, unsigned offset) -> PabloAST * {
        if (set.empty()) return zeroes;
        std::string key;
        appendSet(key, set);
        auto f = memo.find({offset, key});
        if (f != memo.end()) return f->second;
        auto & c = compilers[offset];
        if (!c) {
            std::vector<PabloAST *> bits;
            for (PabloAST * b : basis) bits.push_back(offset == 0 ? b : pb.createLookahead(b, offset));
            bits.resize(std::max<size_t>(bits.size(), 21), zeroes);
            c = std::make_unique<cc::Parabix_CC_Compiler_Builder>(bits);
        }
        PabloAST * const r = c->compileCC(re::makeCC(UCD::UnicodeSet(set), &cc::Unicode), pb);
        memo.emplace(std::make_pair(offset, key), r);
        return r;
    };
    //  The match of a sequence of sets: forward, beginning at the position,
    //  starting at offset; backward, ending at the position, with back further
    //  positions after its end.
    auto sequence = [&](const std::vector<UCD::UnicodeSet> & seq, unsigned offset) -> PabloAST * {
        PabloAST * m = nullptr;
        const unsigned n = static_cast<unsigned>(seq.size());
        for (unsigned i = 0; i < n; i++) {
            PabloAST * item;
            if (mSpec.backward) {
                item = classAt(seq[i], 0);
                const unsigned shift = offset + (n - 1 - i);
                if (shift > 0) item = pb.createAdvance(item, shift);
            } else {
                item = classAt(seq[i], offset + i);
            }
            m = m ? pb.createAnd(m, item) : item;
        }
        return m ? m : zeroes;
    };
    std::vector<PabloAST *> out(mSpec.lengths.size(), zeroes);
    for (const StringSetSpec::Term & t : mSpec.terms) {
        const unsigned len = static_cast<unsigned>(t.seq.size());
        PabloAST * m = sequence(t.seq, 0);
        for (const auto & g : t.guards) {
            //  Forward, the guard follows the term; backward, it precedes it.
            m = pb.createAnd(m, pb.createNot(sequence(g, len)));
        }
        const size_t i = std::lower_bound(mSpec.lengths.begin(), mSpec.lengths.end(), len) - mSpec.lengths.begin();
        out[i] = pb.createOr(out[i], m);
    }
    Var * const lengths = getOutputStreamVar("lengths");
    for (size_t i = 0; i < out.size(); i++) {
        pb.createAssign(pb.createExtract(lengths, pb.getInteger(i)), pb.createInFile(out[i]));
    }
}

}

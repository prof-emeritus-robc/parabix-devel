/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  The NFC kernels, built in C++ from NFC_Tables (doc/masked-nfc-plan.md,
 *  step 2), with the Pablo structure of the code that scripts/UCD/nfc_generator.py
 *  emits (its kernels are no longer built): the kernel of each generator stage
 *  is a class here, and each template's logic is a function.
 */

#include <kernel/unicode/normalization/normalization.h>
#include <kernel/unicode/normalization/nfc_tables.h>

#include <array>
#include <set>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/stream_shift.h>
#include <ucd/core/unicode_set.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>
#include <ucd/utf/utf_compiler.h>
#include <toolchain/toolchain.h>
#include <re/adt/re_cc.h>
#include <re/cc/cc_compiler_target.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/sorting.h>
#include <kernel/bitwise/bixlogic.h>
#include <kernel/bitwise/bixnum_kernel.h>

using namespace kernel;
using namespace pablo;
using namespace llvm;
using UCD::UnicodeSet;
using codepoint_t = UCD::codepoint_t;

namespace {

//  The kernel name suffix of an input mode.
std::string modeSuffix(NormalizationInput mode) {
    switch (mode) {
        case NormalizationInput::Any: return "";
        case NormalizationInput::InputIsNFD: return "+inNFD";
        case NormalizationInput::InputIsNFKD: return "+inNFKD";
    }
    return "";
}

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)

const NFC_Tables & tables() {return NFC_Tables::get();}

//  The suffix of the names of the kernels: the hash of the tables they are built
//  from (to become the Unicode version once the port is stable).
const std::string & nameSuffix() {
    static const std::string suffix = [] {
        std::string dump;
        raw_string_ostream out(dump);
        tables().dump(out);
        out.flush();
        return "_cxx" + Kernel::getStringHash(dump).substr(0, 16);
    }();
    return suffix;
}

//  UTF-8 helpers, as in the generator.
unsigned u8_encoded_length(codepoint_t cp) {
    if (cp <= 0x7F) return 1;
    if (cp <= 0x7FF) return 2;
    if (cp <= 0xFFFF) return 3;
    return 4;
}

unsigned u8_code_unit(codepoint_t cp, unsigned n) {
    const unsigned len = u8_encoded_length(cp);
    if (n == 1) {
        switch (len) {
            case 1: return cp;
            case 2: return 0xC0 | (cp >> 6);
            case 3: return 0xE0 | (cp >> 12);
            default: return 0xF0 | (cp >> 18);
        }
    }
    return 0x80 | ((cp >> (6 * (len - n))) & 0x3F);
}

std::vector<unsigned> u8_bytes(const std::u32string & s) {
    std::vector<unsigned> bytes;
    for (char32_t c : s) {
        for (unsigned i = 1; i <= u8_encoded_length(c); i++) bytes.push_back(u8_code_unit(c, i));
    }
    return bytes;
}

//  The XOR of the bytes of cp with the target bytes, each padded with zeroes to the longer.
std::vector<unsigned> u8_byte_xfrms(codepoint_t cp, std::vector<unsigned> target) {
    const unsigned len1 = u8_encoded_length(cp);
    if (len1 > target.size()) target.resize(len1, 0);
    std::vector<unsigned> x;
    for (unsigned i = 0; i < target.size(); i++) x.push_back((i < len1 ? u8_code_unit(cp, i + 1) : 0) ^ target[i]);
    return x;
}

std::vector<unsigned> u8_byte_xfrms(codepoint_t cp, const std::u32string & s) {
    return u8_byte_xfrms(cp, u8_bytes(s));
}

unsigned pfxLength(unsigned pfx) {
    if (pfx == 0) return 1;
    if (pfx <= 0xDF) return 2;
    if (pfx <= 0xEF) return 3;
    return 4;
}

//  The code points of a set of them by prefix code (see NFC_Tables::prefixCode).
template <typename Iterable>
std::map<unsigned, std::vector<codepoint_t>> byPrefix(const Iterable & cps) {
    std::map<unsigned, std::vector<codepoint_t>> m;
    for (codepoint_t cp : cps) m[NFC_Tables::prefixCode(cp)].push_back(cp);
    for (auto & e : m) std::sort(e.second.begin(), e.second.end());
    return m;
}

PabloAST * pfxTest(BixNumCompiler & bnc, PabloBuilder & pb, const std::vector<PabloAST *> & Basis, unsigned pfx) {
    if (pfx == 0) return pb.createNot(Basis[7]);
    if (pfx <= 0xDF) return pb.createAnd(bnc.UGE(Basis, pfx), bnc.ULE(Basis, pfx | 3));
    return bnc.EQ(Basis, pfx);
}

//  Classes compiled together in a scope (as Uset_Builder's scope compilations):
//  the sets are added, and compiled by UTF_Compiler, each to a Var.
class ScopeClasses {
public:
    unsigned add(const UnicodeSet & s) {
        for (unsigned i = 0; i < mSets.size(); i++) if (mSets[i] == s) return i;
        mSets.push_back(s);
        return mSets.size() - 1;
    }
    std::vector<Var *> compile(PabloBuilder & b, Var * basis, BitMovementMode mode = BitMovementMode::LookAhead,
                               unsigned extraLookahead = 0) {
        std::vector<Var *> vars;
        if (mSets.empty()) return vars;
        UTF::UTF_Compiler compiler(basis, b, mode);
        if (extraLookahead) compiler.setExtraLookahead(extraLookahead);
        for (unsigned i = 0; i < mSets.size(); i++) vars.push_back(b.createVar("nfc_class" + std::to_string(i), b.createZeroes()));
        compiler.compile(vars, mSets);
        return vars;
    }
private:
    std::vector<UnicodeSet> mSets;
};

//  The bit transforms of a map of characters to strings (u8_bit_transform_sets):
//  the sets, added to a scope's classes, and the specifications for UpdateBitXfrms.
struct BitXfrms {
    std::vector<unsigned> classIndex;       // of each set, in the scope's classes
    std::vector<BitXfrmSpec> specs;         // BitXfrmIndex: in classIndex
    void add(ScopeClasses & classes, const std::map<codepoint_t, std::u32string> & map) {
        std::map<codepoint_t, std::vector<unsigned>> targets;
        for (const auto & e : map) targets[e.first] = u8_bytes(e.second);
        add(classes, targets);
    }
    //  (With the bytes written at each character's position given.)
    void add(ScopeClasses & classes, const std::map<codepoint_t, std::vector<unsigned>> & targets) {
        std::map<std::pair<unsigned, unsigned>, UnicodeSet> sets;   // (position, bit) -> characters
        for (const auto & e : targets) {
            const std::vector<unsigned> x = u8_byte_xfrms(e.first, e.second);
            for (unsigned i = 0; i < x.size(); i++) {
                for (unsigned j = 0; j < 8; j++) {
                    if ((x[i] >> j) & 1) sets[{i, j}].insert(e.first);
                }
            }
        }
        for (const auto & s : sets) {
            const unsigned c = classes.add(s.second);
            unsigned k = 0;
            while (k < classIndex.size() && classIndex[k] != c) k++;
            if (k == classIndex.size()) classIndex.push_back(c);
            specs.push_back(BitXfrmSpec{k, s.first.first, s.first.second});
        }
    }
    void update(PabloBuilder & b, std::vector<Var *> & xfrm, PabloAST * marker, const std::vector<PabloAST *> & cls) {
        if (specs.empty()) return;
        std::vector<PabloAST *> sets;
        for (unsigned c : classIndex) sets.push_back(cls[c]);
        UpdateBitXfrms(b, xfrm, marker, sets, specs);
    }
};

//  The deletion sets of a map (u8_deletion_sets): by the number of bytes deleted.
std::map<unsigned, UnicodeSet> deletionSets(const std::map<codepoint_t, std::u32string> & map) {
    std::map<unsigned, UnicodeSet> sets;
    for (const auto & e : map) {
        const unsigned len1 = u8_encoded_length(e.first), len2 = u8_bytes(e.second).size();
        if (len1 > len2) sets[len1 - len2].insert(e.first);
    }
    return sets;
}

//  The translation of the bytes of cp1 at marker to those of cp2 (CharacterTranslationLogic).
void characterTranslation(PabloBuilder & b, codepoint_t cp1, codepoint_t cp2, PabloAST * marker, std::vector<PabloAST *> & xfrm) {
    const std::vector<unsigned> x = u8_byte_xfrms(cp1, std::u32string(1, static_cast<char32_t>(cp2)));
    for (unsigned i = 0; i < u8_encoded_length(cp2); i++) {
        if (x[i] == 0) continue;
        PabloAST * const adv = i > 0 ? b.createAdvance(marker, i) : marker;
        for (unsigned j = 0; j < 8; j++) {
            if ((x[i] >> j) & 1) xfrm[j] = b.createOr(xfrm[j], adv);
        }
    }
}

//  The classes of a scope, within the mask if any (at an offset ahead: the mask
//  of the character the classes are of).
std::vector<PabloAST *> withinMask(PabloBuilder & b, const std::vector<Var *> & vars, PabloAST * mask) {
    std::vector<PabloAST *> cls(vars.begin(), vars.end());
    if (mask) for (PabloAST *& c : cls) c = b.createAnd(c, mask);
    return cls;
}

//  A kernel's mask input (see NFC_MaskedTransform): the bytes of the characters
//  within the mask, looked ahead as far as its basis.
struct MaskInput {
    static void bind(Bindings & inputs, StreamSet * Mask, unsigned lookahead) {
        if (Mask == nullptr) return;
        if (lookahead) inputs.emplace_back("Mask", Mask, FixedRate(), LookAhead(lookahead)); else inputs.emplace_back("Mask", Mask);
    }
    static std::string suffix(StreamSet * Mask) {return Mask ? "+mask" : "";}
};

PabloAST * maskAt(PabloBuilder & b, PabloAST * mask, unsigned offset) {
    if (mask == nullptr) return nullptr;
    return offset ? b.createLookahead(mask, offset) : mask;
}

std::vector<Var *> xfrmVars(PabloBuilder & pb, unsigned n) {
    std::vector<Var *> v(n);
    for (unsigned i = 0; i < n; i++) v[i] = pb.createVar("XfrmBasis" + std::to_string(i), pb.createZeroes());
    return v;
}

//  The UTF-8 bytes of the characters marked at their first bytes, extended over
//  their sequences (as the generators' final code does for deletions).
PabloAST * u8Span(BixNumCompiler & bnc, PabloBuilder & pb, const std::vector<PabloAST *> & Basis, PabloAST * first) {
    PabloAST * span = pb.createOr(first, pb.createAdvance(pb.createAnd(bnc.UGE(Basis, 0xC2), first), 1));
    span = pb.createOr(span, pb.createAdvance(pb.createAnd(bnc.UGE(Basis, 0xE0), first), 2));
    return pb.createOr(span, pb.createAdvance(pb.createAnd(bnc.UGE(Basis, 0xF0), first), 3));
}

//  The insertion amounts of the NFC working space (NFC_Initial_Insertion).
class NFC_InsertionKernel final : public PabloKernel {
public:
    NFC_InsertionKernel(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * InsertionBixNum, StreamSet * WorkMask,
                        NormalizationInput mode)
    : PabloKernel(ts, "NFC_Insertion" + Basis->shapeString() + (WorkMask ? "+wkmsk" : "") + modeSuffix(mode) + nameSuffix(),
                  {Binding{"Basis", Basis, FixedRate(), LookAhead(3)}}, {Binding{"InsertionBixNum", InsertionBixNum}}),
      mHasWorkMask(WorkMask != nullptr), mBits(InsertionBixNum->getNumElements()), mMode(mode) {
        addAttribute(kernel::LocalCarries());
        if (WorkMask) mInputStreamSets.push_back(Binding{"WorkMask", WorkMask});
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<UnicodeSet> bitSets;
        //  (Only for the characters that may occur in the input.)
        const UnicodeSet possible = NormalizationInputSet(mMode);
        for (const auto & e : tables().max_insert_map) {
            if (!possible.contains(e.first)) continue;
            for (unsigned i = 0; (1u << i) <= e.second; i++) {
                if (e.second & (1u << i)) {
                    if (bitSets.size() <= i) bitSets.resize(i + 1);
                    bitSets[i].insert(e.first);
                }
            }
        }
        if (bitSets.size() > mBits) report_fatal_error("NFC_InsertionKernel: too few insertion bits");
        ScopeClasses classes;
        for (const UnicodeSet & s : bitSets) classes.add(s);
        const std::vector<Var *> vars = classes.compile(pb, getInputStreamVar("Basis"), BitMovementMode::Advance);
        std::vector<PabloAST *> insertions(mBits, pb.createZeroes());
        for (unsigned i = 0; i < bitSets.size(); i++) insertions[i] = vars[classes.add(bitSets[i])];
        if (mHasWorkMask) {
            PabloAST * const work = getInputStreamSet("WorkMask")[0];
            for (PabloAST *& x : insertions) x = pb.createAnd(x, work);
        }
        writeOutputStreamSet("InsertionBixNum", insertions);
    }
private:
    const bool mHasWorkMask;
    const unsigned mBits;
    const NormalizationInput mMode;
};

//  A character-to-string stage (the singletons, the excluded composites): each
//  character of the map replaced by its string, in place, with the excess bytes
//  deleted (singleton_header / excluded_composite_header and their final code).
class CharacterStage final : public PabloKernel {
public:
    CharacterStage(LLVMTypeSystemInterface & ts, std::string name, std::map<codepoint_t, std::u32string> map,
                   StreamSet * Basis, StreamSet * XfrmBasis, StreamSet * Mask = nullptr)
    : PabloKernel(ts, name + Basis->shapeString() + MaskInput::suffix(Mask) + nameSuffix(),
                  [&] {
                      Bindings b{Binding{"Basis", Basis, FixedRate(), LookAhead(3)}};
                      MaskInput::bind(b, Mask, 0);
                      return b;
                  }(),
                  {Binding{"XfrmBasis", XfrmBasis}}),
      mMap(std::move(map)), mHasMask(Mask != nullptr) {
        addAttribute(kernel::LocalCarries());
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> Basis = getInputStreamSet("Basis");
        Var * const basisVar = getInputStreamVar("Basis");
        const bool needsDelete = !deletionSets(mMap).empty();
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        Var * const DeleteVar = needsDelete ? pb.createVar("DeleteVar", zeroes) : nullptr;
        std::vector<Var *> XfrmVar = xfrmVars(pb, Basis.size());
        std::vector<codepoint_t> keys;
        for (const auto & e : mMap) keys.push_back(e.first);
        for (const auto & r : byPrefix(keys)) {
            const unsigned pfx = r.first;
            std::map<codepoint_t, std::u32string> sub;
            for (codepoint_t cp : r.second) sub[cp] = mMap.at(cp);
            auto b = pb.createScope();
            pb.createIf(pfxTest(bnc, pb, Basis, pfx), b);
            ScopeClasses classes;
            BitXfrms bits;
            bits.add(classes, sub);
            const std::map<unsigned, UnicodeSet> del = deletionSets(sub);
            std::map<unsigned, unsigned> delClass;
            for (const auto & d : del) delClass[d.first] = classes.add(d.second);
            const std::vector<PabloAST *> cls = withinMask(b, classes.compile(b, basisVar), mask);
            bits.update(b, XfrmVar, nullptr, cls);
            if (needsDelete && !del.empty()) {
                const unsigned cpLen = pfxLength(pfx);
                PabloAST * d = zeroes;
                for (const auto & dc : delClass) {
                    for (unsigned offset = cpLen - dc.first; offset < cpLen; offset++) {
                        PabloAST * const v = cls[dc.second];
                        d = b.createOr(d, offset == 0 ? v : b.createAdvance(v, offset));
                    }
                }
                b.createAssign(DeleteVar, b.createOr(DeleteVar, d));
            }
        }
        PabloAST * const select = needsDelete ? pb.createNot(DeleteVar) : nullptr;
        Var * const out = getOutputStreamVar("XfrmBasis");
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * v = pb.createXor(Basis[i], XfrmVar[i]);
            if (select) v = pb.createAnd(select, v);
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), v);
        }
    }
private:
    const std::map<codepoint_t, std::u32string> mMap;
    const bool mHasMask;
};

std::map<codepoint_t, std::u32string> singletonStrings() {
    std::map<codepoint_t, std::u32string> m;
    for (const auto & s : tables().singleton_map) m[s.first] = std::u32string(1, static_cast<char32_t>(s.second));
    return m;
}

//  The self-composables (generate_self_composable_stage).
class SelfComposableStage final : public PabloKernel {
public:
    SelfComposableStage(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * XfrmedBasis, StreamSet * Mask = nullptr)
    : PabloKernel(ts, "NFC_SelfComposable" + Basis->shapeString() + MaskInput::suffix(Mask) + nameSuffix(),
                  [&] {
                      Bindings b{Binding{"Basis", Basis, FixedRate(), LookAhead(7)}};
                      MaskInput::bind(b, Mask, 7);
                      return b;
                  }(),
                  {Binding{"XfrmedBasis", XfrmedBasis}}), mHasMask(Mask != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> Basis = getInputStreamSet("Basis");
        Var * const basisVar = getInputStreamVar("Basis");
        Var * const DelVar = pb.createVar("DelVar", zeroes);
        std::vector<Var *> XfrmVar = xfrmVars(pb, Basis.size());
        const auto & selfc = tables().self_composables;
        ScopeClasses top;
        for (const auto & s : selfc) {
            top.add(UnicodeSet(s.first));
            top.add(UnicodeSet(s.second));
        }
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        const std::vector<PabloAST *> topVars = withinMask(pb, top.compile(pb, basisVar), mask);
        for (const auto & s : selfc) {
            const codepoint_t A = s.first, AA = s.second;
            PabloAST * const A_v = topVars[top.add(UnicodeSet(A))];
            PabloAST * const AA_v = topVars[top.add(UnicodeSet(AA))];
            auto b = pb.createScope();
            pb.createIf(pb.createOr(A_v, AA_v), b);
            //  A and AA ahead by their own lengths, compiled by length.
            std::map<unsigned, std::vector<codepoint_t>> byLength;
            for (codepoint_t cp : {A, AA}) byLength[u8_encoded_length(cp)].push_back(cp);
            std::map<codepoint_t, PabloAST *> ahead;
            for (const auto & l : byLength) {
                ScopeClasses la;
                for (codepoint_t cp : l.second) la.add(UnicodeSet(cp));
                const std::vector<PabloAST *> v = withinMask(b, la.compile(b, basisVar, BitMovementMode::LookAhead, l.first),
                                                             maskAt(b, mask, l.first));
                for (codepoint_t cp : l.second) ahead[cp] = v[la.add(UnicodeSet(cp))];
            }
            const SCResults r = SelfComposableLogic(b, Basis, u8_encoded_length(A), u8_encoded_length(AA), A_v, AA_v, ahead[A], ahead[AA]);
            b.createAssign(DelVar, b.createOr(DelVar, r.A_or_AA_to_delete));
            PabloAST * const xfrm = b.createOr(r.A_to_convert_to_AA, r.AA_to_convert_to_A);
            ScopeClasses classes;
            BitXfrms bits;
            bits.add(classes, {{A, std::u32string(1, static_cast<char32_t>(AA))}, {AA, std::u32string(1, static_cast<char32_t>(A))}});
            const std::vector<Var *> vars = classes.compile(b, basisVar);
            bits.update(b, XfrmVar, xfrm, withinMask(b, vars, nullptr));
        }
        PabloAST * const select = pb.createNot(DelVar);
        Var * const out = getOutputStreamVar("XfrmedBasis");
        for (unsigned i = 0; i < 8; i++) {
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), pb.createAnd(select, pb.createXor(XfrmVar[i], Basis[i])));
        }
    }
private:
    const bool mHasMask;
};

//  A pass of the short composables (generate_short_composable_stage): pairs of
//  adjacent starters composed, the second replaced by the composite (or both by
//  a recomposition) and the first deleted.
class ShortComposableStage final : public PabloKernel {
public:
    ShortComposableStage(LLVMTypeSystemInterface & ts, unsigned pass, StreamSet * Basis, StreamSet * OutputBasis, StreamSet * Mask = nullptr)
    : PabloKernel(ts, "NFC_ShortComposable" + std::to_string(pass) + "_" + Basis->shapeString() + MaskInput::suffix(Mask) + nameSuffix(),
                  [&] {
                      Bindings b{Binding{"Basis", Basis, FixedRate(), LookAhead(7)}};
                      MaskInput::bind(b, Mask, 7);
                      return b;
                  }(),
                  {Binding{"OutputBasis", OutputBasis}}),
      mPass(pass), mHasMask(Mask != nullptr) {
        addAttribute(kernel::LocalCarries());
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> Basis = getInputStreamSet("Basis");
        Var * const basisVar = getInputStreamVar("Basis");
        Var * const DeleteVar = pb.createVar("DeleteVar", zeroes);
        std::vector<Var *> XfrmVar = xfrmVars(pb, Basis.size());
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        const auto & rules = tables().short_pass_map.at(mPass);
        std::vector<codepoint_t> keys;
        for (const auto & r : rules) keys.push_back(r.first);
        for (const auto & r : byPrefix(keys)) {
            const unsigned pfx = r.first;
            const std::vector<codepoint_t> & cp1_list = r.second;
            auto b = pb.createScope();
            pb.createIf(pfxTest(bnc, pb, Basis, pfx), b);
            std::vector<PabloAST *> xfrm(8, zeroes);
            PabloAST * del = zeroes;
            //  The seconds, looking ahead past a first character of the prefix's length.
            ScopeClasses secondClasses;
            for (codepoint_t cp1 : cp1_list) {
                for (const auto & y : rules.at(cp1)) secondClasses.add(UnicodeSet(y.first));
            }
            const std::vector<Var *> secondVars = secondClasses.compile(b, basisVar, BitMovementMode::LookAhead, pfxLength(pfx));
            ScopeClasses firstClasses;
            for (codepoint_t cp1 : cp1_list) firstClasses.add(UnicodeSet(cp1));
            const std::vector<Var *> firstVars = firstClasses.compile(b, basisVar);
            //  (Within the mask: the first character, and the second, ahead.)
            std::map<codepoint_t, Var *> generated;
            for (codepoint_t cp1 : cp1_list) {
                Var * const v = firstVars[firstClasses.add(UnicodeSet(cp1))];
                generated[cp1] = mask ? b.createVar("first", b.createAnd(v, mask)) : v;
            }
            PabloAST * const secondMask = maskAt(b, mask, pfxLength(pfx));
            auto second = [&](codepoint_t cp2) -> PabloAST * {
                PabloAST * const v = secondVars[secondClasses.add(UnicodeSet(cp2))];
                return secondMask ? b.createAnd(v, secondMask) : v;
            };
            std::map<codepoint_t, PabloAST *> precomposedGenerated;
            auto emitCases = [&](codepoint_t cp1) {
                for (const auto & y : rules.at(cp1)) {
                    const codepoint_t cp2 = y.first;
                    if (cp1 == cp2) continue;
                    const std::u32string & composed = y.second;
                    PabloAST * const before = b.createAnd(generated[cp1], second(cp2));
                    PabloAST * const found = b.createAdvance(before, u8_encoded_length(cp1));
                    if (composed.size() == 1) {
                        del = b.createOr(del, before);
                        characterTranslation(b, cp2, composed[0], found, xfrm);
                        precomposedGenerated[composed[0]] = found;
                    } else {
                        characterTranslation(b, cp1, composed[0], before, xfrm);
                        characterTranslation(b, cp2, composed[1], found, xfrm);
                    }
                }
            };
            //  A composite that is itself a first character of the pass (in this
            //  prefix) composes again: its cases follow those generating it.
            std::vector<codepoint_t> deferred;
            for (codepoint_t cp1 : cp1_list) {
                for (const auto & y : rules.at(cp1)) {
                    if (y.first == cp1 || y.second.size() != 1) continue;
                    const codepoint_t p = y.second[0];
                    if (std::binary_search(cp1_list.begin(), cp1_list.end(), p)
                            && std::find(deferred.begin(), deferred.end(), p) == deferred.end()) deferred.push_back(p);
                }
            }
            for (codepoint_t cp1 : cp1_list) {
                if (std::find(deferred.begin(), deferred.end(), cp1) == deferred.end()) emitCases(cp1);
            }
            while (!deferred.empty()) {
                std::vector<codepoint_t> next;
                for (codepoint_t cp1 : deferred) {
                    const auto g = precomposedGenerated.find(cp1);
                    if (g != precomposedGenerated.end()) {
                        b.createAssign(generated[cp1], b.createOr(generated[cp1], g->second));
                        emitCases(cp1);
                    } else {
                        next.push_back(cp1);
                    }
                }
                if (next.size() == deferred.size()) report_fatal_error("ShortComposableStage: deferred composites never generated");
                deferred = next;
            }
            for (unsigned i = 0; i < 8; i++) b.createAssign(XfrmVar[i], b.createOr(XfrmVar[i], xfrm[i]));
            b.createAssign(DeleteVar, b.createOr(DeleteVar, del));
        }
        PabloAST * const select = pb.createNot(u8Span(bnc, pb, Basis, DeleteVar));
        Var * const out = getOutputStreamVar("OutputBasis");
        for (unsigned i = 0; i < 8; i++) {
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), pb.createAnd(select, pb.createXor(XfrmVar[i], Basis[i])));
        }
    }
private:
    const unsigned mPass;
    const bool mHasMask;
};

//  The marks of a long-composition pass by prefix code of their starters:
//  prefix -> mark -> starters.
std::map<unsigned, std::map<codepoint_t, std::vector<codepoint_t>>> passData(unsigned pass) {
    const NFC_Tables & T = tables();
    auto * cccObj = cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    std::map<unsigned, std::map<codepoint_t, std::vector<codepoint_t>>> data;
    for (const auto & m : T.long_composable_map) {
        const unsigned ccc = UCD::CCC_ns::enum_val[cccObj->GetEnumerationValue(m.first)];
        if (T.ccc_pass_allocation.at(ccc) != pass) continue;
        std::vector<codepoint_t> starters;
        for (const auto & s : m.second) starters.push_back(s.first);
        for (const auto & r : byPrefix(starters)) data[r.first][m.first] = r.second;
    }
    return data;
}

unsigned markCode(unsigned pass, unsigned pfx, codepoint_t mark) {
    const auto & codes = tables().conditional_mark_codes;
    const auto p = codes.find(pass);
    if (p == codes.end()) return 1;
    const auto f = p->second.find(pfx);
    if (f == p->second.end()) return 1;
    return f->second.at(mark);
}

//  The marks of a long-composition pass that compose with the starter they
//  follow, unblocked, coded at the marks (generate_nfc_stage).
class FindComposablesStage final : public PabloKernel {
public:
    //  With a mask, ccc_NR also marks the positions outside it, where a scan stops.
    FindComposablesStage(LLVMTypeSystemInterface & ts, unsigned pass, StreamSet * Basis, StreamSet * ccc_NR,
                         StreamSet * MarkCode, StreamSet * Index, StreamSet * Mask = nullptr)
    : PabloKernel(ts, "NFC_FindComposables" + std::to_string(pass) + "_" + Basis->shapeString() + MaskInput::suffix(Mask) + nameSuffix(),
                  [&] {
                      Bindings b{Binding{"Basis", Basis, FixedRate(), LookAhead(3)}, Binding{"ccc_NR", ccc_NR}};
                      MaskInput::bind(b, Mask, 0);
                      return b;
                  }(),
                  {Binding{"MarkCode", MarkCode}, Binding{"Index_ccc_NR_or_MarksFound", Index}}), mPass(pass), mHasMask(Mask != nullptr) {}
protected:
    void generatePabloMethod() override {
        auto * cccObj = cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> Basis = getInputStreamSet("Basis");
        Var * const basisVar = getInputStreamVar("Basis");
        PabloAST * const ccc_NR = getInputStreamSet("ccc_NR")[0];
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        const unsigned markCodeBits = tables().max_conditional_code_bits.at(mPass);
        std::vector<Var *> markCodeVar(markCodeBits);
        for (unsigned i = 0; i < markCodeBits; i++) markCodeVar[i] = pb.createVar("markCode" + std::to_string(i), zeroes);
        for (const auto & d : passData(mPass)) {
            const unsigned pfx = d.first;
            auto b = pb.createScope();
            pb.createIf(pfxTest(bnc, pb, Basis, pfx), b);
            ScopeClasses classes;
            std::map<codepoint_t, std::array<unsigned, 3>> idx;      // starters, mark, ccc of the mark
            for (const auto & m : d.second) {
                UnicodeSet starters;
                for (codepoint_t s : m.second) starters.insert(s);
                const unsigned cccEnum = cccObj->GetEnumerationValue(m.first);
                idx[m.first] = {classes.add(starters), classes.add(UnicodeSet(m.first)), classes.add(cccObj->GetCodepointSet(cccEnum))};
            }
            //  (The starters and the marks within the mask.)
            const std::vector<Var *> cv = classes.compile(b, basisVar);
            std::vector<PabloAST *> vars(cv.begin(), cv.end());
            if (mask) {
                for (const auto & m : d.second) {
                    for (unsigned k : {0, 1}) vars[idx[m.first][k]] = b.createAnd(cv[idx[m.first][k]], mask);
                }
            }
            std::map<unsigned, PabloAST *> cccOrNR;
            for (const auto & m : d.second) {
                const unsigned c = idx[m.first][2];
                if (!cccOrNR.count(c)) cccOrNR[c] = b.createOr(cv[c], ccc_NR);
            }
            for (const auto & m : d.second) {
                const auto & ix = idx[m.first];
                PabloAST * const possible = b.createAdvanceThenScanTo(vars[ix[0]], cccOrNR[ix[2]]);
                PabloAST * const found = b.createAnd(possible, vars[ix[1]]);
                const unsigned code = markCode(mPass, pfx, m.first);
                for (unsigned i = 0; i < markCodeBits; i++) {
                    if ((code >> i) & 1) b.createAssign(markCodeVar[i], b.createOr(markCodeVar[i], found));
                }
            }
        }
        PabloAST * composable2nd = markCodeVar[0];
        for (unsigned i = 1; i < markCodeBits; i++) composable2nd = pb.createOr(composable2nd, markCodeVar[i]);
        Var * const markOut = getOutputStreamVar("MarkCode");
        for (unsigned i = 0; i < markCodeBits; i++) pb.createAssign(pb.createExtract(markOut, pb.getInteger(i)), markCodeVar[i]);
        Var * const indexOut = getOutputStreamVar("Index_ccc_NR_or_MarksFound");
        pb.createAssign(pb.createExtract(indexOut, pb.getInteger(0)), pb.createOr(ccc_NR, composable2nd));
    }
private:
    const unsigned mPass;
    const bool mHasMask;
};

//  The bytes a long composition writes at its starter P: the composite; or, for
//  an overridable primary (a starter and the marks that remain), the starter,
//  then zeroes, then the marks, right-aligned in the region of P (its bytes and
//  the bytes inserted after it), so that the starter may grow into the zeroes
//  by the compositions of the passes that follow (see NFC_Tables::max_insert_map).
std::vector<unsigned> compositionTarget(codepoint_t P, const std::u32string & result) {
    std::vector<unsigned> bytes = u8_bytes(result.substr(0, 1));
    if (result.size() == 1) return bytes;
    const auto f = tables().max_insert_map.find(P);
    const unsigned region = u8_encoded_length(P) + (f == tables().max_insert_map.end() ? 0 : f->second);
    const std::vector<unsigned> marks = u8_bytes(result.substr(1));
    if (bytes.size() + marks.size() > region) report_fatal_error("NFC: no room for an overridable primary");
    bytes.resize(region - marks.size(), 0);
    bytes.insert(bytes.end(), marks.begin(), marks.end());
    return bytes;
}

//  The compositions of a long-composition pass at the starters given their
//  marks' codes, and the marks deleted (generate_long_composition_xfrm_stage).
class ApplyLongCompositionStage final : public PabloKernel {
public:
    ApplyLongCompositionStage(LLVMTypeSystemInterface & ts, unsigned pass, StreamSet * Basis, StreamSet * MarkCodeAtStarter,
                              StreamSet * MarkCode, StreamSet * OutputBasis)
    : PabloKernel(ts, "NFC_ApplyLongComposition" + std::to_string(pass) + "_" + Basis->shapeString() + nameSuffix(),
                  {Binding{"Basis", Basis, FixedRate(), LookAhead(3)}, Binding{"MarkCodeAtStarter", MarkCodeAtStarter},
                   Binding{"MarkCode", MarkCode}},
                  {Binding{"OutputBasis", OutputBasis}}), mPass(pass) {
        addAttribute(kernel::LocalCarries());
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        const std::vector<PabloAST *> Basis = getInputStreamSet("Basis");
        Var * const basisVar = getInputStreamVar("Basis");
        const std::vector<PabloAST *> markCodeAtStarter = getInputStreamSet("MarkCodeAtStarter");
        const std::vector<PabloAST *> markCodes = getInputStreamSet("MarkCode");
        PabloAST * markFoundForStarter = markCodeAtStarter[0];
        for (unsigned i = 1; i < markCodeAtStarter.size(); i++) markFoundForStarter = pb.createOr(markFoundForStarter, markCodeAtStarter[i]);
        PabloAST * anyMark = markCodes[0];
        for (unsigned i = 1; i < markCodes.size(); i++) anyMark = pb.createOr(anyMark, markCodes[i]);
        std::vector<Var *> XfrmVar = xfrmVars(pb, Basis.size());
        const NFC_Tables & T = tables();
        for (const auto & d : passData(mPass)) {
            const unsigned pfx = d.first;
            auto b = pb.createScope();
            pb.createIf(pb.createAnd(pfxTest(bnc, pb, Basis, pfx), markFoundForStarter), b);
            BixNumCompiler b_bnc(b);
            ScopeClasses classes;
            std::map<codepoint_t, BitXfrms> bits;
            for (const auto & m : d.second) {
                std::map<codepoint_t, std::vector<unsigned>> xlate;
                for (codepoint_t s : m.second) xlate[s] = compositionTarget(s, T.long_composable_map.at(m.first).at(s));
                bits[m.first].add(classes, xlate);
            }
            const std::vector<PabloAST *> vars = withinMask(b, classes.compile(b, basisVar), nullptr);
            for (const auto & m : d.second) {
                PabloAST * const foundMark = b_bnc.EQ(markCodeAtStarter, markCode(mPass, pfx, m.first));
                bits[m.first].update(b, XfrmVar, foundMark, vars);
            }
        }
        PabloAST * const select = pb.createNot(u8Span(bnc, pb, Basis, anyMark));
        Var * const out = getOutputStreamVar("OutputBasis");
        for (unsigned i = 0; i < 8; i++) {
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), pb.createAnd(select, pb.createXor(Basis[i], XfrmVar[i])));
        }
    }
private:
    const unsigned mPass;
};

//  The streams of a set within a mask.
class MaskStreams final : public PabloKernel {
public:
    MaskStreams(LLVMTypeSystemInterface & ts, StreamSet * In, StreamSet * Mask, StreamSet * Out)
    : PabloKernel(ts, "NFC_MaskStreams" + std::to_string(In->getNumElements()),
                  {Binding{"In", In}, Binding{"Mask", Mask}}, {Binding{"Out", Out}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> in = getInputStreamSet("In");
        PabloAST * const mask = getInputStreamSet("Mask")[0];
        for (PabloAST *& x : in) x = pb.createAnd(x, mask);
        writeOutputStreamSet("Out", in);
    }
};

//  The candidates of NFC, and the characters that need expansion (NFC_CandidateClass).
class CandidateClassKernel final : public PabloKernel {
public:
    CandidateClassKernel(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * candidates, NormalizationInput mode)
    : PabloKernel(ts, "NFC_CandidateClass" + Basis->shapeString() + modeSuffix(mode) + nameSuffix(),
                  {Binding{"Basis", Basis, FixedRate(), LookAhead(3)}}, {Binding{"candidates", candidates}}), mMode(mode) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        UTF::UTF_Compiler compiler(getInputStreamVar("Basis"), pb, BitMovementMode::LookAhead);
        std::vector<Var *> targets = {pb.createVar("candidates", pb.createZeroes()), pb.createVar("expandfirst", pb.createZeroes())};
        //  (No character of an NFD input needs expansion.)
        const UnicodeSet expansion = mMode == NormalizationInput::Any ? tables().expansion_required_class : UnicodeSet();
        compiler.compile(targets, {tables().candidate_class, expansion});
        writeOutputStreamSet("candidates", targets);
    }
private:
    const NormalizationInput mMode;
};

//  ---- NFC on the 21-bit basis (doc/masked-nfc-plan.md, A U21 mode of NFC) ----
//
//  One position per character.  A character stage writes the first character
//  of a replacement in place and the others in the positions inserted after it;
//  a composition writes the composite at one position and deletes the other (a
//  composite never needs room); an overridable primary writes its starter in
//  place and the marks that remain in the positions inserted after it.  With a
//  mask, as in UTF-8 mode: the classes within it (the character ahead, within
//  the mask ahead).

//  The classes of the characters at a position (or one ahead), compiled on the
//  basis (looked ahead), each once.
class U21Classes {
public:
    U21Classes(std::vector<PabloAST *> basis) : mCompiler(std::move(basis)) {}
    PabloAST * get(PabloBuilder & pb, const UnicodeSet & set) {
        for (unsigned i = 0; i < mSets.size(); i++) if (mSets[i] == set) return mClasses[i];
        mSets.push_back(set);
        mClasses.push_back(set.empty() ? pb.createZeroes() : mCompiler.compileCC(re::makeCC(set, &cc::Unicode), pb));
        return mClasses.back();
    }
private:
    cc::Parabix_CC_Compiler_Builder mCompiler;
    std::vector<UnicodeSet> mSets;
    std::vector<PabloAST *> mClasses;
};

std::vector<PabloAST *> aheadBasis(PabloBuilder & pb, const std::vector<PabloAST *> & basis) {
    std::vector<PabloAST *> a;
    for (PabloAST * b : basis) a.push_back(pb.createLookahead(b, 1));
    return a;
}

//  The XOR into xfrm, at marker, of the bits of v.
void xorBits(PabloBuilder & pb, std::vector<PabloAST *> & xfrm, codepoint_t v, PabloAST * marker) {
    for (unsigned j = 0; j < 21; j++) if ((v >> j) & 1) xfrm[j] = pb.createOr(xfrm[j], marker);
}

//  The changes of the characters of a map to strings, at the positions of their
//  classes (within the mask) and the positions after them: (position, bit) ->
//  the characters whose replacement changes that bit there.
std::map<std::pair<unsigned, unsigned>, UnicodeSet> u21Changes(const std::map<codepoint_t, std::u32string> & map) {
    std::map<std::pair<unsigned, unsigned>, UnicodeSet> sets;
    for (const auto & e : map) {
        for (unsigned i = 0; i < e.second.size(); i++) {
            const codepoint_t v = i == 0 ? (e.first ^ e.second[0]) : e.second[i];
            for (unsigned j = 0; j < 21; j++) if ((v >> j) & 1) sets[{i, j}].insert(e.first);
        }
    }
    return sets;
}

void writeU21(PabloBuilder & pb, Var * out, const std::vector<PabloAST *> & basis, const std::vector<PabloAST *> & xfrm, PabloAST * del) {
    PabloAST * const select = del ? pb.createNot(del) : nullptr;
    for (unsigned i = 0; i < 21; i++) {
        PabloAST * v = pb.createXor(basis[i], xfrm[i]);
        if (select) v = pb.createAnd(select, v);
        pb.createAssign(pb.createExtract(out, pb.getInteger(i)), v);
    }
}

Bindings u21Inputs(StreamSet * Basis, StreamSet * Mask, unsigned lookahead) {
    Bindings b;
    if (lookahead) b.emplace_back("Basis", Basis, FixedRate(), LookAhead(lookahead)); else b.emplace_back("Basis", Basis);
    MaskInput::bind(b, Mask, lookahead);
    return b;
}

//  A character-to-string stage (the excluded composites, the singletons).
class CharacterStage21 final : public PabloKernel {
public:
    CharacterStage21(LLVMTypeSystemInterface & ts, std::string name, std::map<codepoint_t, std::u32string> map,
                     StreamSet * Basis, StreamSet * Out, StreamSet * Mask = nullptr)
    : PabloKernel(ts, name + MaskInput::suffix(Mask) + nameSuffix(), u21Inputs(Basis, Mask, 0), {Binding{"Out", Out}}),
      mMap(std::move(map)), mHasMask(Mask != nullptr) {
        addAttribute(kernel::LocalCarries());
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        U21Classes classes(basis);
        std::vector<PabloAST *> xfrm(21, pb.createZeroes());
        for (const auto & c : u21Changes(mMap)) {
            PabloAST * at = classes.get(pb, c.second);
            if (mask) at = pb.createAnd(at, mask);
            if (c.first.first) at = pb.createAdvance(at, c.first.first);
            xfrm[c.first.second] = pb.createOr(xfrm[c.first.second], at);
        }
        writeU21(pb, getOutputStreamVar("Out"), basis, xfrm, nullptr);
    }
private:
    const std::map<codepoint_t, std::u32string> mMap;
    const bool mHasMask;
};

//  A pass of the short composables: the composite written at the second
//  character, the first deleted (or both replaced, by a recomposition).
class ShortComposableStage21 final : public PabloKernel {
public:
    ShortComposableStage21(LLVMTypeSystemInterface & ts, unsigned pass, StreamSet * Basis, StreamSet * Out, StreamSet * Mask = nullptr)
    : PabloKernel(ts, "NFC21_ShortComposable" + std::to_string(pass) + MaskInput::suffix(Mask) + nameSuffix(),
                  u21Inputs(Basis, Mask, 1), {Binding{"Out", Out}}), mPass(pass), mHasMask(Mask != nullptr) {
        addAttribute(kernel::LocalCarries());
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        PabloAST * const maskAhead = maskAt(pb, mask, 1);
        U21Classes here(basis), ahead(aheadBasis(pb, basis));
        const auto & rules = tables().short_pass_map.at(mPass);
        std::vector<codepoint_t> cp1_list;
        for (const auto & r : rules) cp1_list.push_back(r.first);
        std::map<codepoint_t, Var *> generated;
        for (codepoint_t cp1 : cp1_list) {
            PabloAST * c = here.get(pb, UnicodeSet(cp1));
            if (mask) c = pb.createAnd(c, mask);
            generated[cp1] = pb.createVar("first", c);
        }
        auto second = [&](codepoint_t cp2) {
            PabloAST * c = ahead.get(pb, UnicodeSet(cp2));
            return maskAhead ? pb.createAnd(c, maskAhead) : c;
        };
        std::vector<PabloAST *> xfrm(21, zeroes);
        PabloAST * del = zeroes;
        std::map<codepoint_t, PabloAST *> precomposedGenerated;
        auto emitCases = [&](codepoint_t cp1) {
            for (const auto & y : rules.at(cp1)) {
                const codepoint_t cp2 = y.first;
                if (cp1 == cp2) continue;
                PabloAST * const before = pb.createAnd(generated[cp1], second(cp2));
                PabloAST * const found = pb.createAdvance(before, 1);
                if (y.second.size() == 1) {
                    del = pb.createOr(del, before);
                    xorBits(pb, xfrm, cp2 ^ y.second[0], found);
                    precomposedGenerated[y.second[0]] = found;
                } else {
                    xorBits(pb, xfrm, cp1 ^ y.second[0], before);
                    xorBits(pb, xfrm, cp2 ^ y.second[1], found);
                }
            }
        };
        //  (A composite that is itself a first character composes again: its
        //  cases follow those generating it, as in UTF-8 mode.)
        std::vector<codepoint_t> deferred;
        for (codepoint_t cp1 : cp1_list) {
            for (const auto & y : rules.at(cp1)) {
                if (y.first == cp1 || y.second.size() != 1) continue;
                const codepoint_t p = y.second[0];
                if (rules.count(p) && std::find(deferred.begin(), deferred.end(), p) == deferred.end()) deferred.push_back(p);
            }
        }
        for (codepoint_t cp1 : cp1_list) {
            if (std::find(deferred.begin(), deferred.end(), cp1) == deferred.end()) emitCases(cp1);
        }
        while (!deferred.empty()) {
            std::vector<codepoint_t> next;
            for (codepoint_t cp1 : deferred) {
                const auto g = precomposedGenerated.find(cp1);
                if (g != precomposedGenerated.end()) {
                    pb.createAssign(generated[cp1], pb.createOr(generated[cp1], g->second));
                    emitCases(cp1);
                } else {
                    next.push_back(cp1);
                }
            }
            if (next.size() == deferred.size()) report_fatal_error("ShortComposableStage21: deferred composites never generated");
            deferred = next;
        }
        writeU21(pb, getOutputStreamVar("Out"), basis, xfrm, del);
    }
private:
    const unsigned mPass;
    const bool mHasMask;
};

//  The self-composables (A A -> AA), by SelfComposableLogic, with characters of
//  one position (no UTF-8 suffixes).
class SelfComposableStage21 final : public PabloKernel {
public:
    SelfComposableStage21(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Out, StreamSet * Mask = nullptr)
    : PabloKernel(ts, "NFC21_SelfComposable" + MaskInput::suffix(Mask) + nameSuffix(), u21Inputs(Basis, Mask, 1), {Binding{"Out", Out}}),
      mHasMask(Mask != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        PabloAST * const maskAhead = maskAt(pb, mask, 1);
        U21Classes here(basis), ahead(aheadBasis(pb, basis));
        auto within = [&](PabloAST * c, PabloAST * m) {return m ? pb.createAnd(c, m) : c;};
        const std::vector<PabloAST *> noSuffix(8, zeroes);
        std::vector<PabloAST *> xfrm(21, zeroes);
        PabloAST * del = zeroes;
        for (const auto & sc : tables().self_composables) {
            const codepoint_t A = sc.first, AA = sc.second;
            const SCResults r = SelfComposableLogic(pb, noSuffix, 1, 1, within(here.get(pb, UnicodeSet(A)), mask),
                                                    within(here.get(pb, UnicodeSet(AA)), mask),
                                                    within(ahead.get(pb, UnicodeSet(A)), maskAhead),
                                                    within(ahead.get(pb, UnicodeSet(AA)), maskAhead));
            del = pb.createOr(del, r.A_or_AA_to_delete);
            xorBits(pb, xfrm, A ^ AA, pb.createOr(r.A_to_convert_to_AA, r.AA_to_convert_to_A));
        }
        writeU21(pb, getOutputStreamVar("Out"), basis, xfrm, del);
    }
private:
    const bool mHasMask;
};

//  The marks of a long-composition pass that compose with the starter they follow.
class FindComposablesStage21 final : public PabloKernel {
public:
    FindComposablesStage21(LLVMTypeSystemInterface & ts, unsigned pass, StreamSet * Basis, StreamSet * ccc_NR,
                           StreamSet * MarkCode, StreamSet * Index, StreamSet * Mask = nullptr)
    : PabloKernel(ts, "NFC21_FindComposables" + std::to_string(pass) + MaskInput::suffix(Mask) + nameSuffix(),
                  [&] {
                      Bindings b = u21Inputs(Basis, Mask, 0);
                      b.emplace_back("ccc_NR", ccc_NR);
                      return b;
                  }(),
                  {Binding{"MarkCode", MarkCode}, Binding{"Index_ccc_NR_or_MarksFound", Index}}), mPass(pass), mHasMask(Mask != nullptr) {}
protected:
    void generatePabloMethod() override {
        auto * cccObj = cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
        PabloBuilder pb(getEntryScope());
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        PabloAST * const ccc_NR = getInputStreamSet("ccc_NR")[0];
        PabloAST * const mask = mHasMask ? getInputStreamSet("Mask")[0] : nullptr;
        U21Classes classes(basis);
        const unsigned bits = tables().max_conditional_code_bits.at(mPass);
        std::vector<PabloAST *> codeBits(bits, zeroes);
        std::map<unsigned, PabloAST *> cccOrNR;
        for (const auto & d : passData(mPass)) {
            for (const auto & m : d.second) {
                UnicodeSet starters;
                for (codepoint_t s : m.second) starters.insert(s);
                PabloAST * st = classes.get(pb, starters);
                PabloAST * mk = classes.get(pb, UnicodeSet(m.first));
                if (mask) {
                    st = pb.createAnd(st, mask);
                    mk = pb.createAnd(mk, mask);
                }
                const unsigned cccEnum = cccObj->GetEnumerationValue(m.first);
                if (!cccOrNR.count(cccEnum)) cccOrNR[cccEnum] = pb.createOr(classes.get(pb, cccObj->GetCodepointSet(cccEnum)), ccc_NR);
                PabloAST * const found = pb.createAnd(pb.createAdvanceThenScanTo(st, cccOrNR[cccEnum]), mk);
                const unsigned code = markCode(mPass, d.first, m.first);
                for (unsigned i = 0; i < bits; i++) if ((code >> i) & 1) codeBits[i] = pb.createOr(codeBits[i], found);
            }
        }
        PabloAST * any = zeroes;
        for (PabloAST * c : codeBits) any = pb.createOr(any, c);
        writeOutputStreamSet("MarkCode", codeBits);
        writeOutputStreamSet("Index_ccc_NR_or_MarksFound", std::vector<PabloAST *>{pb.createOr(ccc_NR, any)});
    }
private:
    const unsigned mPass;
    const bool mHasMask;
};

//  The compositions of a long-composition pass at the starters, and the marks deleted.
class ApplyLongCompositionStage21 final : public PabloKernel {
public:
    ApplyLongCompositionStage21(LLVMTypeSystemInterface & ts, unsigned pass, StreamSet * Basis, StreamSet * MarkCodeAtStarter,
                                StreamSet * MarkCode, StreamSet * Out)
    : PabloKernel(ts, "NFC21_ApplyLongComposition" + std::to_string(pass) + nameSuffix(),
                  {Binding{"Basis", Basis}, Binding{"MarkCodeAtStarter", MarkCodeAtStarter}, Binding{"MarkCode", MarkCode}},
                  {Binding{"Out", Out}}), mPass(pass) {
        addAttribute(kernel::LocalCarries());
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        PabloAST * const zeroes = pb.createZeroes();
        const std::vector<PabloAST *> basis = getInputStreamSet("Basis");
        const std::vector<PabloAST *> atStarter = getInputStreamSet("MarkCodeAtStarter");
        const std::vector<PabloAST *> codes = getInputStreamSet("MarkCode");
        U21Classes classes(basis);
        std::vector<PabloAST *> xfrm(21, zeroes);
        for (const auto & d : passData(mPass)) {
            for (const auto & m : d.second) {
                PabloAST * const foundMark = bnc.EQ(atStarter, markCode(mPass, d.first, m.first));
                std::map<codepoint_t, std::u32string> xlate;
                for (codepoint_t s : m.second) xlate[s] = tables().long_composable_map.at(m.first).at(s);
                for (const auto & c : u21Changes(xlate)) {
                    PabloAST * at = pb.createAnd(classes.get(pb, c.second), foundMark);
                    if (c.first.first) at = pb.createAdvance(at, c.first.first);
                    xfrm[c.first.second] = pb.createOr(xfrm[c.first.second], at);
                }
            }
        }
        PabloAST * any = zeroes;
        for (PabloAST * c : codes) any = pb.createOr(any, c);
        writeU21(pb, getOutputStreamVar("Out"), basis, xfrm, any);
    }
private:
    const unsigned mPass;
};

//  The positions to insert after each character for NFC on the 21-bit basis:
//  for the decomposition of an excluded composite, or the marks an overridable
//  primary leaves after its starter.
std::map<codepoint_t, unsigned> u21Insertions() {
    std::map<codepoint_t, unsigned> ins;
    auto need = [&](codepoint_t cp, size_t n) {if (n > 1) ins[cp] = std::max<unsigned>(ins[cp], static_cast<unsigned>(n - 1));};
    for (const auto & e : tables().excluded_composite_map) need(e.first, e.second.size());
    for (const auto & o : tables().overridable_primaries) {
        for (const auto & m : o.second) need(o.first, m.second.size());
    }
    //  A singleton has the room of its canonical character (e.g. U+212B, whose
    //  U+00C5 is an overridable primary).
    for (const auto & sg : tables().singleton_map) {
        const auto f = ins.find(sg.second);
        if (f != ins.end()) ins[sg.first] = std::max(ins[sg.first], f->second);
    }
    return ins;
}

//  The work of a focused NFC on the 21-bit basis (as NFC_Focus by character):
//  the candidates, the characters before them, and those to expand.
class U21Focus final : public PabloKernel {
public:
    U21Focus(LLVMTypeSystemInterface & ts, StreamSet * Candidates, StreamSet * Work)
    : PabloKernel(ts, "NFC21_Focus", {Binding{"Candidates", Candidates, FixedRate(), LookAhead(1)}}, {Binding{"Work", Work}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> c = getInputStreamSet("Candidates");
        PabloAST * const work = pb.createOr3(c[0], pb.createLookahead(c[0], 1), c[1]);
        writeOutputStreamSet("Work", std::vector<PabloAST *>{pb.createInFile(work)});
    }
};

//  The insertion amounts of the working space (see u21Insertions), at the
//  characters of restriction (if any), or nullptr if none are needed.
StreamSet * u21InsertionBixNum(PipelineBuilder & P, StreamSet * U21, StreamSet * restriction, NormalizationInput mode) {
    std::map<codepoint_t, unsigned> ins = u21Insertions();
    //  (Only for the characters that may occur in the input.)
    const UnicodeSet possible = NormalizationInputSet(mode);
    for (auto i = ins.begin(); i != ins.end(); ) i = possible.contains(i->first) ? std::next(i) : ins.erase(i);
    unsigned maxIns = 0;
    for (const auto & e : ins) maxIns = std::max(maxIns, e.second);
    unsigned bits = 0;
    while ((maxIns >> bits) != 0) bits++;
    if (bits == 0) return nullptr;
    std::vector<re::CC *> ccs;
    for (unsigned b = 0; b < bits; b++) {
        UnicodeSet set;
        for (const auto & e : ins) if ((e.second >> b) & 1) set.insert(e.first);
        ccs.push_back(re::makeCC(set, &cc::Unicode));
    }
    StreamSet * const InsertBixNum = P.CreateStreamSet(bits);
    if (restriction) {
        P.CreateKernelCall<CharClassesKernel>(ccs, U21, StreamMask{restriction, MaskKind::Zeroing}, InsertBixNum);
    } else {
        P.CreateKernelCall<CharClassesKernel>(ccs, U21, InsertBixNum);
    }
    return InsertBixNum;
}

}

UnicodeSet NormalizationInputSet(NormalizationInput mode) {
    if (mode == NormalizationInput::Any) return UnicodeSet(0, 0x10FFFF);
    const UCD::property_t qc = mode == NormalizationInput::InputIsNFD ? UCD::NFD_QC : UCD::NFKD_QC;
    //  (Y is the first value of NFD_QC and of NFKD_QC.)
    return cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(qc))->GetCodepointSet(UCD::NFD_QC_ns::Y);
}

void NFC_InitialInsertion(PipelineBuilder & P, StreamSet * Basis, StreamSet * InsertionBixNum, StreamSet * WorkMask,
                          NormalizationInput mode) {
    P.CreateKernelCall<NFC_InsertionKernel>(Basis, InsertionBixNum, WorkMask, mode);
}

void NFC_ExcludedComposites(PipelineBuilder & P, StreamSet * Basis, StreamSet * Result, StreamSet * Mask) {
    P.CreateKernelCall<CharacterStage>("NFC_ExcludedComposites", tables().excluded_composite_map, Basis, Result, Mask);
}

void NFC_Singletons(PipelineBuilder & P, StreamSet * Basis, StreamSet * Result, StreamSet * Mask) {
    P.CreateKernelCall<CharacterStage>("NFC_Singletons", singletonStrings(), Basis, Result, Mask);
}

StreamSet * NFC_MaskStreams(PipelineBuilder & P, StreamSet * In, StreamSet * Mask) {
    StreamSet * const Out = P.CreateStreamSet(In->getNumElements(), 1);
    P.CreateKernelCall<MaskStreams>(In, Mask, Out);
    return Out;
}

void NFC_ShortComposables(PipelineBuilder & P, StreamSet * Basis, StreamSet * FinalBasis, StreamSet * Mask) {
    const NFC_Tables & T = tables();
    StreamSet * input = Basis;
    for (unsigned pass = 0; pass < T.short_pass_count; pass++) {
        StreamSet * const output = pass + 1 == T.short_pass_count ? FinalBasis : P.CreateStreamSet(8, 1);
        if (static_cast<int>(pass) == T.self_composable_pass_no) {
            P.CreateKernelCall<SelfComposableStage>(input, output, Mask);
        } else {
            P.CreateKernelCall<ShortComposableStage>(pass, input, output, Mask);
        }
        SHOW_BIXNUM(output);
        input = output;
    }
}

void NFC_LongComposables(PipelineBuilder & P, StreamSet * Basis, StreamSet * ccc_NR, StreamSet * FinalBasis, StreamSet * Mask) {
    const NFC_Tables & T = tables();
    StreamSet * input = Basis;
    for (unsigned pass = 0; pass < T.pass_count; pass++) {
        const unsigned bits = T.max_conditional_code_bits.at(pass);
        for (unsigned a = 0; a < T.pass_applications[pass]; a++) {
            const bool last = pass + 1 == T.pass_count && a + 1 == T.pass_applications[pass];
            StreamSet * const output = last ? FinalBasis : P.CreateStreamSet(8, 1);
            StreamSet * const MarkCode = P.CreateStreamSet(bits);
            StreamSet * const Index = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<FindComposablesStage>(pass, input, ccc_NR, MarkCode, Index, Mask);
            SHOW_BIXNUM(MarkCode);
            SHOW_STREAM(Index);
            StreamSet * const MarkCodeAtStarter = P.CreateStreamSet(bits, 1);
            P.CreateKernelCall<IndexedShiftBack>(Index, MarkCode, MarkCodeAtStarter);
            SHOW_BIXNUM(MarkCodeAtStarter);
            P.CreateKernelCall<ApplyLongCompositionStage>(pass, input, MarkCodeAtStarter, MarkCode, output);
            SHOW_BIXNUM(output);
            input = output;
        }
    }
}

void NFC_CandidateClasses(PipelineBuilder & P, StreamSet * Basis, StreamSet * Candidates, NormalizationInput mode) {
    P.CreateKernelCall<CandidateClassKernel>(Basis, Candidates, mode);
}

//  NFC of a 21-bit basis in its working space: the result (compressed and
//  sorted), and Valid, the positions of the working space that remain.  With an
//  input in NFD (or NFKD), the decomposition stages and the canonical ordering
//  are skipped (see NormalizationInput).
static StreamSet * U21_Transform(PipelineBuilder & P, StreamSet * U21, StreamSet * Mask, StreamSet * OutputMask, StreamSet *& Valid,
                                 NormalizationInput mode) {
    const NFC_Tables & T = tables();
    const bool decomposed = mode != NormalizationInput::Any;
    //  The working space: room for the decompositions and the marks of overrides
    //  (within the mask).
    StreamSet * Working = U21;
    StreamSet * SpreadMask = nullptr;
    StreamSet * WorkingMask = Mask;
    if (StreamSet * const InsertBixNum = u21InsertionBixNum(P, U21, Mask, mode)) {
        SpreadMask = P.CreateStreamSet(1);
        InsertionSpreadMask(P, InsertBixNum, SpreadMask, kernel::InsertPosition::After);
        Working = P.CreateStreamSet(21, 1);
        SpreadByMask(P, SpreadMask, U21, Working);
        if (Mask) {
            StreamSet * const spread = P.CreateStreamSet(1);
            SpreadByMask(P, SpreadMask, Mask, spread);
            StreamSet * const inserted = P.CreateStreamSet(1);
            P.CreateKernelCall<bixnum::EQ_immediate>(SpreadMask, 0, inserted);
            WorkingMask = P.CreateStreamSet(1);
            OrCombine(P, spread, inserted, WorkingMask);
        }
    }
    StreamSet * EC = Working;
    if (!decomposed) {
        EC = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<CharacterStage21>("NFC21_ExcludedComposites", T.excluded_composite_map, Working, EC, WorkingMask);
    }
    //  The starters (not the positions inserted), and with a mask the positions
    //  outside it, which block the long compositions.
    auto * cccObj = cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::ccc));
    StreamSet * ccc_NR = P.CreateStreamSet(1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{re::makeCC(cccObj->GetCodepointSet(UCD::CCC_ns::NR), &cc::Unicode)}, EC, ccc_NR);
    if (SpreadMask) {
        StreamSet * const original = P.CreateStreamSet(1);
        AndCombine(P, ccc_NR, SpreadMask, original);
        ccc_NR = original;
    }
    StreamSet * blocking = ccc_NR;
    if (WorkingMask) {
        StreamSet * const outside = P.CreateStreamSet(1);
        P.CreateKernelCall<bixnum::EQ_immediate>(WorkingMask, 0, outside);
        blocking = P.CreateStreamSet(1);
        OrCombine(P, ccc_NR, outside, blocking);
    }
    StreamSet * input = EC;
    if (!decomposed) {
        input = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<CharacterStage21>("NFC21_Singletons", singletonStrings(), EC, input, WorkingMask);
    }
    for (unsigned pass = 0; pass < T.short_pass_count; pass++) {
        StreamSet * const output = P.CreateStreamSet(21, 1);
        if (static_cast<int>(pass) == T.self_composable_pass_no) {
            P.CreateKernelCall<SelfComposableStage21>(input, output, WorkingMask);
        } else {
            P.CreateKernelCall<ShortComposableStage21>(pass, input, output, WorkingMask);
        }
        input = output;
    }
    for (unsigned pass = 0; pass < T.pass_count; pass++) {
        const unsigned codeBits = T.max_conditional_code_bits.at(pass);
        for (unsigned a = 0; a < T.pass_applications[pass]; a++) {
            StreamSet * const MarkCode = P.CreateStreamSet(codeBits);
            StreamSet * const Index = P.CreateStreamSet(1);
            P.CreateKernelCall<FindComposablesStage21>(pass, input, blocking, MarkCode, Index, WorkingMask);
            StreamSet * const AtStarter = P.CreateStreamSet(codeBits);
            P.CreateKernelCall<IndexedShiftBack>(Index, MarkCode, AtStarter);
            StreamSet * const output = P.CreateStreamSet(21, 1);
            P.CreateKernelCall<ApplyLongCompositionStage21>(pass, input, AtStarter, MarkCode, output);
            input = output;
        }
    }
    StreamSet * L_V_T = P.CreateStreamSet(Hangul_Composables::HC_Kind::Count);
    P.CreateKernelCall<Hangul_Composables>(input, L_V_T, pablo::BitMovementMode::LookAhead);
    if (WorkingMask) L_V_T = NFC_MaskStreams(P, L_V_T, WorkingMask);
    StreamSet * const Translated = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<Hangul_Composition>(input, L_V_T, Translated);
    //  The positions that remain: the characters, and the nulls of the source.
    StreamSet * const NonZero = P.CreateStreamSet(1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(Translated, 0, NonZero);
    StreamSet * const SourceZero = P.CreateStreamSet(1);
    P.CreateKernelCall<bixnum::EQ_immediate>(Working, 0, SourceZero);
    StreamSet * SourceNull = SourceZero;
    if (SpreadMask) {
        SourceNull = P.CreateStreamSet(1);
        AndCombine(P, SourceZero, SpreadMask, SourceNull);
    }
    Valid = P.CreateStreamSet(1);
    OrCombine(P, NonZero, SourceNull, Valid);
    StreamSet * const Compressed = P.CreateStreamSet(21, 1);
    FilterByMask(P, Valid, Translated, Compressed);
    StreamSet * CompressedMask = nullptr;
    if (WorkingMask && (OutputMask || !decomposed)) {
        CompressedMask = OutputMask ? OutputMask : P.CreateStreamSet(1);
        FilterByMask(P, Valid, WorkingMask, CompressedMask);
    }
    //  (From an NFD input, composition leaves the marks that remain in canonical order.)
    if (decomposed) return Compressed;
    //  The canonical ordering (within the mask).
    StreamSet * const CCC_Basis = P.CreateStreamSet(cccObj->GetEnumerationBasisSets().size(), 1);
    P.CreateKernelCall<UnicodePropertyBasis>(cccObj, Compressed, CCC_Basis);
    StreamSet * NonZeroCCC = P.CreateStreamSet(1);
    P.CreateKernelCall<bixnum::NEQ_immediate>(CCC_Basis, 0, NonZeroCCC);
    if (CompressedMask) {
        StreamSet * const within = P.CreateStreamSet(1);
        AndCombine(P, NonZeroCCC, CompressedMask, within);
        NonZeroCCC = within;
    }
    StreamSet * const Sorted = P.CreateStreamSet(21, 1);
    SortRuns(P, NonZeroCCC, CCC_Basis, Compressed, Sorted);
    return Sorted;
}

//  NFC of a 21-bit basis: in place, U21_Transform of the whole text; focused, of
//  its work regions (U21Focus, within the mask: the characters before the
//  candidates are kept even outside it, inert, so that the regions do not
//  compose across their seams), placed in the output as NFC_WorkPlacement
//  places the UTF-8 work: the source expanded by the same insertions at the
//  work, with the valid positions of the working space at the work.
StreamSet * NFC_U21(PipelineBuilder & P, StreamSet * U21, StreamSet * Mask, StreamSet * OutputMask, bool focus,
                    NormalizationInput mode) {
    StreamSet * Valid = nullptr;
    if (!focus) return U21_Transform(P, U21, Mask, OutputMask, Valid, mode);
    StreamSet * const Candidates = P.CreateStreamSet(2);
    //  (No character of an NFD input needs expansion.)
    const UnicodeSet expansion = mode == NormalizationInput::Any ? tables().expansion_required_class : UnicodeSet();
    std::vector<re::CC *> ccs{re::makeCC(tables().candidate_class, &cc::Unicode), re::makeCC(expansion, &cc::Unicode)};
    if (Mask) {
        P.CreateKernelCall<CharClassesKernel>(ccs, U21, StreamMask{Mask, MaskKind::Zeroing}, Candidates);
    } else {
        P.CreateKernelCall<CharClassesKernel>(ccs, U21, Candidates);
    }
    StreamSet * const Work = P.CreateStreamSet(1);
    P.CreateKernelCall<U21Focus>(Candidates, Work);
    SHOW_STREAM(Work);
    StreamSet * const Focused = P.CreateStreamSet(21, 1);
    FilterByMask(P, Work, U21, Focused);
    StreamSet * FocusedMask = nullptr;
    if (Mask) {
        FocusedMask = P.CreateStreamSet(1);
        FilterByMask(P, Work, Mask, FocusedMask);
    }
    StreamSet * const TransformedMask = OutputMask ? P.CreateStreamSet(1) : nullptr;
    StreamSet * const Transformed = U21_Transform(P, Focused, FocusedMask, TransformedMask, Valid, mode);
    //  The placement of the transformed characters in the output.
    StreamSet * restriction = Work;
    if (Mask) {
        restriction = P.CreateStreamSet(1);
        AndCombine(P, Work, Mask, restriction);
    }
    StreamSet * ExpandedWork = Work;
    if (StreamSet * const InsertBixNum = u21InsertionBixNum(P, U21, restriction, mode)) {
        StreamSet * const SourceExpansion = P.CreateStreamSet(1);
        InsertionSpreadMask(P, InsertBixNum, SourceExpansion, kernel::InsertPosition::After);
        ExpandedWork = P.CreateStreamSet(1);
        ExpandFilter(P, SourceExpansion, Work, ExpandedWork);
    }
    StreamSet * const Final = P.CreateStreamSet(1);
    ExpandFilter(P, ExpandedWork, Valid, Final);
    StreamSet * const Placement = P.CreateStreamSet(1);
    FilterByMask(P, Final, ExpandedWork, Placement);
    SHOW_STREAM(Placement);
    //  The unmodified characters, and the output.
    StreamSet * const NotWork = P.CreateStreamSet(1);
    P.CreateKernelCall<bixnum::EQ_immediate>(Work, 0, NotWork);
    StreamSet * const Unmodified = P.CreateStreamSet(21, 1);
    FilterByMask(P, NotWork, U21, Unmodified);
    StreamSet * const Output = P.CreateStreamSet(21, 1);
    MergeByMask(P, Placement, Transformed, Unmodified, Output);
    if (OutputMask) {
        StreamSet * const UnmodifiedMask = P.CreateStreamSet(1);
        FilterByMask(P, NotWork, Mask, UnmodifiedMask);
        MergeByMask(P, Placement, TransformedMask, UnmodifiedMask, OutputMask);
    }
    return Output;
}

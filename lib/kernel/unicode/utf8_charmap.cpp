/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/utf8_charmap.h>

#include <cstdio>
#include <map>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8_support.h>
#include <pablo/pablo.h>
#include <re/adt/re_cc.h>
#include <re/alphabet/alphabet.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <toolchain/toolchain.h>

using namespace pablo;

namespace kernel {

namespace {

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)

std::string utf8(const std::u32string & s) {
    std::string out;
    for (const char32_t c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

unsigned bitsFor(unsigned n) {
    unsigned bits = 0;
    while ((n >> bits) != 0) bits++;
    return bits;
}

//  The positions where bits (a number at each position, 0 where none) give a
//  number of a set (of numbers from 1): compiled as a class over the bits, so
//  that the logic grows with the structure of the set, not with its size (a
//  map may have thousands of strings or tails).
class NumberSets {
public:
    NumberSets(PabloBuilder & pb, std::vector<PabloAST *> bits) : mPB(pb) {
        bits.resize(std::max<size_t>(bits.size(), 21), pb.createZeroes());
        mCompiler = std::make_unique<cc::Parabix_CC_Compiler_Builder>(bits);
    }
    PabloAST * in(const UCD::UnicodeSet & set) {
        if (set.empty()) return mPB.createZeroes();
        return mCompiler->compileCC(re::makeCC(set, &cc::Unicode), mPB);
    }
private:
    PabloBuilder & mPB;
    std::unique_ptr<cc::Parabix_CC_Compiler_Builder> mCompiler;
};

//  The plan of a character map.  Each character mapped keeps, in place, as
//  many of its bytes as its replacement has (up to all of them), changed by
//  XOR to the bytes of the replacement; the bytes of the character beyond are
//  deleted, or those of the replacement beyond (its tail) inserted after the
//  character's last byte.  The tails are numbered (from 1), and the bytes
//  inserted after a character given by the number of its tail at its last
//  byte: only the basis bits, the lengths and the numbers of the tails are
//  carried through the deletions and the insertions.
//
//  The classes of the characters mapped, at their first bytes, in order:
//    - Mapped: all the characters mapped;
//    - for byte j and bit b, the characters whose replacement's byte j differs
//      from their own in bit b (j within both);
//    - the bits of the number of bytes to delete (the last bytes of a
//      character longer than its nonempty replacement);
//    - Empty: the characters deleted (an empty replacement), if any;
//    - the bits of the number of the tail inserted (0: none).
struct Plan {
    unsigned changeBytes = 0;   // the bytes changed in place: at most 4
    unsigned deleteBits = 0;
    bool empty = false;
    std::vector<std::string> tails;     // by number less one
    unsigned tailBits = 0;
    unsigned insertBits = 0;    // the bits of the longest tail's length
    unsigned changeIndex(unsigned j, unsigned b) const {return 1 + 8 * j + b;}
    unsigned deleteIndex() const {return 1 + 8 * changeBytes;}
    unsigned emptyIndex() const {return deleteIndex() + deleteBits;}
    unsigned tailIndex() const {return emptyIndex() + (empty ? 1 : 0);}
    unsigned count() const {return tailIndex() + tailBits;}
    std::string signature() const {
        std::string s = std::to_string(changeBytes) + "_" + std::to_string(deleteBits) + (empty ? "e" : "") + "_";
        for (const std::string & t : tails) {
            for (const char c : t) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned char>(c));
                s += buf;
            }
            s += ".";
        }
        return s;
    }
};

//  The text with each character mapped changed in place (Translated), the
//  positions kept (Keep, if bytes are deleted), and the insertions (Inserts,
//  if bytes are inserted): the bits of the number of bytes to insert, then
//  those of the number of the tail, both at the last byte of each character
//  (after which they are inserted).
class UTF8_CharMapApply final : public PabloKernel {
public:
    //  (With Applies, the characters of the class Contextual (the last of the
    //  classes) are mapped only where Applies marks their last bytes.)
    UTF8_CharMapApply(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Classes, StreamSet * Applies, StreamSet * Within,
                      Plan plan, StreamSet * Translated, StreamSet * Keep, StreamSet * Inserts)
    : PabloKernel(ts, "UTF8_CharMapApply_" + getStringHash(plan.signature()) + "_" + std::to_string(Classes->getNumElements()) +
                      (Keep ? "_k" : "") + (Inserts ? "_i" : "") +
                      (Applies ? "_c" : "") + (Within ? "_w" : ""),
                  [&] {
                      Bindings in{Binding{"basis", Basis, FixedRate(1), LookAhead(3)}, Binding{"classes", Classes}};
                      if (Applies) in.emplace_back("applies", Applies, FixedRate(1), LookAhead(3));
                      if (Within) in.emplace_back("within", Within);
                      return in;
                  }(),
                  [&] {
                      Bindings out{Binding{"translated", Translated}};
                      if (Keep) out.emplace_back("keep", Keep);
                      if (Inserts) out.emplace_back("inserts", Inserts);
                      return out;
                  }()),
      mPlan(std::move(plan)), mKeep(Keep != nullptr), mInserts(Inserts != nullptr), mApplies(Applies != nullptr), mWithin(Within != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> basis = getInputStreamSet("basis");
        const std::vector<PabloAST *> classes = getInputStreamSet("classes");
        PabloAST * mapped = classes[0];
        if (mApplies) {
            // The contextual characters where Applies marks their last bytes
            // (L - 1 bytes ahead of the first byte of a character of length L).
            Var * const applies = getInputStreamVar("applies");
            PabloAST * const at = pb.createExtract(applies, pb.getInteger(0));
            PabloAST * const contextual = classes.back();
            PabloAST * const prefix = pb.createAnd(basis[7], basis[6]);
            PabloAST * const prefix34 = pb.createAnd(prefix, basis[5]);
            PabloAST * holds = pb.createAnd(pb.createNot(basis[7]), at);
            holds = pb.createOr(holds, pb.createAnd(pb.createAnd(prefix, pb.createNot(basis[5])), pb.createLookahead(at, 1)));
            holds = pb.createOr(holds, pb.createAnd(pb.createAnd(prefix34, pb.createNot(basis[4])), pb.createLookahead(at, 2)));
            holds = pb.createOr(holds, pb.createAnd(pb.createAnd(prefix34, basis[4]), pb.createLookahead(at, 3)));
            mapped = pb.createOr(pb.createAnd(mapped, pb.createNot(contextual)), pb.createAnd(contextual, holds));
        }
        // (Within a filter, the characters at its positions only.)
        if (mWithin) mapped = pb.createAnd(mapped, getInputStreamSet("within")[0]);
        // The bytes changed in place.
        std::vector<PabloAST *> translated;
        for (unsigned b = 0; b < 8; b++) {
            PabloAST * change = pb.createZeroes();
            for (unsigned j = 0; j < mPlan.changeBytes; j++) {
                PabloAST * const c = pb.createAnd(mapped, classes[mPlan.changeIndex(j, b)]);
                change = pb.createOr(change, (j == 0) ? c : pb.createAdvance(c, j));
            }
            translated.push_back(pb.createXor(basis[b], change));
        }
        // The characters mapped, by their lengths.
        PabloAST * const prefix = pb.createAnd(basis[7], basis[6]);
        PabloAST * const prefix34 = pb.createAnd(prefix, basis[5]);
        PabloAST * const len1 = pb.createAnd(mapped, pb.createNot(basis[7]));
        PabloAST * const len2 = pb.createAnd(mapped, pb.createAnd(prefix, pb.createNot(basis[5])));
        PabloAST * const len3 = pb.createAnd(mapped, pb.createAnd(prefix34, pb.createNot(basis[4])));
        PabloAST * const len4 = pb.createAnd(mapped, pb.createAnd(prefix34, basis[4]));
        // The deletions: the last d bytes of a character of length L (byte j,
        // 1 <= j < L, if d >= L - j), and all the bytes of a character deleted.
        PabloAST * deleted = pb.createZeroes();
        if (mPlan.deleteBits > 0) {
            PabloAST * const d0 = classes[mPlan.deleteIndex()];
            PabloAST * const d1 = (mPlan.deleteBits > 1) ? classes[mPlan.deleteIndex() + 1] : pb.createZeroes();
            PabloAST * const atLeast1 = pb.createOr(d0, d1);
            PabloAST * const atLeast2 = d1;
            PabloAST * const atLeast3 = pb.createAnd(d0, d1);
            deleted = pb.createOr(deleted, pb.createAdvance(pb.createAnd(len2, atLeast1), 1));
            deleted = pb.createOr3(deleted, pb.createAdvance(pb.createAnd(len3, atLeast2), 1), pb.createAdvance(pb.createAnd(len3, atLeast1), 2));
            deleted = pb.createOr3(deleted, pb.createAdvance(pb.createAnd(len4, atLeast3), 1), pb.createAdvance(pb.createAnd(len4, atLeast2), 2));
            deleted = pb.createOr(deleted, pb.createAdvance(pb.createAnd(len4, atLeast1), 3));
        }
        if (mPlan.empty) {
            PabloAST * const e = pb.createAnd(mapped, classes[mPlan.emptyIndex()]);
            deleted = pb.createOr3(deleted, e, pb.createAdvance(pb.createAnd(e, prefix), 1));
            deleted = pb.createOr3(deleted, pb.createAdvance(pb.createAnd(e, prefix34), 2), pb.createAdvance(pb.createAnd(e, len4), 3));
        }
        // The numbers of the tails, moved from the first bytes to the last.
        auto atLast = [&](PabloAST * x) {
            PabloAST * at = pb.createOr(pb.createAnd(len1, x), pb.createAdvance(pb.createAnd(len2, x), 1));
            return pb.createOr3(at, pb.createAdvance(pb.createAnd(len3, x), 2), pb.createAdvance(pb.createAnd(len4, x), 3));
        };
        std::vector<PabloAST *> tail;
        for (unsigned k = 0; k < mPlan.tailBits; k++) tail.push_back(atLast(classes[mPlan.tailIndex() + k]));
        writeOutputStreamSet("translated", translated);
        if (mKeep) writeOutputStreamSet("keep", std::vector<PabloAST *>{pb.createInFile(pb.createNot(deleted))});
        if (mInserts) {
            // The lengths of the tails, by their numbers.
            std::vector<PabloAST *> inserts(mPlan.insertBits, pb.createZeroes());
            NumberSets tailIn(pb, tail);
            for (unsigned k = 0; k < mPlan.insertBits; k++) {
                UCD::UnicodeSet with;
                for (unsigned t = 1; t <= mPlan.tails.size(); t++) {
                    if ((mPlan.tails[t - 1].size() >> k) & 1) with.insert(t);
                }
                inserts[k] = tailIn.in(with);
            }
            inserts.insert(inserts.end(), tail.begin(), tail.end());
            writeOutputStreamSet("inserts", inserts);
        }
    }
private:
    const Plan mPlan;
    const bool mKeep;
    const bool mInserts;
    const bool mApplies;
    const bool mWithin;
};


//  The bytes of the tails written at the positions inserted (zeroes) after
//  the last bytes of the characters, given the numbers of the tails there.
class UTF8_CharMapTails final : public PabloKernel {
public:
    UTF8_CharMapTails(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Tails, Plan plan, StreamSet * Result)
    : PabloKernel(ts, "UTF8_CharMapTails_" + getStringHash(plan.signature()),
                  {Binding{"basis", Basis}, Binding{"tails", Tails}}, {Binding{"result", Result}}),
      mPlan(std::move(plan)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> result = getInputStreamSet("basis");
        const std::vector<PabloAST *> tails = getInputStreamSet("tails");
        //  Bit b of byte i of the tails: the numbers of the tails with it, i + 1
        //  positions after.
        NumberSets tailIn(pb, tails);
        size_t longest = 0;
        for (const std::string & tail : mPlan.tails) longest = std::max(longest, tail.size());
        for (unsigned i = 0; i < longest; i++) {
            for (unsigned b = 0; b < 8; b++) {
                UCD::UnicodeSet with;
                for (unsigned t = 1; t <= mPlan.tails.size(); t++) {
                    const std::string & tail = mPlan.tails[t - 1];
                    if (i < tail.size() && ((static_cast<unsigned char>(tail[i]) >> b) & 1)) with.insert(t);
                }
                if (!with.empty()) result[b] = pb.createOr(result[b], pb.createAdvance(tailIn.in(with), i + 1));
            }
        }
        writeOutputStreamSet("result", result);
    }
private:
    const Plan mPlan;
};


//  The selector values of the strings that keep the character (see
//  UTF8_SelectorKept), whose UTF-8 begins with these bytes.
static const std::string KeptPrefix = "\xEF\xB7\x91";
static bool keptString(const std::string & str) {return str.compare(0, KeptPrefix.size(), KeptPrefix) == 0;}

//  The positions with a selector (a number other than 0) that replaces the
//  character (not one of Kept).
class SelectorsPresent final : public PabloKernel {
public:
    SelectorsPresent(LLVMTypeSystemInterface & ts, StreamSet * Selectors, UCD::UnicodeSet kept, StreamSet * Present)
    : PabloKernel(ts, "SelectorsPresent" + std::to_string(Selectors->getNumElements()) + (kept.empty() ? "" : "_" + keptKey(kept)),
                  {Binding{"selectors", Selectors}}, {Binding{"present", Present}}),
      mKept(std::move(kept)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> sel = getInputStreamSet("selectors");
        PabloAST * any = pb.createZeroes();
        for (PabloAST * b : sel) any = pb.createOr(any, b);
        if (!mKept.empty()) {
            NumberSets selIn(pb, sel);
            any = pb.createAnd(any, pb.createNot(selIn.in(mKept)));
        }
        writeOutputStreamSet("present", std::vector<PabloAST *>{any});
    }
private:
    static std::string keptKey(const UCD::UnicodeSet & kept) {
        std::string s;
        for (const auto & r : kept) s += std::to_string(r.first) + "-" + std::to_string(r.second) + ",";
        return getStringHash(s);
    }
    const UCD::UnicodeSet mKept;
};

//  The selector form of a map (see UTF8_SelectorPipeline): at the last byte of
//  each character with a selector s (Selectors, a number there; 0: none), the
//  first byte of string s is assigned (not XORed: it does not depend on the
//  character replaced), the character's other bytes (Spans, all the bytes of
//  the characters with a selector) are deleted, or all of them for an empty
//  string, and the rest of the string is inserted after it (the bits of its
//  length, then of the number of its tail, at the last byte).  A string that
//  keeps the character (UTF8_SelectorKept) leaves its bytes, and the rest of
//  the string is inserted after it.
class UTF8_SelectorApply final : public PabloKernel {
public:
    UTF8_SelectorApply(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Selectors, StreamSet * Spans,
                       std::vector<std::string> strings, std::vector<unsigned> tailOf, Plan plan,
                       StreamSet * Translated, StreamSet * Keep, StreamSet * Inserts)
    : PabloKernel(ts, "UTF8_SelectorApply_" + getStringHash(signature(strings, plan)) + "_" + std::to_string(Selectors->getNumElements()) +
                      (Keep ? "_k" : "") + (Inserts ? "_i" : ""),
                  {Binding{"basis", Basis}, Binding{"selectors", Selectors}, Binding{"spans", Spans}},
                  outputs(Translated, Keep, Inserts)),
      mStrings(std::move(strings)), mTailOf(std::move(tailOf)), mPlan(std::move(plan)), mKeep(Keep != nullptr), mInserts(Inserts != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("basis");
        const std::vector<PabloAST *> sel = getInputStreamSet("selectors");
        PabloAST * const spans = getInputStreamSet("spans")[0];
        PabloAST * changed = pb.createZeroes();
        for (PabloAST * b : sel) changed = pb.createOr(changed, b);
        //  The selector values with each bit of the first byte, of the length of
        //  the tail and of its number, and those of the empty string, and of the
        //  strings that keep the character (their tails all but the mark).
        UCD::UnicodeSet emptySet, keptSet;
        std::vector<UCD::UnicodeSet> assignedSet(8), countSet(mPlan.insertBits), tailSet(mPlan.tailBits);
        for (unsigned s = 1; s <= mStrings.size(); s++) {
            const std::string & str = mStrings[s - 1];
            if (str.empty()) {
                emptySet.insert(s);
                continue;
            }
            const bool kept = keptString(str);
            if (kept) {
                keptSet.insert(s);
            } else {
                for (unsigned b = 0; b < 8; b++) {
                    if ((static_cast<unsigned char>(str[0]) >> b) & 1) assignedSet[b].insert(s);
                }
            }
            const unsigned len = static_cast<unsigned>(str.size() - (kept ? KeptPrefix.size() : 1));
            for (unsigned k = 0; k < mPlan.insertBits; k++) if ((len >> k) & 1) countSet[k].insert(s);
            const unsigned t = mTailOf[s - 1];
            for (unsigned k = 0; k < mPlan.tailBits; k++) if ((t >> k) & 1) tailSet[k].insert(s);
        }
        NumberSets selIn(pb, sel);
        PabloAST * const emptied = selIn.in(emptySet);
        if (!keptSet.empty()) changed = pb.createAnd(changed, pb.createNot(selIn.in(keptSet)));
        std::vector<PabloAST *> assigned(8), counts(mPlan.insertBits), tails(mPlan.tailBits);
        for (unsigned b = 0; b < 8; b++) assigned[b] = selIn.in(assignedSet[b]);
        for (unsigned k = 0; k < mPlan.insertBits; k++) counts[k] = selIn.in(countSet[k]);
        for (unsigned k = 0; k < mPlan.tailBits; k++) tails[k] = selIn.in(tailSet[k]);
        for (unsigned b = 0; b < 8; b++) basis[b] = pb.createOr(pb.createAnd(basis[b], pb.createNot(changed)), assigned[b]);
        writeOutputStreamSet("translated", basis);
        if (mKeep) {
            //  (Deleted: the bytes of a character with a selector but its last, and
            //  the last too for an empty string.)
            PabloAST * const deleted = pb.createOr(pb.createAnd(spans, pb.createNot(changed)), emptied);
            writeOutputStreamSet("keep", std::vector<PabloAST *>{pb.createInFile(pb.createNot(deleted))});
        }
        if (mInserts) {
            std::vector<PabloAST *> inserts = counts;
            inserts.insert(inserts.end(), tails.begin(), tails.end());
            writeOutputStreamSet("inserts", inserts);
        }
    }
private:
    static Bindings outputs(StreamSet * Translated, StreamSet * Keep, StreamSet * Inserts) {
        Bindings b{Binding{"translated", Translated}};
        if (Keep) b.emplace_back("keep", Keep);
        if (Inserts) b.emplace_back("inserts", Inserts);
        return b;
    }
    static std::string signature(const std::vector<std::string> & strings, const Plan & plan) {
        std::string s = plan.signature() + "|";
        for (const std::string & str : strings) {
            for (const char c : str) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned char>(c));
                s += buf;
            }
            s += ".";
        }
        return s;
    }
    const std::vector<std::string> mStrings;
    const std::vector<unsigned> mTailOf;
    const Plan mPlan;
    const bool mKeep;
    const bool mInserts;
};

//  The prefixes of the strings that keep a character with a string inserted
//  before it (see UTF8_SelectorPipeline): numbered from 1, the bytes inserted
//  before the character's first byte.
struct PrefixPlan {
    std::vector<std::string> prefixes;      // by number less one
    std::vector<unsigned> prefixOf;         // by selector less one (0: none)
    unsigned numberBits = 0;
    unsigned countBits = 0;                 // the bits of the longest prefix's length
    unsigned longest() const {
        size_t n = 0;
        for (const std::string & p : prefixes) n = std::max(n, p.size());
        return static_cast<unsigned>(n);
    }
    std::string signature() const {
        std::string s;
        for (const std::string & p : prefixes) {
            for (const char c : p) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned char>(c));
                s += buf;
            }
            s += ".";
        }
        s += "|";
        for (const unsigned n : prefixOf) s += std::to_string(n) + ",";
        return s;
    }
};

//  At the first byte of each character whose selector (at its last byte) has
//  a prefix: the bits of the prefix's length, then of its number.
class UTF8_SelectorPrefixes final : public PabloKernel {
public:
    UTF8_SelectorPrefixes(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Selectors, PrefixPlan plan, StreamSet * Prefixes)
    : PabloKernel(ts, "UTF8_SelectorPrefixes_" + getStringHash(plan.signature()) + "_" + std::to_string(Selectors->getNumElements()),
                  {Binding{"basis", Basis}, Binding{"selectors", Selectors, FixedRate(1), LookAhead(3)}},
                  {Binding{"prefixes", Prefixes}}),
      mPlan(std::move(plan)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> basis = getInputStreamSet("basis");
        const std::vector<PabloAST *> sel = getInputStreamSet("selectors");
        //  The sets of selector values with each bit of the length and number of their prefix.
        std::vector<UCD::UnicodeSet> countSet(mPlan.countBits), numberSet(mPlan.numberBits);
        for (unsigned s = 1; s <= mPlan.prefixOf.size(); s++) {
            const unsigned n = mPlan.prefixOf[s - 1];
            if (n == 0) continue;
            const unsigned len = static_cast<unsigned>(mPlan.prefixes[n - 1].size());
            for (unsigned k = 0; k < mPlan.countBits; k++) if ((len >> k) & 1) countSet[k].insert(s);
            for (unsigned k = 0; k < mPlan.numberBits; k++) if ((n >> k) & 1) numberSet[k].insert(s);
        }
        //  The first bytes of the characters of each length, whose selectors are
        //  length - 1 positions ahead.
        NumberSets byteIn(pb, basis);
        const UCD::UnicodeSet leads[4] = {UCD::UnicodeSet(0, 0x7F), UCD::UnicodeSet(0xC0, 0xDF), UCD::UnicodeSet(0xE0, 0xEF),
                                          UCD::UnicodeSet(0xF0, 0xF7)};
        std::vector<PabloAST *> out(mPlan.countBits + mPlan.numberBits, pb.createZeroes());
        for (unsigned n = 1; n <= 4; n++) {
            PabloAST * const lead = byteIn.in(leads[n - 1]);
            std::vector<PabloAST *> ahead;
            for (PabloAST * b : sel) ahead.push_back(n == 1 ? b : pb.createLookahead(b, n - 1));
            NumberSets selIn(pb, ahead);
            for (unsigned k = 0; k < mPlan.countBits; k++) out[k] = pb.createOr(out[k], pb.createAnd(lead, selIn.in(countSet[k])));
            for (unsigned k = 0; k < mPlan.numberBits; k++) {
                out[mPlan.countBits + k] = pb.createOr(out[mPlan.countBits + k], pb.createAnd(lead, selIn.in(numberSet[k])));
            }
        }
        writeOutputStreamSet("prefixes", out);
    }
private:
    const PrefixPlan mPlan;
};


//  The bytes of the prefixes written at the positions inserted (zeroes) before
//  the first bytes of the characters, given the numbers of the prefixes there:
//  byte j of a prefix of length L, L - j positions before.
class UTF8_SelectorPrefixWrite final : public PabloKernel {
public:
    UTF8_SelectorPrefixWrite(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Numbers, PrefixPlan plan, StreamSet * Result)
    : PabloKernel(ts, "UTF8_SelectorPrefixWrite_" + getStringHash(plan.signature()),
                  {Binding{"basis", Basis}, Binding{"numbers", Numbers, FixedRate(1), LookAhead(plan.longest())}},
                  {Binding{"result", Result}}),
      mPlan(std::move(plan)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> result = getInputStreamSet("basis");
        const std::vector<PabloAST *> numbers = getInputStreamSet("numbers");
        for (unsigned d = 1; d <= mPlan.longest(); d++) {
            std::vector<PabloAST *> ahead;
            for (PabloAST * b : numbers) ahead.push_back(pb.createLookahead(b, d));
            NumberSets numberIn(pb, ahead);
            for (unsigned b = 0; b < 8; b++) {
                UCD::UnicodeSet with;
                for (unsigned n = 1; n <= mPlan.prefixes.size(); n++) {
                    const std::string & p = mPlan.prefixes[n - 1];
                    if (p.size() < d) continue;
                    if ((static_cast<unsigned char>(p[p.size() - d]) >> b) & 1) with.insert(n);
                }
                if (!with.empty()) result[b] = pb.createOr(result[b], numberIn.in(with));
            }
        }
        writeOutputStreamSet("result", result);
    }
private:
    const PrefixPlan mPlan;
};

//  The prefixes inserted before the characters whose selectors have them:
//  returns the basis with them, and the selectors kept in step; the positions
//  inserted (each in the run of the character after it) added to Positions.
StreamSet * insertPrefixes(PipelineBuilder & P, const PrefixPlan & plan, StreamSet * Basis, StreamSet *& Selectors,
                           PositionMap * Positions) {
    StreamSet * const Prefixes = P.CreateStreamSet(plan.countBits + plan.numberBits);
    P.CreateKernelCall<UTF8_SelectorPrefixes>(Basis, Selectors, plan, Prefixes);
    std::vector<unsigned> countBits;
    for (unsigned k = 0; k < plan.countBits; k++) countBits.push_back(k);
    StreamSet * const Counts = P.CreateStreamSet(plan.countBits);
    P.CreateKernelCall<StreamSelect>(Counts, Select(Prefixes, countBits));
    StreamSet * const SpreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, Counts, SpreadMask, InsertPosition::Before);
    SHOW_STREAM(SpreadMask);
    StreamSet * const SpreadBasis = P.CreateStreamSet(8);
    SpreadByMask(P, SpreadMask, Basis, SpreadBasis);
    StreamSet * const SpreadSelectors = P.CreateStreamSet(Selectors->getNumElements());
    SpreadByMask(P, SpreadMask, Selectors, SpreadSelectors);
    Selectors = SpreadSelectors;
    StreamSet * const Numbers = P.CreateStreamSet(plan.numberBits);
    SpreadByMask(P, SpreadMask, Prefixes, Numbers, plan.countBits);
    if (Positions) Positions->insert(SpreadMask, PositionMap::Inserted::InheritNext, plan.longest());
    StreamSet * const Result = P.CreateStreamSet(8);
    P.CreateKernelCall<UTF8_SelectorPrefixWrite>(SpreadBasis, Numbers, plan, Result);
    SHOW_BIXNUM(Result);
    return Result;
}

//  The rest of a map, from the text changed in place: the bytes deleted (Keep)
//  and the tails inserted (Inserts), added to Positions (each byte inserted in
//  the run of the character before it).
StreamSet * finishMap(PipelineBuilder & P, const Plan & plan, StreamSet * Translated, StreamSet * Keep, StreamSet * Inserts,
                      const bool deletes, const bool inserts, PositionMap * Positions) {
    SHOW_BIXNUM(Translated);
    StreamSet * Basis = Translated;
    if (deletes) {
        SHOW_STREAM(Keep);
        StreamSet * const KeptBasis = P.CreateStreamSet(8);
        FilterByMask(P, Keep, Basis, KeptBasis);
        Basis = KeptBasis;
        if (Inserts) {
            StreamSet * const KeptInserts = P.CreateStreamSet(Inserts->getNumElements());
            FilterByMask(P, Keep, Inserts, KeptInserts);
            Inserts = KeptInserts;
        }
        if (Positions) Positions->remove(Keep);
    }
    if (!inserts) return Basis;
    // The insertions: the positions inserted after the last bytes, and the
    // bytes of the tails written there.
    std::vector<unsigned> countBits;
    for (unsigned k = 0; k < plan.insertBits; k++) countBits.push_back(k);
    StreamSet * const Counts = P.CreateStreamSet(plan.insertBits);
    P.CreateKernelCall<StreamSelect>(Counts, Select(Inserts, countBits));
    StreamSet * const SpreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, Counts, SpreadMask, InsertPosition::After);
    SHOW_STREAM(SpreadMask);
    StreamSet * const SpreadBasis = P.CreateStreamSet(8);
    SpreadByMask(P, SpreadMask, Basis, SpreadBasis);
    StreamSet * const SpreadTails = P.CreateStreamSet(plan.tailBits);
    SpreadByMask(P, SpreadMask, Inserts, SpreadTails, plan.insertBits);
    if (Positions) Positions->insert(SpreadMask, PositionMap::Inserted::Inherit);
    StreamSet * const Result = P.CreateStreamSet(8);
    P.CreateKernelCall<UTF8_CharMapTails>(SpreadBasis, SpreadTails, plan, Result);
    SHOW_BIXNUM(Result);
    return Result;
}

}

StreamSet * UTF8_CharMapPipeline(PipelineBuilder & P, const UTF8_CharMap & map, StreamSet * Basis,
                                 const UCD::UnicodeSet & contextual, StreamSet * Applies, StreamSet * Within,
                                 PositionMap * Positions) {
    if (map.empty()) return Basis;
    // Each character mapped: the changes of its bytes in place, the number of
    // bytes to delete, and the tail inserted.
    struct Change {
        UCD::codepoint_t cp;
        unsigned remove = 0;
        bool empty = false;
        unsigned tail = 0;
        std::vector<unsigned char> bytes;
    };
    Plan plan;
    std::map<std::string, unsigned> tailNumber;
    std::vector<Change> changes;
    UCD::UnicodeSet mapped;
    unsigned maxDelete = 0, maxTail = 0;
    for (const auto & m : map) {
        const std::string r = utf8(m.second);
        for (const auto & range : m.first) {
            for (UCD::codepoint_t cp = range.first; cp <= range.second; cp++) {
                const std::string s = utf8(std::u32string(1, static_cast<char32_t>(cp)));
                Change c;
                c.cp = cp;
                if (r.empty()) {
                    c.empty = true;
                    plan.empty = true;
                } else {
                    const size_t common = std::min(r.size(), s.size());
                    for (size_t j = 0; j < common; j++) c.bytes.push_back(static_cast<unsigned char>(s[j] ^ r[j]));
                    c.remove = static_cast<unsigned>(s.size() - common);
                    if (r.size() > common) {
                        const std::string tail = r.substr(common);
                        auto f = tailNumber.emplace(tail, static_cast<unsigned>(plan.tails.size() + 1));
                        if (f.second) plan.tails.push_back(tail);
                        c.tail = f.first->second;
                        maxTail = std::max(maxTail, static_cast<unsigned>(tail.size()));
                    }
                }
                maxDelete = std::max(maxDelete, c.remove);
                plan.changeBytes = std::max(plan.changeBytes, static_cast<unsigned>(c.bytes.size()));
                changes.push_back(std::move(c));
            }
        }
        mapped = mapped + m.first;
    }
    plan.deleteBits = bitsFor(maxDelete);
    plan.tailBits = bitsFor(static_cast<unsigned>(plan.tails.size()));
    plan.insertBits = bitsFor(maxTail);
    std::vector<UCD::UnicodeSet> sets(plan.count());
    sets[0] = mapped;
    for (const Change & c : changes) {
        for (unsigned j = 0; j < c.bytes.size(); j++) {
            for (unsigned b = 0; b < 8; b++) if ((c.bytes[j] >> b) & 1) sets[plan.changeIndex(j, b)].insert(c.cp);
        }
        for (unsigned k = 0; k < plan.deleteBits; k++) if ((c.remove >> k) & 1) sets[plan.deleteIndex() + k].insert(c.cp);
        if (c.empty) sets[plan.emptyIndex()].insert(c.cp);
        for (unsigned k = 0; k < plan.tailBits; k++) if ((c.tail >> k) & 1) sets[plan.tailIndex() + k].insert(c.cp);
    }
    // (The contextual characters, the last class.)
    if (Applies) sets.push_back(contextual);
    std::vector<re::CC *> ccs;
    for (const UCD::UnicodeSet & s : sets) ccs.push_back(re::makeCC(s, &cc::Unicode));
    StreamSet * const Classes = P.CreateStreamSet(ccs.size());
    P.CreateKernelCall<CharClassesKernel>(ccs, Basis, Classes, BitMovementMode::LookAhead);
    SHOW_BIXNUM(Classes);
    // The characters changed in place; the positions kept, and the
    // insertions, where bytes are deleted or inserted.
    const bool deletes = (maxDelete > 0) || plan.empty;
    const bool inserts = !plan.tails.empty();
    StreamSet * const Translated = P.CreateStreamSet(8);
    StreamSet * const Keep = deletes ? P.CreateStreamSet(1) : nullptr;
    StreamSet * Inserts = inserts ? P.CreateStreamSet(plan.insertBits + plan.tailBits) : nullptr;
    P.CreateKernelCall<UTF8_CharMapApply>(Basis, Classes, Applies, Within, plan, Translated, Keep, Inserts);
    return finishMap(P, plan, Translated, Keep, Inserts, deletes, inserts, Positions);
}

StreamSet * UTF8_SelectorPipeline(PipelineBuilder & P, const std::vector<std::u32string> & strings, StreamSet * Basis,
                                  StreamSet * Selectors, PositionMap * Positions) {
    Plan plan;
    std::vector<std::string> bytes;
    std::vector<unsigned> tailOf;
    std::map<std::string, unsigned> tailNumber;
    unsigned maxTail = 0;
    bool deletes = false;
    UCD::UnicodeSet kept;
    //  (A string with a prefix before the mark: the prefix inserted first, and
    //  the rest applied as a string that keeps the character.)
    PrefixPlan prefixes;
    std::map<std::string, unsigned> prefixNumber;
    for (const std::u32string & full : strings) {
        std::u32string str = full;
        unsigned pn = 0;
        const size_t at = full.find(UTF8_SelectorKept);
        if (at != std::u32string::npos && at > 0) {
            const std::string p = utf8(full.substr(0, at));
            auto f = prefixNumber.emplace(p, static_cast<unsigned>(prefixes.prefixes.size() + 1));
            if (f.second) prefixes.prefixes.push_back(p);
            pn = f.first->second;
            prefixes.countBits = std::max(prefixes.countBits, bitsFor(static_cast<unsigned>(p.size())));
            str = full.substr(at);
        }
        prefixes.prefixOf.push_back(pn);
        bytes.push_back(utf8(str));
        const std::string & b = bytes.back();
        unsigned t = 0;
        //  (The tail: the string but its first byte, or but the mark of a kept character.)
        const size_t skip = keptString(b) ? KeptPrefix.size() : 1;
        if (skip > 1) kept.insert(static_cast<UCD::codepoint_t>(bytes.size()));
        if (b.empty()) {
            deletes = true;
        } else if (b.size() > skip) {
            auto f = tailNumber.emplace(b.substr(skip), static_cast<unsigned>(plan.tails.size() + 1));
            if (f.second) plan.tails.push_back(b.substr(skip));
            t = f.first->second;
            maxTail = std::max(maxTail, static_cast<unsigned>(b.size() - skip));
        }
        tailOf.push_back(t);
    }
    plan.tailBits = bitsFor(static_cast<unsigned>(plan.tails.size()));
    plan.insertBits = bitsFor(maxTail);
    if (!prefixes.prefixes.empty()) {
        prefixes.numberBits = bitsFor(static_cast<unsigned>(prefixes.prefixes.size()));
        Basis = insertPrefixes(P, prefixes, Basis, Selectors, Positions);
    }
    // The bytes of the characters with a selector (marked at their last bytes).
    StreamSet * const Index = P.CreateStreamSet(1);
    P.CreateKernelCall<UTF8_index>(Basis, Index);
    StreamSet * const Changed = P.CreateStreamSet(1);
    P.CreateKernelCall<SelectorsPresent>(Selectors, kept, Changed);
    StreamSet * const Spans = P.CreateStreamSet(1);
    P.CreateKernelCall<U8Spans>(Changed, Index, Spans, BitMovementMode::LookAhead);
    // (Bytes are deleted wherever a multi-byte character has a selector, or a string is empty.)
    deletes = true;
    const bool inserts = !plan.tails.empty();
    StreamSet * const Translated = P.CreateStreamSet(8);
    StreamSet * const Keep = deletes ? P.CreateStreamSet(1) : nullptr;
    StreamSet * const Inserts = inserts ? P.CreateStreamSet(plan.insertBits + plan.tailBits) : nullptr;
    P.CreateKernelCall<UTF8_SelectorApply>(Basis, Selectors, Spans, bytes, tailOf, plan, Translated, Keep, Inserts);
    return finishMap(P, plan, Translated, Keep, Inserts, deletes, inserts, Positions);
}

}

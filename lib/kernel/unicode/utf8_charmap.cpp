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

//  The positions where bits (a number at each position) give the number t.
PabloAST * numberAt(PabloBuilder & pb, const std::vector<PabloAST *> & bits, unsigned t) {
    PabloAST * is = pb.createOnes();
    for (unsigned k = 0; k < bits.size(); k++) is = pb.createAnd(is, ((t >> k) & 1) ? bits[k] : pb.createNot(bits[k]));
    return is;
}

//  The handling of ill-formed code units (see codegen::UTF8Errors), by the
//  first character map of a text: none (the text is well-formed, or the code
//  units are copied), each maximal ill-formed subpart replaced by U+FFFD, or
//  deleted.
enum class Errors {None, Replace, Drop};

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
//  An ill-formed subpart replaced by U+FFFD (EF BF BD) keeps its first byte,
//  set to EF, with the tail BF BD (the last tail).
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
    UTF8_CharMapApply(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Classes, Plan plan, Errors errors,
                      StreamSet * Translated, StreamSet * Keep, StreamSet * Inserts)
    : PabloKernel(ts, "UTF8_CharMapApply_" + getStringHash(plan.signature()) +
                      (errors == Errors::Replace ? "_r" : errors == Errors::Drop ? "_d" : "") + (Keep ? "_k" : "") + (Inserts ? "_i" : ""),
                  {Binding{"basis", Basis, FixedRate(1), LookAhead(3)}, Binding{"classes", Classes}},
                  [&] {
                      Bindings out{Binding{"translated", Translated}};
                      if (Keep) out.emplace_back("keep", Keep);
                      if (Inserts) out.emplace_back("inserts", Inserts);
                      return out;
                  }()),
      mPlan(std::move(plan)), mErrors(errors), mKeep(Keep != nullptr), mInserts(Inserts != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> basis = getInputStreamSet("basis");
        const std::vector<PabloAST *> classes = getInputStreamSet("classes");
        PabloAST * const mapped = classes[0];
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
        if (mErrors != Errors::None) {
            // The maximal ill-formed subparts (of at most 3 bytes), from their
            // first bytes to their last.
            const UTF8_Structure u = UTF8_structure(pb, basis, true);
            PabloAST * const second = pb.createAdvance(pb.createAnd(u.errorInitial, pb.createNot(u.errorFinal)), 1);
            PabloAST * const third = pb.createAdvance(pb.createAnd(second, pb.createNot(u.errorFinal)), 1);
            deleted = pb.createOr3(deleted, second, third);
            if (mErrors == Errors::Drop) {
                deleted = pb.createOr(deleted, u.errorInitial);
            } else {
                for (unsigned b = 0; b < 8; b++) {
                    translated[b] = ((0xEF >> b) & 1) ? pb.createOr(translated[b], u.errorInitial)
                                                      : pb.createAnd(translated[b], pb.createNot(u.errorInitial));
                }
                const unsigned last = static_cast<unsigned>(mPlan.tails.size());     // (BF BD)
                for (unsigned k = 0; k < mPlan.tailBits; k++) {
                    if ((last >> k) & 1) tail[k] = pb.createOr(tail[k], u.errorInitial);
                }
            }
        }
        writeOutputStreamSet("translated", translated);
        if (mKeep) writeOutputStreamSet("keep", std::vector<PabloAST *>{pb.createInFile(pb.createNot(deleted))});
        if (mInserts) {
            // The lengths of the tails, by their numbers.
            std::vector<PabloAST *> inserts(mPlan.insertBits, pb.createZeroes());
            for (unsigned t = 1; t <= mPlan.tails.size(); t++) {
                PabloAST * const is = numberAt(pb, tail, t);
                const unsigned length = static_cast<unsigned>(mPlan.tails[t - 1].size());
                for (unsigned k = 0; k < mPlan.insertBits; k++) if ((length >> k) & 1) inserts[k] = pb.createOr(inserts[k], is);
            }
            inserts.insert(inserts.end(), tail.begin(), tail.end());
            writeOutputStreamSet("inserts", inserts);
        }
    }
private:
    const Plan mPlan;
    const Errors mErrors;
    const bool mKeep;
    const bool mInserts;
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
        for (unsigned t = 1; t <= mPlan.tails.size(); t++) {
            PabloAST * const at = numberAt(pb, tails, t);
            const std::string & tail = mPlan.tails[t - 1];
            for (unsigned i = 0; i < tail.size(); i++) {
                PabloAST * const pos = pb.createAdvance(at, i + 1);
                for (unsigned b = 0; b < 8; b++) {
                    if ((static_cast<unsigned char>(tail[i]) >> b) & 1) result[b] = pb.createOr(result[b], pos);
                }
            }
        }
        writeOutputStreamSet("result", result);
    }
private:
    const Plan mPlan;
};

}

StreamSet * UTF8_CharMapPipeline(PipelineBuilder & P, const UTF8_CharMap & map, StreamSet * Basis, bool wellFormed) {
    const Errors errors = (wellFormed || (codegen::UTF8Errors == codegen::UTF8ErrorMode::Abort)) ? Errors::None
                          : (codegen::UTF8Errors == codegen::UTF8ErrorMode::DropBadInput) ? Errors::Drop : Errors::Replace;
    if (map.empty() && (errors == Errors::None)) return Basis;
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
    if (errors == Errors::Replace) {
        plan.tails.push_back("\xBF\xBD");     // (U+FFFD after EF, see Plan)
        maxTail = std::max(maxTail, 2u);
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
    std::vector<re::CC *> ccs;
    for (const UCD::UnicodeSet & s : sets) ccs.push_back(re::makeCC(s, &cc::Unicode));
    StreamSet * const Classes = P.CreateStreamSet(ccs.size());
    P.CreateKernelCall<CharClassesKernel>(ccs, Basis, Classes, BitMovementMode::LookAhead);
    SHOW_BIXNUM(Classes);
    // The characters changed in place; the positions kept, and the
    // insertions, where bytes are deleted or inserted.
    const bool deletes = (maxDelete > 0) || plan.empty || (errors != Errors::None);
    const bool inserts = !plan.tails.empty();
    StreamSet * const Translated = P.CreateStreamSet(8);
    StreamSet * const Keep = deletes ? P.CreateStreamSet(1) : nullptr;
    StreamSet * Inserts = inserts ? P.CreateStreamSet(plan.insertBits + plan.tailBits) : nullptr;
    P.CreateKernelCall<UTF8_CharMapApply>(Basis, Classes, plan, errors, Translated, Keep, Inserts);
    SHOW_BIXNUM(Translated);
    Basis = Translated;
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
    StreamSet * const Result = P.CreateStreamSet(8);
    P.CreateKernelCall<UTF8_CharMapTails>(SpreadBasis, SpreadTails, plan, Result);
    SHOW_BIXNUM(Result);
    return Result;
}

}

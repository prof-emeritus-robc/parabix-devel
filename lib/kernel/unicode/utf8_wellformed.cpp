/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/utf8_wellformed.h>

#include <kernel/bitwise/bixlogic.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/streamutils/until_n.h>
#include <kernel/unicode/utf8_support.h>
#include <pablo/pablo.h>
#include <toolchain/fileutil.h>
#include <unistd.h>

using namespace pablo;

namespace kernel {

namespace {


//  The maximal ill-formed subparts (of at most 3 bytes, from their first bytes
//  to their last, see UTF8_structure): Keep, the positions kept (all but the
//  bytes of each subpart after the first, and the first too if dropped); with
//  replacement characters, Repaired, the basis with the first byte of each
//  subpart set to EF, and Inserts, the numbers of positions to insert after
//  each position (2, after the first byte of a subpart: zero, then the first
//  bytes; the second stream, the first bytes, also marks where BF BD go).
class UTF8_RepairMarks final : public PabloKernel {
public:
    UTF8_RepairMarks(LLVMTypeSystemInterface & ts, StreamSet * Basis, bool replace,
                     StreamSet * Keep, StreamSet * Repaired, StreamSet * Inserts)
    : PabloKernel(ts, std::string("UTF8_RepairMarks") + (replace ? "_r" : "_d"),
                  {Binding{"basis", Basis, FixedRate(1), LookAhead(3)}},
                  [&] {
                      Bindings out{Binding{"keep", Keep}};
                      if (replace) {
                          out.emplace_back("repaired", Repaired);
                          out.emplace_back("inserts", Inserts);
                      }
                      return out;
                  }()),
      mReplace(replace) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("basis");
        const UTF8_Structure u = UTF8_structure(pb, basis, true);
        PabloAST * const second = pb.createAdvance(pb.createAnd(u.errorInitial, pb.createNot(u.errorFinal)), 1);
        PabloAST * const third = pb.createAdvance(pb.createAnd(second, pb.createNot(u.errorFinal)), 1);
        PabloAST * deleted = pb.createOr(second, third);
        if (mReplace) {
            for (unsigned b = 0; b < 8; b++) {
                basis[b] = ((0xEF >> b) & 1) ? pb.createOr(basis[b], u.errorInitial)
                                             : pb.createAnd(basis[b], pb.createNot(u.errorInitial));
            }
            writeOutputStreamSet("repaired", basis);
            writeOutputStreamSet("inserts", std::vector<PabloAST *>{pb.createZeroes(), u.errorInitial});
        } else {
            deleted = pb.createOr(deleted, u.errorInitial);
        }
        writeOutputStreamSet("keep", std::vector<PabloAST *>{pb.createInFile(pb.createNot(deleted))});
    }
private:
    const bool mReplace;
};

//  BF BD written at the two positions inserted (zeroes) after each first byte
//  of an ill-formed subpart, set to EF (Firsts).
class UTF8_RepairFill final : public PabloKernel {
public:
    UTF8_RepairFill(LLVMTypeSystemInterface & ts, StreamSet * Basis, StreamSet * Firsts, StreamSet * Result)
    : PabloKernel(ts, "UTF8_RepairFill", {Binding{"basis", Basis}, Binding{"firsts", Firsts}}, {Binding{"result", Result}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> result = getInputStreamSet("basis");
        PabloAST * const first = getInputStreamSet("firsts")[0];
        PabloAST * const at1 = pb.createAdvance(first, 1);
        PabloAST * const at2 = pb.createAdvance(first, 2);
        for (unsigned b = 0; b < 8; b++) {
            if ((0xBF >> b) & 1) result[b] = pb.createOr(result[b], at1);
            if ((0xBD >> b) & 1) result[b] = pb.createOr(result[b], at2);
        }
        writeOutputStreamSet("result", result);
    }
};

}

StreamSet * WellFormedUTF8(PipelineBuilder & P, StreamSet * Basis, codegen::UTF8ErrorMode mode) {
    StreamSet * const Result = P.CreateStreamSet(8);
    WellFormedUTF8(P, Basis, Result, mode);
    return Result;
}

void WellFormedUTF8(PipelineBuilder & P, StreamSet * Basis, StreamSet * Result, codegen::UTF8ErrorMode mode) {
    if (mode == codegen::UTF8ErrorMode::Abort) {
        // The text before the first error: the errors truncated at the first
        // (the pipeline then stops), and the bytes before it kept.
        StreamSet * const errors = P.CreateStreamSet(1);
        P.CreateKernelCall<UTF8_ErrorMarks>(Basis, errors);
        StreamSet * const first = P.CreateTruncatedStreamSet(errors);
        P.CreateKernelCall<UntilNkernel>(P.CreateConstant(P.getSize(1)), errors, first,
                                         UntilNkernel::Mode::ReportAcceptedLengthAtAndBeforeN);
        StreamSet * const keep = P.CreateStreamSet(1);
        Invert(P, first, keep);
        FilterByMask(P, keep, Basis, Result);
        return;
    }
    const bool replace = (mode == codegen::UTF8ErrorMode::UseReplacementChar);
    StreamSet * const Keep = P.CreateStreamSet(1);
    StreamSet * const Repaired = replace ? P.CreateStreamSet(8) : nullptr;
    StreamSet * const Inserts = replace ? P.CreateStreamSet(2) : nullptr;
    P.CreateKernelCall<UTF8_RepairMarks>(Basis, replace, Keep, Repaired, Inserts);
    if (!replace) {
        FilterByMask(P, Keep, Basis, Result);
        return;
    }
    StreamSet * const Kept = P.CreateStreamSet(8);
    FilterByMask(P, Keep, Repaired, Kept);
    StreamSet * const KeptInserts = P.CreateStreamSet(2);
    FilterByMask(P, Keep, Inserts, KeptInserts);
    StreamSet * const SpreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, KeptInserts, SpreadMask, InsertPosition::After);
    StreamSet * const Spread = P.CreateStreamSet(8);
    SpreadByMask(P, SpreadMask, Kept, Spread);
    StreamSet * const Firsts = P.CreateStreamSet(1);
    SpreadByMask(P, SpreadMask, KeptInserts, Firsts, 1);
    P.CreateKernelCall<UTF8_RepairFill>(Spread, Firsts, Result);
}

// The offset of the first byte of the first maximal ill-formed subpart of
// UTF-8 text (Table 3-7 of the Unicode Standard), or its length.
size_t UTF8_ErrorOffset(const char * text, size_t length) {
    const unsigned char * const s = reinterpret_cast<const unsigned char *>(text);
    size_t i = 0;
    while (i < length) {
        const unsigned c = s[i];
        unsigned n = 0;                 // the suffixes that follow the lead
        unsigned lo = 0x80, hi = 0xBF;  // the range of the first suffix
        if (c < 0x80) { i++; continue; }
        else if ((c >= 0xC2) && (c <= 0xDF)) n = 1;
        else if (c == 0xE0) { n = 2; lo = 0xA0; }
        else if (c == 0xED) { n = 2; hi = 0x9F; }
        else if ((c >= 0xE1) && (c <= 0xEF)) n = 2;
        else if (c == 0xF0) { n = 3; lo = 0x90; }
        else if (c == 0xF4) { n = 3; hi = 0x8F; }
        else if ((c >= 0xF1) && (c <= 0xF3)) n = 3;
        else return i;
        if (i + n >= length) return i;
        if ((s[i + 1] < lo) || (s[i + 1] > hi)) return i;
        for (unsigned k = 2; k <= n; k++) {
            if ((s[i + k] < 0x80) || (s[i + k] > 0xBF)) return i;
        }
        i += n + 1;
    }
    return length;
}

bool UTF8_FileError(int fd, bool useMMap, size_t & offset) {
    if (lseek(fd, 0, SEEK_SET) != 0) return false;
    AlignedFileBuffer buf;
    if (!buf.load(fd, useMMap)) return false;
    offset = UTF8_ErrorOffset(buf.getBuf(), buf.getBufSize());
    const bool error = offset < buf.getBufSize();
    buf.release();
    return error;
}

}

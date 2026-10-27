/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/utf8_support.h>
#include <pablo/pablo.h>
#include <re/cc/cc_compiler.h>         // for CC_Compiler
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_kernel.h>
#include <llvm/Support/ErrorHandling.h>

using namespace kernel;
using namespace pablo;
using namespace re;
using namespace llvm;

namespace {
// A compiler of byte classes for the code units of the source, looked ahead
// by k positions.
std::unique_ptr<cc::CC_Compiler> lookaheadCompiler(PabloBuilder & pb, const std::vector<PabloAST *> & source, unsigned k) {
    if (source.size() != 8) {
        // (A lookahead of a byte stream is not compiled correctly.)
        llvm::report_fatal_error("UTF8_structure: the source must be the 8 basis bit streams of the UTF-8 code units");
    }
    std::vector<PabloAST *> ahead;
    for (PabloAST * s : source) ahead.push_back(k == 0 ? s : pb.createLookahead(s, k));
    return std::make_unique<cc::Parabix_CC_Compiler_Builder>(ahead);
}

}

//
// The well-formed sequences are those of Table 3-7 of the Unicode Standard.
// As blocks of text are mostly ASCII, the sequences are found within an
// if-hierarchy: only blocks with prefix bytes (C0-FF), and within them only
// those with 2-, 3- or 4-byte leads, compute them.  A lead or a suffix
// before the last is continued (nonFinal) where the next byte is the next
// byte of a well-formed sequence (the byte classes looked ahead by 1); the
// last byte of each well-formed character (ASCII, or the last suffix of a
// continued sequence) is u8valid.  Every other byte ends a maximal ill-formed
// subpart: a byte of a sequence not continued, and a byte (other than ASCII)
// that neither begins nor continues a sequence.
//
UTF8_Structure kernel::UTF8_structure(PabloBuilder & pb, const std::vector<PabloAST *> & source, bool initials) {
    std::unique_ptr<cc::CC_Compiler> ccc = lookaheadCompiler(pb, source, 0);
    Zeroes * const ZEROES = pb.createZeroes();
    PabloAST * const ASCII = pb.createNot(source[7], "ASCII");
    Var * const nonFinal = pb.createVar("nonFinal", ZEROES);
    Var * const u8valid = pb.createVar("u8valid", ASCII);
    // The legal leads, and the suffixes that continue them.
    Var * const legalLeads = pb.createVar("legalLeads", ZEROES);
    Var * const u8cont = pb.createVar("u8cont", ZEROES);
    Var * const validInitial = initials ? pb.createVar("validInitial", ASCII) : nullptr;

    PabloAST * const u8pfx = ccc->compileCC(makeByte(0xC0, 0xFF), pb);
    auto it = pb.createScope();
    pb.createIf(u8pfx, it);
    std::unique_ptr<cc::CC_Compiler> ccc1 = lookaheadCompiler(it, source, 1);
    std::unique_ptr<cc::CC_Compiler> ccc2 = initials ? lookaheadCompiler(it, source, 2) : nullptr;
    std::unique_ptr<cc::CC_Compiler> ccc3 = initials ? lookaheadCompiler(it, source, 3) : nullptr;
    auto byte = [&](cc::CC_Compiler & c, unsigned lo, unsigned hi, PabloBuilder & b) {
        return c.compileCC(makeByte(lo, hi), b);
    };
    PabloAST * const u8pfx2 = byte(*ccc, 0xC2, 0xDF, it);
    PabloAST * const u8pfx3 = byte(*ccc, 0xE0, 0xEF, it);
    PabloAST * const u8pfx4 = byte(*ccc, 0xF0, 0xF4, it);
    it.createAssign(legalLeads, it.createOr3(u8pfx2, u8pfx3, u8pfx4));
    auto accumulate = [&](PabloBuilder & b, Var * v, PabloAST * x) { b.createAssign(v, b.createOr(v, x)); };

    //
    // Two-byte sequences
    auto it2 = it.createScope();
    it.createIf(u8pfx2, it2);
    PabloAST * const suffix1_2 = byte(*ccc1, 0x80, 0xBF, it2);
    PabloAST * const lead2 = it2.createAnd(u8pfx2, suffix1_2, "lead2");
    PabloAST * const u8scope22 = it2.createAdvance(lead2, 1, "u8scope22");
    accumulate(it2, nonFinal, lead2);
    accumulate(it2, u8valid, u8scope22);
    accumulate(it2, u8cont, u8scope22);
    if (initials) accumulate(it2, validInitial, lead2);

    //
    // Three-byte sequences: E0 A0-BF, E1-EC EE-EF 80-BF, ED 80-9F, then 80-BF.
    auto it3 = it.createScope();
    it.createIf(u8pfx3, it3);
    PabloAST * const suffix1_3 = byte(*ccc1, 0x80, 0xBF, it3);
    PabloAST * const E0 = it3.createAnd(byte(*ccc, 0xE0, 0xE0, it3), byte(*ccc1, 0xA0, 0xBF, it3));
    PabloAST * const ED = it3.createAnd(byte(*ccc, 0xED, 0xED, it3), byte(*ccc1, 0x80, 0x9F, it3));
    PabloAST * const EX = it3.createAnd(it3.createOr(byte(*ccc, 0xE1, 0xEC, it3), byte(*ccc, 0xEE, 0xEF, it3)), suffix1_3);
    PabloAST * const lead3 = it3.createOr3(E0, ED, EX, "lead3");
    PabloAST * const u8scope32 = it3.createAdvance(lead3, 1, "u8scope32");
    PabloAST * const mid32 = it3.createAnd(u8scope32, suffix1_3);
    PabloAST * const u8scope33 = it3.createAdvance(mid32, 1, "u8scope33");
    accumulate(it3, nonFinal, it3.createOr(lead3, mid32));
    accumulate(it3, u8valid, u8scope33);
    accumulate(it3, u8cont, it3.createOr(u8scope32, u8scope33));
    if (initials) accumulate(it3, validInitial, it3.createAnd(lead3, byte(*ccc2, 0x80, 0xBF, it3)));

    //
    // Four-byte sequences: F0 90-BF, F1-F3 80-BF, F4 80-8F, then 80-BF 80-BF.
    auto it4 = it.createScope();
    it.createIf(u8pfx4, it4);
    PabloAST * const suffix1_4 = byte(*ccc1, 0x80, 0xBF, it4);
    PabloAST * const F0 = it4.createAnd(byte(*ccc, 0xF0, 0xF0, it4), byte(*ccc1, 0x90, 0xBF, it4));
    PabloAST * const F4 = it4.createAnd(byte(*ccc, 0xF4, 0xF4, it4), byte(*ccc1, 0x80, 0x8F, it4));
    PabloAST * const FX = it4.createAnd(byte(*ccc, 0xF1, 0xF3, it4), suffix1_4);
    PabloAST * const lead4 = it4.createOr3(F0, F4, FX, "lead4");
    PabloAST * const u8scope42 = it4.createAdvance(lead4, 1, "u8scope42");
    PabloAST * const mid42 = it4.createAnd(u8scope42, suffix1_4);
    PabloAST * const u8scope43 = it4.createAdvance(mid42, 1, "u8scope43");
    PabloAST * const mid43 = it4.createAnd(u8scope43, suffix1_4);
    PabloAST * const u8scope44 = it4.createAdvance(mid43, 1, "u8scope44");
    accumulate(it4, nonFinal, it4.createOr3(lead4, mid42, mid43));
    accumulate(it4, u8valid, u8scope44);
    accumulate(it4, u8cont, it4.createOr3(u8scope42, u8scope43, u8scope44));
    if (initials) {
        accumulate(it4, validInitial, it4.createAnd3(lead4, byte(*ccc2, 0x80, 0xBF, it4), byte(*ccc3, 0x80, 0xBF, it4)));
    }

    UTF8_Structure u;
    u.validFinal = pb.createInFile(u8valid, "u8validFinal");
    u.errorFinal = pb.createInFile(pb.createNot(pb.createOr(nonFinal, u8valid)), "u8errorFinal");
    if (initials) {
        // A byte (other than ASCII) that neither begins nor continues a sequence.
        PabloAST * const lone = pb.createAnd(source[7], pb.createNot(pb.createOr(legalLeads, u8cont)), "u8lone");
        u.validInitial = pb.createInFile(validInitial, "u8validInitial");
        u.errorInitial = pb.createInFile(pb.createOr(pb.createAnd(legalLeads, pb.createNot(validInitial)), lone),
                                         "u8errorInitial");
    }
    return u;
}

void UTF8_index::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const UTF8_Structure u = UTF8_structure(pb, getInputStreamSet("source"));
    PabloAST * u8final = u.validFinal;
    if (mMode != codegen::UTF8ErrorMode::DropBadInput) {
        u8final = pb.createOr(u8final, u.errorFinal);
    }
    Var * const u8index = getOutputStreamVar("u8index");
    if (getNumOfStreamInputs() > 1) {
        u8final = pb.createOr(u8final, getInputStreamSet("u8_LB")[0]);
    }
    pb.createAssign(pb.createExtract(u8index, pb.getInteger(0)), u8final);
}

static std::string utf8ErrorAnnotation(codegen::UTF8ErrorMode mode) {
    return (mode == codegen::UTF8ErrorMode::DropBadInput) ? "_drop" : "";
}

UTF8_index::UTF8_index(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * u8index, StreamSet * u8_LB,
                       codegen::UTF8ErrorMode mode)
: PabloKernel(ts, [&]() -> std::string {
    std::stringstream s;
    s << "UTF8_index_";
    s << Source->getNumElements() << "x" << Source->getFieldWidth();
    if (u8_LB) {
        s << "_LB";
    }
    s << utf8ErrorAnnotation(mode);
    return s.str();}(),
{}, {}), mMode(mode) {
    if (u8_LB) {
        // A line-break stream produced with UnterminatedLineAtEOF::Add1 has one
        // extra position (AddCarry) exactly when the final line is unterminated.
        // u8final copies that bit, so u8index is extended exactly when u8_LB is
        // (as in UnicodeLinesKernelBuilder); otherwise filtering the line breaks
        // by this index drops it and the final line is never matched.
        mInputStreamSets.push_back(Binding{"source", Source, FixedRate(), {ZeroExtended(), LookAhead(1)}});
        mInputStreamSets.push_back(Binding{"u8_LB", u8_LB, FixedRate(), Principal()});
        mOutputStreamSets.push_back(Binding{"u8index", u8index, FixedRate(), AddCarry()});
    } else {
        mInputStreamSets.push_back(Binding{"source", Source, FixedRate(), LookAhead(1)});
        mOutputStreamSets.push_back(Binding{"u8index", u8index});
    }
}

UTF8_InitialMask::UTF8_InitialMask(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * initial,
                                   codegen::UTF8ErrorMode mode)
: PabloKernel(ts, "UTF8_InitialMask_" + Source->shapeString() + utf8ErrorAnnotation(mode),
              {Binding{"source", Source, FixedRate(), LookAhead(3)}}, {Binding{"initial", initial}}),
  mMode(mode) {}

void UTF8_InitialMask::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const UTF8_Structure u = UTF8_structure(pb, getInputStreamSet("source"), true);
    PabloAST * initial = u.validInitial;
    if (mMode != codegen::UTF8ErrorMode::DropBadInput) {
        initial = pb.createOr(initial, u.errorInitial);
    }
    pb.createAssign(pb.createExtract(getOutputStreamVar("initial"), pb.getInteger(0)), initial);
}

UTF8_ErrorMarks::UTF8_ErrorMarks(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * errors,
                                 pablo::BitMovementMode m)
: PabloKernel(ts, "UTF8_ErrorMarks_" + Source->shapeString() + pablo::BitMovementMode_string(m),
              {Binding{"source", Source, FixedRate(), LookAhead(m == pablo::BitMovementMode::LookAhead ? 3 : 1)}},
              {Binding{"errors", errors}}),
  mBitMovement(m) {}

void UTF8_ErrorMarks::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    const bool initials = (mBitMovement == pablo::BitMovementMode::LookAhead);
    const UTF8_Structure u = UTF8_structure(pb, getInputStreamSet("source"), initials);
    pb.createAssign(pb.createExtract(getOutputStreamVar("errors"), pb.getInteger(0)),
                    initials ? u.errorInitial : u.errorFinal);
}

U8Spans::U8Spans(LLVMTypeSystemInterface & ts, StreamSet * marks, StreamSet * u8index, StreamSet * spans, pablo::BitMovementMode m)
: PabloKernel(ts, "U8Spans_" + marks->shapeString() + pablo::BitMovementMode_string(m), {}, {Binding{"spans", spans}}),
    mBitMovement(m) {
        if (m == pablo::BitMovementMode::LookAhead) {
            mInputStreamSets.push_back(Binding{"marks", marks, FixedRate(1), LookAhead(3)});
            mInputStreamSets.push_back(Binding{"u8index", u8index, FixedRate(1), LookAhead(3)});
        } else {
            mInputStreamSets.push_back(Binding{"marks", marks});
            mInputStreamSets.push_back(Binding{"u8index", u8index});
        }

    }

void U8Spans::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> marks = getInputStreamSet("marks");
    PabloAST * u8index = getInputStreamSet("u8index")[0];
    std::vector<PabloAST *> spans(marks.size());
    if (mBitMovement == BitMovementMode::LookAhead) {
        PabloAST * back1InSpan = pb.createNot(u8index);
        PabloAST * ix_or_next = pb.createOr(u8index, pb.createLookahead(u8index, 1));
        PabloAST * back2InSpan = pb.createNot(ix_or_next);
        PabloAST * ix_or_next2 = pb.createOr(ix_or_next, pb.createLookahead(u8index, 2));
        PabloAST * back3InSpan = pb.createNot(ix_or_next2);
        for (unsigned i = 0; i < marks.size(); i++) {
            PabloAST * back1 = pb.createAnd(pb.createLookahead(marks[i], 1), back1InSpan);
            PabloAST * back2 = pb.createAnd(pb.createLookahead(marks[i], 2), back2InSpan);
            PabloAST * back3 = pb.createAnd(pb.createLookahead(marks[i], 3), back3InSpan);
            spans[i] = pb.createOr(marks[i], pb.createOr3(back1, back2, back3));
        }
    } else {
        for (unsigned i = 0; i < marks.size(); i++) {
            spans[i] = pb.createMatchStar(marks[i], pb.createNot(u8index));
        }
    }
    writeOutputStreamSet("spans", spans);
}

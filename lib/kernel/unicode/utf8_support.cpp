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

// The classes of a byte for the well-formed sequences (Table 3-7 of the
// Unicode Standard), for the code units of a compiler.
struct UTF8_ByteClasses {
    PabloAST * ASCII;
    PabloAST * S;                       // 80-BF
    PabloAST * L2;                      // C2-DF
    PabloAST * L3a, * L3b, * L3c;       // E0, E1-EC EE-EF, ED
    PabloAST * L4a, * L4b, * L4c;       // F0, F1-F3, F4
    PabloAST * A0_BF, * x80_9F, * x90_BF, * x80_8F;
};

UTF8_ByteClasses byteClasses(PabloBuilder & pb, cc::CC_Compiler & ccc) {
    auto byte = [&](unsigned lo, unsigned hi) { return ccc.compileCC(makeByte(lo, hi), pb); };
    UTF8_ByteClasses b;
    b.ASCII = byte(0x00, 0x7F);
    b.S = byte(0x80, 0xBF);
    b.L2 = byte(0xC2, 0xDF);
    b.L3a = byte(0xE0, 0xE0);
    b.L3b = pb.createOr(byte(0xE1, 0xEC), byte(0xEE, 0xEF));
    b.L3c = byte(0xED, 0xED);
    b.L4a = byte(0xF0, 0xF0);
    b.L4b = byte(0xF1, 0xF3);
    b.L4c = byte(0xF4, 0xF4);
    b.A0_BF = byte(0xA0, 0xBF);
    b.x80_9F = byte(0x80, 0x9F);
    b.x90_BF = byte(0x90, 0xBF);
    b.x80_8F = byte(0x80, 0x8F);
    return b;
}

// The state of the well-formed sequences at each position: the leads, and
// the suffixes that continue a sequence (Bjk: the k-th byte of a sequence of
// j bytes).
struct UTF8_Chains {
    PabloAST * leads;
    PabloAST * L2, * L3a, * L3b, * L3c, * L4a, * L4b, * L4c;
    PabloAST * B2, * B32, * B33, * B42, * B43, * B44;
    // A sequence is complete (ASCII, or a last suffix), still incomplete
    // (pending: a lead or a suffix before the last), or continued from the
    // byte before (cont).
    PabloAST * complete(PabloBuilder & pb, PabloAST * ASCII) const {
        return pb.createOr(ASCII, pb.createOr3(B2, B33, B44));
    }
    PabloAST * pending(PabloBuilder & pb) const { return pb.createOr(leads, pb.createOr3(B32, B42, B43)); }
    PabloAST * cont(PabloBuilder & pb) const {
        return pb.createOr(pb.createOr3(B2, B32, B33), pb.createOr3(B42, B43, B44));
    }
};

// The suffixes at a position, given the chains at the position before
// (prev, aligned to this position) and the classes of its byte (b).
void continueChains(PabloBuilder & pb, const UTF8_Chains & prev, const UTF8_ByteClasses & b, UTF8_Chains & c) {
    c.B2 = pb.createAnd(prev.L2, b.S);
    c.B32 = pb.createOr3(pb.createAnd(prev.L3a, b.A0_BF), pb.createAnd(prev.L3b, b.S), pb.createAnd(prev.L3c, b.x80_9F));
    c.B33 = pb.createAnd(prev.B32, b.S);
    c.B42 = pb.createOr3(pb.createAnd(prev.L4a, b.x90_BF), pb.createAnd(prev.L4b, b.S), pb.createAnd(prev.L4c, b.x80_8F));
    c.B43 = pb.createAnd(prev.B42, b.S);
    c.B44 = pb.createAnd(prev.B43, b.S);
}

void setLeads(PabloBuilder & pb, const UTF8_ByteClasses & b, UTF8_Chains & c) {
    c.L2 = b.L2; c.L3a = b.L3a; c.L3b = b.L3b; c.L3c = b.L3c; c.L4a = b.L4a; c.L4b = b.L4b; c.L4c = b.L4c;
    c.leads = pb.createOr(pb.createOr3(b.L2, b.L3a, pb.createOr(b.L3b, b.L3c)), pb.createOr3(b.L4a, b.L4b, b.L4c));
}
}

UTF8_Structure kernel::UTF8_structure(PabloBuilder & pb, const std::vector<PabloAST *> & source, bool initials) {
    // (Only the streams of this position are advanced: an advance of a stream
    // looked ahead does not give the first position its value.)
    std::unique_ptr<cc::CC_Compiler> ccc0 = lookaheadCompiler(pb, source, 0);
    std::unique_ptr<cc::CC_Compiler> ccc1 = lookaheadCompiler(pb, source, 1);
    const UTF8_ByteClasses b0 = byteClasses(pb, *ccc0);
    const UTF8_ByteClasses b1 = byteClasses(pb, *ccc1);
    UTF8_Chains here;
    setLeads(pb, b0, here);
    UTF8_Chains prev;
    auto adv = [&](PabloAST * x) { return pb.createAdvance(x, 1); };
    prev.L2 = adv(here.L2); prev.L3a = adv(here.L3a); prev.L3b = adv(here.L3b); prev.L3c = adv(here.L3c);
    prev.L4a = adv(here.L4a); prev.L4b = adv(here.L4b); prev.L4c = adv(here.L4c);
    // B32, B42 and B43 of the position before depend on the position before
    // that, and so on: they are computed in turn.
    here.B2 = pb.createAnd(prev.L2, b0.S);
    here.B32 = pb.createOr3(pb.createAnd(prev.L3a, b0.A0_BF), pb.createAnd(prev.L3b, b0.S),
                            pb.createAnd(prev.L3c, b0.x80_9F));
    here.B42 = pb.createOr3(pb.createAnd(prev.L4a, b0.x90_BF), pb.createAnd(prev.L4b, b0.S),
                            pb.createAnd(prev.L4c, b0.x80_8F));
    here.B33 = pb.createAnd(adv(here.B32), b0.S);
    here.B43 = pb.createAnd(adv(here.B42), b0.S);
    here.B44 = pb.createAnd(adv(here.B43), b0.S);
    // The suffixes of the next position.
    UTF8_Chains next;
    continueChains(pb, here, b1, next);
    // A byte not continued by the next ends a subpart, as does a byte that
    // neither begins nor continues a sequence (and is not ASCII).
    PabloAST * const pending = here.pending(pb);
    PabloAST * const lone = pb.createNot(pb.createOr3(b0.ASCII, pending, here.cont(pb)), "u8lone");
    PabloAST * const unfinished = pb.createAnd(pending, pb.createNot(next.cont(pb)));
    UTF8_Structure u;
    u.validFinal = pb.createInFile(here.complete(pb, b0.ASCII), "u8validFinal");
    u.errorFinal = pb.createInFile(pb.createOr(unfinished, lone), "u8errorFinal");
    if (initials) {
        std::unique_ptr<cc::CC_Compiler> ccc2 = lookaheadCompiler(pb, source, 2);
        std::unique_ptr<cc::CC_Compiler> ccc3 = lookaheadCompiler(pb, source, 3);
        PabloAST * const S2 = ccc2->compileCC(makeByte(0x80, 0xBF), pb);
        PabloAST * const S3 = ccc3->compileCC(makeByte(0x80, 0xBF), pb);
        // A lead whose sequence is complete 1, 2 or 3 positions ahead.
        PabloAST * const valid = pb.createOr(pb.createOr(b0.ASCII, next.B2),
                                             pb.createOr(pb.createAnd(next.B32, S2), pb.createAnd3(next.B42, S2, S3)));
        u.validInitial = pb.createInFile(valid, "u8validInitial");
        u.errorInitial = pb.createInFile(pb.createOr(pb.createAnd(here.leads, pb.createNot(valid)), lone),
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

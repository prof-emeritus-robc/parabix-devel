/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "normalize.h"

#include <re/adt/adt.h>
#include <re/parse/parser.h>
#include <re/transforms/re_simplifier.h>
#include <re/unicode/resolve_properties.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/streamutils/deletion.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <llvm/Support/Casting.h>

using namespace kernel;
using namespace pablo;

// InvertMaskKernel
//
// Produces NOT of a single-bit stream.
// Used to turn the Mn-span mask (1=Mn byte, delete) into a
// keep mask (1=non-Mn byte, keep) for FilterByMask.

class InvertMaskKernel : public PabloKernel {
public:
    InvertMaskKernel(LLVMTypeSystemInterface & ts,
                     StreamSet * input,
                     StreamSet * output)
    : PabloKernel(ts, "InvertMask",
                  {Binding{"input",  input}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * in = getInputStreamSet("input")[0];
        writeOutputStreamSet("output", std::vector<PabloAST*>{pb.createNot(in)});
    }
};

// applyStripAccents
//
// Removes all Unicode Mn (Mark, Nonspacing) characters from the
// byte stream.  Should be called after NFD normalization so that
// combining accents are isolated codepoints.
//
// Pipeline:
//   BasisBits
//     → UTF8_index          → u8index   (last byte of each UTF-8 char)
//     → UnicodePropertyKernelBuilder(\p{Mn})
//                           → MnMask    (last byte of each Mn char)
//     → U8Spans             → MnSpans   (all bytes of each Mn char)
//     → InvertMaskKernel    → KeepMask  (1 = non-Mn byte, keep)
//     → FilterByMask        → filtered BasisBits (Mn chars removed)

static StreamSet * applyStripAccents(PipelineBuilder & P, StreamSet * BasisBits) {

    // u8index — marks the last byte of every UTF-8 character.
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    // Resolve the \p{Mn} Unicode property to a named RE.
    re::RE * mnRE = re::simplifyRE(re::RE_Parser::parse("\\p{Mn}"));
    mnRE = UCD::linkAndResolve(mnRE);
    mnRE = UCD::externalizeProperties(mnRE);
    re::Name * mnName = llvm::cast<re::Name>(mnRE);

    // MnMask — 1 at the last byte of each Mn character.
    StreamSet * MnMask = P.CreateStreamSet(1, 1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(mnName, BasisBits, MnMask);

    // MnSpans — extend Mn marks to cover ALL bytes of each Mn char.
    // U8Spans default (LookAhead mode) expects marks at the last byte,
    // which is what UnicodePropertyKernelBuilder produces.
    StreamSet * MnSpans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(MnMask, u8index, MnSpans);

    // KeepMask = NOT MnSpans.
    // FilterByMask keeps positions where mask=1, so we need 1=keep (non-Mn).
    StreamSet * KeepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertMaskKernel>(MnSpans, KeepMask);

    // Filter BasisBits — remove all bytes belonging to Mn characters.
    StreamSet * FilteredBasis = P.CreateStreamSet(8, 1);
    FilterByMask(P, KeepMask, BasisBits, FilteredBasis);

    return FilteredBasis;
}

// applyNormalization — public dispatch function
StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               NormalizationMode mode) {

    if (mode == NormStripAccents) return applyStripAccents(P, BasisBits);

    // NFC / NFD are in lib/kernel/unicode/normalization.cpp
    // if (mode == NormNFC) return applyNFC(P, BasisBits);
    // if (mode == NormNFD) return applyNFD(P, BasisBits);

    return BasisBits;  // NormNone — pass-through
}

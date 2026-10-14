/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/util/hex_convert.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/bixnum/bixnum.h>
#include <toolchain/toolchain.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <sstream>

using namespace kernel;
using namespace llvm;
using namespace pablo;

HexToBinary::HexToBinary(LLVMTypeSystemInterface & ts, StreamSet * hexStream, StreamSet * binStream)
: BlockOrientedKernel(ts, "HexToBinary",
                   {Binding{"hexdata", hexStream, FixedRate()}},
                   {Binding{"binary_data", binStream, FixedRate(4)}},
{}, {}, {}) {}

void HexToBinary::generateDoBlockMethod(KernelBuilder & b) {
    Type * i8Ty = b.getInt8Ty();
    Constant * const ZERO = b.getSize(0);
    const unsigned bytesPerBlock = b.getBitBlockWidth()/8;
    Constant * splat_0 = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, '0'));
    Constant * splat_9 = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, '9'));
    Constant * case_shift = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, 'a' - 'A'));
    Constant * splat_a = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, 'a'));
    Constant * splat_f = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, 'f'));
    Constant * af_cvt = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, 'a' - 10));

    for (unsigned i = 0; i < 4; i++) {
        Value * hexPack[2];
        hexPack[0] = b.loadInputStreamPack("hexdata", ZERO, b.getInt32(2*i));
        hexPack[1] = b.loadInputStreamPack("hexdata", ZERO, b.getInt32(2*i + 1));
        Value * base_val[2];
        for (unsigned j = 0; j < 2; j++) {
            Value * lc_hex = b.simd_or(hexPack[j], case_shift); // valid only for [A-Za-z]=>[a-z]
            Value * range_09 = b.simd_and(b.simd_ule(8, splat_0, hexPack[j]), b.simd_ule(8, hexPack[j], splat_9));
            Value * range_af = b.simd_and(b.simd_ule(8, splat_a, lc_hex), b.simd_ule(8, lc_hex, splat_f));
            base_val[j] = b.simd_or(b.simd_and(range_09, b.simd_sub(8, hexPack[j], splat_0)),
                                    b.simd_and(range_af, b.simd_sub(8, lc_hex, af_cvt)));
        }
        Value * binary_pack = b.bitCast(b.hsimd_packl(8, base_val[0], base_val[1]));
        b.storeOutputStreamBlock("binary_data", ZERO, b.getSize(i), binary_pack);
    }
}

BinaryToHex::BinaryToHex(LLVMTypeSystemInterface & ts, StreamSet * binStream, StreamSet * hexStream)
: BlockOrientedKernel(ts, "BinaryToHex",
                   {Binding{"binary_data", binStream, FixedRate(4)}},
                   {Binding{"hexdata", hexStream, FixedRate(), RoundUpTo(1)}},
                   {}, {}, {}) {}


void BinaryToHex::generateDoBlockMethod(KernelBuilder & b) {
    Type * i8Ty = b.getInt8Ty();
    Constant * const ZERO = b.getSize(0);
    const unsigned bytesPerBlock = b.getBitBlockWidth()/8;
    Constant * splatCh_0 = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, '0'));
    Constant * splat_9 = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, 9));
    Constant * splatCh_a = b.getSplat(bytesPerBlock, ConstantInt::get(i8Ty, 'a'-10));
    for (unsigned i = 0; i < 4; i++) {
        Value * bit_data = b.loadInputStreamBlock("binary_data", ZERO, b.getInt32(i));
        Value * nybble_0 = b.fwCast(8, b.esimd_mergel(4, bit_data, b.allZeroes()));
        Value * nybble_1 = b.fwCast(8, b.esimd_mergeh(4, bit_data, b.allZeroes()));
        Value * data_09 = b.simd_add(8, nybble_0, splatCh_0);
        Value * data_af = b.simd_add(8, nybble_0, splatCh_a);
        Value * hex_data = b.bitCast(b.CreateSelect(b.CreateICmpULE(nybble_0, splat_9), data_09, data_af));
        b.storeOutputStreamPack("hexdata", ZERO, b.getInt32(2*i), hex_data);
        data_09 = b.simd_add(8, nybble_1, splatCh_0);
        data_af = b.simd_add(8, nybble_1, splatCh_a);
        hex_data = b.bitCast(b.CreateSelect(b.CreateICmpULE(nybble_1, splat_9), data_09, data_af));
        b.storeOutputStreamPack("hexdata", ZERO, b.getInt32(2*i + 1), hex_data);
    }
}

void BinaryToHex::generateFinalBlockMethod(KernelBuilder & b, Value * const remainingBits) {
    Value * priorProduced = b.getProducedItemCount("hexdata");
    RepeatDoBlockLogic(b);
    Value * remainingHexDigits = b.CreateUDiv(b.CreateAdd(remainingBits, b.getSize(3)), b.getSize(4));
    Value * toZero = b.CreateSub(b.getSize(b.getBitBlockWidth()), remainingHexDigits);
    b.CreateMemZero(b.getRawOutputPointer("hexdata", b.CreateAdd(priorProduced, remainingHexDigits)), toZero);
}

static std::string bixSplitUniqueName(StreamSet * bixData, StreamSet * bixPairData) {
    unsigned inBits = bixData->getNumElements();
    unsigned outBits = bixPairData->getNumElements();
    assert((outBits < inBits) && (inBits <= 2 * outBits));
    std::stringstream ss;
    ss << "bixSplit_" << inBits << "_x1->" << outBits << "x1";
    return ss.str();
}

BixSplit::BixSplit(LLVMTypeSystemInterface & ts, StreamSet * bixData, StreamSet * bixPairData)
: BlockOrientedKernel(ts, bixSplitUniqueName(bixData, bixPairData),
                   {Binding{"bixData", bixData, FixedRate()}},
                   {Binding{"bixPairData", bixPairData, FixedRate(2)}},
{}, {}, {}), mInputBits(bixData->getNumElements()), mOutputBits(bixPairData->getNumElements()) {}

void BixSplit::generateDoBlockMethod(KernelBuilder & b) {
    Constant * const ZERO = b.getSize(0);
    for (unsigned out_bit = 0; out_bit < mOutputBits; out_bit++) {
        //  Output bit out_bit of the first (high) hex digit of each byte comes from
        //  bit out_bit + 4 of the byte, the second (low) digit from bit out_bit.
        Value * input[2];
        for (unsigned i = 0; i < 2; i++) {
            unsigned input_bit = out_bit + mOutputBits * (1 - i);
            if (input_bit >= mInputBits) {
                input[i] = b.allZeroes();
            } else {
                input[i] = b.loadInputStreamBlock("bixData", b.getInt32(input_bit), ZERO);
            }
        }

        Value * pack[2];
        pack[0] = b.esimd_mergel(1, input[0], input[1]);
        pack[1] = b.esimd_mergeh(1, input[0], input[1]);
        for (unsigned i = 0; i < 2; i++) {
            //  The output rate is 2, so each pack is a full block of output; pack i
            //  goes to block offset i (a pack index would address the next stream).
            b.storeOutputStreamBlock("bixPairData", b.getInt32(out_bit), b.getSize(i), pack[i]);
        }
    }
}

static BixNum Bix4toASCII_Hex(pablo::PabloBuilder & pb, BixNum & bix4Data, bool LowerHex = false) {
    BixNumCompiler bnc(pb);
    //  Given a 4-bit bixnum, a position is in the hex A-F range when its binary code is >= 10.
    PabloAST * hexA_F = bnc.UGE(bix4Data, 10, "hexA_F");
    //  If a number is in the hexA_F range, the low 4 bits are determined by subtracting 9.
    BixNum lo = bnc.Select(hexA_F, bnc.SubModular(bix4Data, 9), bix4Data);
    //
    // Now we will extend to 8 bits and insert the appropriate bits of the
    // ASCII representation.
    BixNum hexBasis = bnc.ZeroExtend(lo, 8);
    // Now compute the high 4 bits of the hexBasis.
    // High 4 bits are 0x40 for hex A-F, 0x60 for hex a-f, 0x30 for hex 0-9,
    hexBasis[7] = pb.createZeroes();  // the high bit (bit 7) is always zero for ASCII
    hexBasis[6] = hexA_F;
    if (LowerHex) {
        hexBasis[5] = pb.createOnes();
    } else {
        hexBasis[5] = pb.createNot(hexA_F);
    }
    hexBasis[4] = pb.createNot(hexA_F);
    return hexBasis;
}

Bix4toHex::Bix4toHex(LLVMTypeSystemInterface & ts,
                     StreamSet * bix4Data, StreamSet * hexBasis, bool LowerHex)
        : PabloKernel(ts, LowerHex ? "Bix4toHex_lc" : "Bix4toHex",
                      {Binding{"bix4Data", bix4Data}},
                      {Binding{"hexBasis", hexBasis}}), mLowerHex(LowerHex) {}

void Bix4toHex::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    std::vector<PabloAST *> hexBasis = Bix4toASCII_Hex(pb, bix4Data, mLowerHex);
    writeOutputStreamSet("hexBasis", hexBasis);
}

class UnicodeEscapedHex final : public pablo::PabloKernel {
public:
    UnicodeEscapedHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * hexBasis, StreamSet * filterMask);
protected:
    void generatePabloMethod() override;
};

UnicodeEscapedHex::UnicodeEscapedHex(LLVMTypeSystemInterface & ts,
                                     StreamSet * bix4Data,
                                     StreamSet * hexBasis, StreamSet * filterMask)
        : PabloKernel(ts, "UnicodeEscapedHex",
                      {Binding{"bix4Data", bix4Data}},
                      {Binding{"hexBasis", hexBasis}, Binding{"filterMask", filterMask}}) {}

void UnicodeEscapedHex::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    //
    // First convert the 4-bit bixnum values to an 8-bit hex-ASCII basis.
    BixNum hexBasis = Bix4toASCII_Hex(pb, bix4Data);

    //  Mark the start of every 8th value
    PabloAST * Umark = pb.createRepeat(1, pb.getInteger(0x01, 8));
    // Encode ASCII U = 0x55, given hexBasis value is digit 0 (0x30);
    hexBasis[6] = pb.createOr(hexBasis[6], Umark);
    hexBasis[5] = pb.createXor(hexBasis[5], Umark);  // flip bit 5
    hexBasis[2] = pb.createOr(hexBasis[2], Umark);
    hexBasis[0] = pb.createOr(hexBasis[0], Umark);
    //
    // Now deal with the "+" mark following U (+ = 0x2b)
    PabloAST * Uplus = pb.createAdvance(Umark, 1);
    hexBasis[4] = pb.createXor(hexBasis[4], Uplus);  // flip bit 4
    hexBasis[3] = pb.createOr(hexBasis[3], Uplus);
    hexBasis[1] = pb.createOr(hexBasis[1], Uplus);
    hexBasis[0] = pb.createOr(hexBasis[0], Uplus);
    //
    // Now determine whether we need to delete excess leading zeroes.
    PabloAST * leadingDigit = pb.createAdvance(Uplus, 1);
    PabloAST * sourceZeroes = bnc.EQ(bix4Data, 0);
    PabloAST * deleteLeading = pb.createAnd(leadingDigit, sourceZeroes);
    PabloAST * deleteSecond = pb.createAnd(pb.createAdvance(deleteLeading, 1), sourceZeroes);
    PabloAST * filterMask = pb.createInFile(pb.createNot(pb.createOr(deleteLeading, deleteSecond)));
    //
    writeOutputStreamSet("hexBasis", hexBasis);
    writeOutputStreamSet("filterMask", std::vector<PabloAST *>{filterMask});
}

class ZeroExtendASCII_to_U21 final : public pablo::PabloKernel {
public:
    ZeroExtendASCII_to_U21(LLVMTypeSystemInterface & ts, StreamSet * AsciiData, StreamSet * U21);
protected:
    void generatePabloMethod() override;
};

ZeroExtendASCII_to_U21::ZeroExtendASCII_to_U21(LLVMTypeSystemInterface & ts,
                                     StreamSet * AsciiData,
                                     StreamSet * U21)
        : PabloKernel(ts, "ZeroExtendASCII_to_U21",
                      {Binding{"AsciiData", AsciiData}},
                      {Binding{"U21", U21}}) {}

void ZeroExtendASCII_to_U21::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    std::vector<PabloAST *> ASCIIData = getInputStreamSet("AsciiData");
    std::vector<PabloAST *> U21data = bnc.ZeroExtend(ASCIIData, 21);
    writeOutputStreamSet("U21", U21data);
}

void kernel::UnicodeHexPipeline(PipelineBuilder & P, StreamSet * U21_in, StreamSet * U21_out) {

    StreamSet * Bix2x16 = P.CreateStreamSet(16);
    P.CreateKernelCall<BixSplit>(U21_in, Bix2x16);

    StreamSet * Bix4x8 = P.CreateStreamSet(8);
    P.CreateKernelCall<BixSplit>(Bix2x16, Bix4x8);

    StreamSet * Bix8x4 = P.CreateStreamSet(4);
    P.CreateKernelCall<BixSplit>(Bix4x8, Bix8x4);

    StreamSet * Hex8 = P.CreateStreamSet(8);
    StreamSet * filterMask = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodeEscapedHex>(Bix8x4, Hex8, filterMask);

    StreamSet * EncodedU8 = P.CreateStreamSet(8);
    FilterByMask(P, filterMask, Hex8, EncodedU8);

    P.CreateKernelCall<ZeroExtendASCII_to_U21>(EncodedU8, U21_out);
}

class BixDecodeSurrogates final : public pablo::PabloKernel {
public:
    BixDecodeSurrogates(LLVMTypeSystemInterface & ts, StreamSet * bix2x16Data, StreamSet * UTF16_pairs, StreamSet * UTF16_valid);
protected:
    void generatePabloMethod() override;
};

BixDecodeSurrogates::BixDecodeSurrogates(LLVMTypeSystemInterface & ts,
                                     StreamSet * bix2x16Data,
                                     StreamSet * UTF16_pairs, StreamSet * UTF16_valid)
        : PabloKernel(ts, "BixDecodeSurrogates",
                      {Binding{"bix2x16Data", bix2x16Data, FixedRate(), LookAhead(1)}},
                      {Binding{"UTF16_pairs", UTF16_pairs}, Binding{"UTF16_valid", UTF16_valid}}) {}

void BixDecodeSurrogates::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix2x16Data = getInputStreamSet("bix2x16Data");
    PabloAST * firstUnits = pb.createRepeat(1, pb.getInteger(0x55, 8));
    PabloAST * surrogate1 = pb.createAnd(firstUnits, bnc.NEQ(bix2x16Data, 0));
    std::vector<Var *> U16_bit(16);
    for (unsigned i = 0; i < 16; i++) {
        U16_bit[i] = pb.createVar("U16_" + std::to_string(i), bix2x16Data[i]);
    }
    auto nested = pb.createScope();
    // nested scope calculations
    BixNumCompiler bnc1(nested);
    PabloAST * surrogate2 = nested.createAdvance(surrogate1, 1);
    PabloAST * anySurrogate = nested.createOr(surrogate1, surrogate2);
    const unsigned movedSurrogateBits = 6;
    // The high surrogate of a codepoint hi:lo (hi = 0x01 to 0x10) is
    // 110110 (hi - 1)[4 bits] lo[15..10], at the position of hi; the low
    // surrogate is 110111 lo[9..0], at the position of lo.  At a surrogate1
    // position, the bits of hi are replaced, not combined.
    BixNum top4 = bnc1.SubModular(bnc1.Truncate(bix2x16Data, 4), 1);
    for (unsigned i = 0; i < 4; i++) {
        unsigned dest = i + movedSurrogateBits;
        PabloAST * bit_to_move = nested.createAnd(surrogate1, top4[i]);
        nested.createAssign(U16_bit[dest], nested.createOr(bix2x16Data[dest], bit_to_move));
    }
    //  Move bits 10 to 15 of the second of the pair to bits 0 to 5 of the first.
    PabloAST * notSurrogate1 = nested.createNot(surrogate1);
    for (unsigned i = 0; i < movedSurrogateBits; i++) {
        unsigned base = 16 - movedSurrogateBits;
        PabloAST * bitAhead = nested.createLookahead(bix2x16Data[base + i], 1);
        nested.createAssign(U16_bit[i], nested.createOr(nested.createAnd(notSurrogate1, bix2x16Data[i]),
                                                        nested.createAnd(surrogate1, bitAhead)));
    }
    // Set the upper bits of surrogate units  0xD8xx/DCxx
    nested.createAssign(U16_bit[15], nested.createOr(bix2x16Data[15], anySurrogate));
    nested.createAssign(U16_bit[14], nested.createOr(bix2x16Data[14], anySurrogate));
    nested.createAssign(U16_bit[13], nested.createAnd(bix2x16Data[13], nested.createNot(anySurrogate)));
    nested.createAssign(U16_bit[12], nested.createOr(bix2x16Data[12], anySurrogate));
    nested.createAssign(U16_bit[11], nested.createOr(bix2x16Data[11], anySurrogate));
    // Bit 10 distinguishes the low surrogate (0xDC00) from the high one (0xD800).
    nested.createAssign(U16_bit[10], nested.createOr(bix2x16Data[10], surrogate2));
    pb.createIf(surrogate1, nested);
    writeOutputStreamSet("UTF16_pairs", U16_bit);
    PabloAST * valid = pb.createOr(surrogate1, pb.createInFile(pb.createNot(firstUnits)));
    writeOutputStreamSet("UTF16_valid", std::vector<PabloAST *>{valid});
}

class JavaEscapedHex final : public pablo::PabloKernel {
public:
    JavaEscapedHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * mask001111, StreamSet * hexBasis);
protected:
    void generatePabloMethod() override;
};

JavaEscapedHex::JavaEscapedHex(LLVMTypeSystemInterface & ts,
                               StreamSet * bix4Data,
                               StreamSet * mask001111,
                               StreamSet * hexBasis)
        : PabloKernel(ts, "JavaEscapedHex",
                      {Binding{"bix4Data", bix4Data}, Binding{"mask001111", mask001111}},
                      {Binding{"hexBasis", hexBasis}}) {}

void JavaEscapedHex::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    PabloAST * mask001111 = getInputStreamSet("mask001111")[0];
    //
    // First convert the 4-bit bixnum values to an 8-bit hex-ASCII basis.
    BixNum hexBasis = Bix4toASCII_Hex(pb, bix4Data);

    PabloAST * mask110000 = pb.createNot(mask001111);
    PabloAST * mask010000 = pb.createAnd(mask110000, pb.createAdvance(mask110000, 1));
    PabloAST * mask100000 = pb.createXor(mask110000, mask010000);

    // At each mask100000 position, we need an \ (0x5c)
    // Note: hexBasis value is digit 0 (0x30);
    hexBasis[6] = pb.createOr(hexBasis[6], mask100000);
    hexBasis[5] = pb.createXor(hexBasis[5], mask100000);  // flip bit 5
    hexBasis[3] = pb.createOr(hexBasis[3], mask100000);
    hexBasis[2] = pb.createOr(hexBasis[2], mask100000);
    //
    // Now deal with the "u" mark following \ (u = 0x75)
    hexBasis[6] = pb.createOr(hexBasis[6], mask010000);
    // Bits 4, 5 already set by digit 0 (0x30)
    hexBasis[2] = pb.createOr(hexBasis[2], mask010000);
    hexBasis[0] = pb.createOr(hexBasis[0], mask010000);
    //
    writeOutputStreamSet("hexBasis", hexBasis);
}

void kernel::JavaHexPipeline(PipelineBuilder & P, StreamSet * U21_in, StreamSet * U21_out) {

    StreamSet * Bix2x16 = P.CreateStreamSet(16);
    P.CreateKernelCall<BixSplit>(U21_in, Bix2x16);

    StreamSet * UTF16_pairs = P.CreateStreamSet(16);
    StreamSet * UTF16_valid = P.CreateStreamSet(1);
    P.CreateKernelCall<BixDecodeSurrogates>(Bix2x16, UTF16_pairs, UTF16_valid);

    StreamSet * UTF16_basis = P.CreateStreamSet(16);
    FilterByMask(P, UTF16_valid, UTF16_pairs, UTF16_basis);

    StreamSet * Bix4x8 = P.CreateStreamSet(8);
    P.CreateKernelCall<BixSplit>(UTF16_basis, Bix4x8);

    StreamSet * Bix8x4 = P.CreateStreamSet(4);
    P.CreateKernelCall<BixSplit>(Bix4x8, Bix8x4);

    //  Each group of 4 hex digits is preceded by 2 positions for the backslash and u.
    //  SpreadByMask ends its output with the data (the mask itself repeats indefinitely).
    std::vector<uint64_t> expand_pattern = {0, 0, 1, 1, 1, 1};
    StreamSet * expand_mask = P.CreateRepeatingStreamSet(1, expand_pattern);

    StreamSet * ExpandedBix4 = P.CreateStreamSet(4);
    SpreadByMask(P, expand_mask, Bix8x4, ExpandedBix4);

    StreamSet * Hex8 = P.CreateStreamSet(8);
    P.CreateKernelCall<JavaEscapedHex>(ExpandedBix4, expand_mask, Hex8);

    P.CreateKernelCall<ZeroExtendASCII_to_U21>(Hex8, U21_out);
}

class CHexInsertMarks final : public pablo::PabloKernel {
public:
    CHexInsertMarks(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * insertMarks);
protected:
    void generatePabloMethod() override;
};

CHexInsertMarks::CHexInsertMarks(LLVMTypeSystemInterface & ts,
                                     StreamSet * bix4Data,
                                     StreamSet * insertMarks)
        : PabloKernel(ts, "CHexInsertMarks",
                      {Binding{"bix4Data", bix4Data, FixedRate(), LookAhead(3)}},
                      {Binding{"insertMarks", insertMarks}}) {}

void CHexInsertMarks::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    //
    // Identify the start of each 8 x 4bit group.
    PabloAST * startMarks = pb.createRepeat(1, pb.getInteger(0x01, 8));
    //
    // We have 8 positions of 4-bit data, which must be expanded to
    // 10 positions when the codedpoint is 0x10000 or greater.
    // This will occur whenever the 3rd or 4th 4-bit bixnum is
    // non-zero.  Calculate at the startMarks position.
    std::vector<PabloAST *> bixDataAhead2(4);
    std::vector<PabloAST *> bixDataAhead3(4);
    for (unsigned i = 0; i < 4; i++) {
        bixDataAhead2[i] = pb.createLookahead(bix4Data[i], 2);
        bixDataAhead3[i] = pb.createLookahead(bix4Data[i], 3);
    }
    PabloAST * above0x10000 =
        pb.createAnd(startMarks, pb.createOr(bnc.NEQ(bixDataAhead2, 0),
                                             bnc.NEQ(bixDataAhead3, 0)));
    PabloAST * insertMarks = pb.createOr(above0x10000, pb.createAdvance(above0x10000, 1));
    writeOutputStreamSet("insertMarks", std::vector<PabloAST *>{insertMarks});
}

class CEscapedHex final : public pablo::PabloKernel {
public:
    CEscapedHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * spreadMask,
                                              StreamSet * hexBasis, StreamSet * filterMask);
protected:
    void generatePabloMethod() override;
};

CEscapedHex::CEscapedHex(LLVMTypeSystemInterface & ts,
                                     StreamSet * bix4Data, StreamSet * spreadMask,
                                     StreamSet * hexBasis, StreamSet * filterMask)
        : PabloKernel(ts, "CEscapedHex",
                      {Binding{"bix4Data", bix4Data}, Binding{"spreadMask", spreadMask, FixedRate(), LookAhead(2)}},
                      {Binding{"hexBasis", hexBasis}, Binding{"filterMask", filterMask}}) {}

void CEscapedHex::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    //
    // First convert the 4-bit bixnum values to an 8-bit hex-ASCII basis.
    BixNum hexBasis = Bix4toASCII_Hex(pb, bix4Data);

    PabloAST * spreadMask = getInputStreamSet("spreadMask")[0];
    // The spreadmask for a group of 10 is .1.1111111
    PabloAST * startOf8 = pb.createEveryNth(spreadMask, pb.getInteger(8), "startOf8");
    PabloAST * startOf10 = pb.createInFile(pb.createNot(pb.createOr(spreadMask, pb.createLookahead(spreadMask, 2)), "startOf10"));
    startOf8 = pb.createAnd(startOf8, pb.createNot(pb.createAdvance(startOf10, 1)), "startOf8_2");
    PabloAST * escapePos = pb.createOr(startOf10, startOf8);

    // At each escapePos position, we need an \ (0x5c)
    // Note: hexBasis value is digit 0 (0x30);
    hexBasis[6] = pb.createOr(hexBasis[6], escapePos);
    hexBasis[5] = pb.createXor(hexBasis[5], escapePos);  // flip bit 5
    hexBasis[3] = pb.createOr(hexBasis[3], escapePos);
    hexBasis[2] = pb.createOr(hexBasis[2], escapePos);

    PabloAST * big_U = pb.createAdvance(startOf10, 1);
    PabloAST * small_u = pb.createAdvance(startOf8, 1);
    PabloAST * uU = pb.createOr(big_U, small_u);

    // Encode ASCII U/u = 0x55/0x75, given hexBasis value is digit 0 (0x30);
    hexBasis[6] = pb.createOr(hexBasis[6], uU);
    hexBasis[5] = pb.createXor(hexBasis[5], big_U);  // flip bit 5 for 0x55
    hexBasis[2] = pb.createOr(hexBasis[2], uU);
    hexBasis[0] = pb.createOr(hexBasis[0], uU);
    //
    // Now create a filter mask to delete 2 positions after \u
    PabloAST * delMarks = pb.createOr(pb.createAdvance(small_u, 1), pb.createAdvance(small_u, 2));
    PabloAST * filterMask = pb.createInFile(pb.createNot(delMarks));
    //
    writeOutputStreamSet("hexBasis", hexBasis);
    writeOutputStreamSet("filterMask", std::vector<PabloAST *>{filterMask});
}

void kernel::CHexPipeline(PipelineBuilder & P, StreamSet * U21_in, StreamSet * U21_out) {

    StreamSet * Bix2x16 = P.CreateStreamSet(16);
    P.CreateKernelCall<BixSplit>(U21_in, Bix2x16);

    StreamSet * Bix4x8 = P.CreateStreamSet(8);
    P.CreateKernelCall<BixSplit>(Bix2x16, Bix4x8);

    StreamSet * Bix8x4 = P.CreateStreamSet(4);
    P.CreateKernelCall<BixSplit>(Bix4x8, Bix8x4);

    // When a supplementary plane character exists, we need 10 total
    // positions for the hex encoding.  Account for this by marking
    // the first two positions for insertion.
    StreamSet * insertMarks = P.CreateStreamSet(1);
    P.CreateKernelCall<CHexInsertMarks>(Bix8x4, insertMarks);

    StreamSet * spreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, insertMarks, spreadMask, kernel::InsertPosition::Before);

    StreamSet * expandedBix4 = P.CreateStreamSet(4);
    SpreadByMask(P, spreadMask, Bix8x4, expandedBix4);

    StreamSet * Hex8 = P.CreateStreamSet(8);
    StreamSet * filterMask = P.CreateStreamSet(1);
    //
    P.CreateKernelCall<CEscapedHex>(expandedBix4, spreadMask, Hex8, filterMask);

    StreamSet * EncodedU8 = P.CreateStreamSet(8);
    FilterByMask(P, filterMask, Hex8, EncodedU8);

    P.CreateKernelCall<ZeroExtendASCII_to_U21>(EncodedU8, U21_out);
}

class XMLEscapedHex final : public pablo::PabloKernel {
public:
    XMLEscapedHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * spreadMask,
                                              StreamSet * hexBasis, StreamSet * filterMask);
protected:
    void generatePabloMethod() override;
};

XMLEscapedHex::XMLEscapedHex(LLVMTypeSystemInterface & ts,
                                     StreamSet * bix4Data, StreamSet * spreadMask,
                                     StreamSet * hexBasis, StreamSet * filterMask)
        : PabloKernel(ts, "XMLEscapedHex",
                      {Binding{"bix4Data", bix4Data, FixedRate(), LookAhead(1)},
                       Binding{"spreadMask", spreadMask, FixedRate(), LookAhead(2)}},
                      {Binding{"hexBasis", hexBasis}, Binding{"filterMask", filterMask}}) {}

void XMLEscapedHex::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    //
    // Data bits must move ahead 1 position to make way for the
    // final semicolon.
    std::vector<PabloAST *> bixDataAhead1(4);
    for (unsigned i = 0; i < 4; i++) {
        bixDataAhead1[i] = pb.createLookahead(bix4Data[i], 1);
    }
    // Now convert the 4-bit bixnum values to an 8-bit hex-ASCII basis.
    BixNum hexBasis = Bix4toASCII_Hex(pb, bixDataAhead1);

    PabloAST * spreadMask = getInputStreamSet("spreadMask")[0];
    // The spreadmask for a group of 10 is .1.1111111
    PabloAST * startOf8 = pb.createEveryNth(spreadMask, pb.getInteger(8), "startOf8");
    PabloAST * startOf10 = pb.createInFile(pb.createNot(pb.createOr(spreadMask, pb.createLookahead(spreadMask, 2)), "startOf10"));
    startOf8 = pb.createAnd(startOf8, pb.createNot(pb.createAdvance(startOf10, 1)), "startOf8_2");

    // Mark positions for prefixing encoded values with &#x
    PabloAST * escapePos = pb.createOr(startOf10, startOf8);
    PabloAST * hashPos = pb.createAdvance(escapePos, 1);
    PabloAST * xPos = pb.createAdvance(hashPos, 1);
    // Mark the position of the final digit, as well as the
    // position to receive the trailing semicolon.
    PabloAST * lastDigitPos = pb.createOr(pb.createAdvance(startOf10, 8),
                                         pb.createAdvance(startOf8, 6));
    PabloAST * semiColonPos = pb.createAdvance(lastDigitPos, 1);

    // At each escapePos position, we need an & (0x26)
    // Note: hexBasis value is digit 0 (0x30).
    hexBasis[4] = pb.createXor(hexBasis[4], escapePos);  // flip bit 4
    hexBasis[2] = pb.createOr(hexBasis[2], escapePos);
    hexBasis[1] = pb.createOr(hexBasis[1], escapePos);

    // At hashPos, encode ASCII # = 0x23, given hexBasis value is 0x30.
    hexBasis[4] = pb.createXor(hexBasis[4], hashPos);  // flip bit 4
    hexBasis[1] = pb.createOr(hexBasis[1], hashPos);
    hexBasis[0] = pb.createOr(hexBasis[0], hashPos);

    // At xPos, encode ASCII x = 0x78, given hexBasis value is 0x30.
    hexBasis[6] = pb.createOr(hexBasis[6], xPos);
    hexBasis[3] = pb.createOr(hexBasis[3], xPos);

    // At semiColonPos, encode ASCII ; = 0x3b, aware that any
    // hex value may be at this location.
    PabloAST * notSemiColonPos = pb.createNot(semiColonPos);
    hexBasis[6] = pb.createAnd(hexBasis[6], notSemiColonPos);
    hexBasis[5] = pb.createOr(hexBasis[5], semiColonPos);
    hexBasis[4] = pb.createOr(hexBasis[4], semiColonPos);
    hexBasis[3] = pb.createOr(hexBasis[3], semiColonPos);
    hexBasis[2] = pb.createAnd(hexBasis[2], notSemiColonPos);
    hexBasis[1] = pb.createOr(hexBasis[1], semiColonPos);
    hexBasis[0] = pb.createOr(hexBasis[0], semiColonPos);

    //
    // Now create a filter mask to delete all leading zeroes (except for
    // the least significant position - &#x0; encodes codepoint 0.
    PabloAST * isZero = bnc.EQ(bixDataAhead1, 0);
    PabloAST * mayDelete = pb.createAnd(isZero, pb.createNot(lastDigitPos));
    PabloAST * firstZero = pb.createAnd(isZero, pb.createAdvance(xPos, 1));
    PabloAST * toDelete = pb.createAnd(pb.createMatchStar(firstZero, mayDelete), mayDelete);
    PabloAST * filterMask = pb.createInFile(pb.createNot(toDelete));
    //
    writeOutputStreamSet("hexBasis", hexBasis);
    writeOutputStreamSet("filterMask", std::vector<PabloAST *>{filterMask});
}

void kernel::XMLHexPipeline(PipelineBuilder & P, StreamSet * U21_in, StreamSet * U21_out) {

    StreamSet * Bix2x16 = P.CreateStreamSet(16);
    P.CreateKernelCall<BixSplit>(U21_in, Bix2x16);

    StreamSet * Bix4x8 = P.CreateStreamSet(8);
    P.CreateKernelCall<BixSplit>(Bix2x16, Bix4x8);

    StreamSet * Bix8x4 = P.CreateStreamSet(4);
    P.CreateKernelCall<BixSplit>(Bix4x8, Bix8x4);

    // When a codepoint above 0xFFFFF exists, we need 10 total
    // positions for the hex encoding.  Nine positions are needed
    // for codepoints in the range 0x10000-0xFFFFF.  However,
    // it is safe and simpler to expand to length 10 for both.
    StreamSet * insertMarks = P.CreateStreamSet(1);
    P.CreateKernelCall<CHexInsertMarks>(Bix8x4, insertMarks);

    StreamSet * spreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, insertMarks, spreadMask, kernel::InsertPosition::Before);

    StreamSet * expandedBix4 = P.CreateStreamSet(4);
    SpreadByMask(P, spreadMask, Bix8x4, expandedBix4);

    StreamSet * Hex8 = P.CreateStreamSet(8);
    StreamSet * filterMask = P.CreateStreamSet(1);

    P.CreateKernelCall<XMLEscapedHex>(expandedBix4, spreadMask, Hex8, filterMask);

    StreamSet * EncodedU8 = P.CreateStreamSet(8);
    FilterByMask(P, filterMask, Hex8, EncodedU8);

    P.CreateKernelCall<ZeroExtendASCII_to_U21>(EncodedU8, U21_out);
}

class PerlEscapedHex final : public pablo::PabloKernel {
public:
    PerlEscapedHex(LLVMTypeSystemInterface & ts, StreamSet * bix4Data, StreamSet * spreadMask,
                                              StreamSet * hexBasis, StreamSet * filterMask);
protected:
    void generatePabloMethod() override;
};

PerlEscapedHex::PerlEscapedHex(LLVMTypeSystemInterface & ts,
                                     StreamSet * bix4Data, StreamSet * spreadMask,
                                     StreamSet * hexBasis, StreamSet * filterMask)
        : PabloKernel(ts, "PerlEscapedHex",
                      {Binding{"bix4Data", bix4Data, FixedRate(), LookAhead(1)},
                       Binding{"spreadMask", spreadMask, FixedRate(), LookAhead(2)}},
                      {Binding{"hexBasis", hexBasis}, Binding{"filterMask", filterMask}}) {}

void PerlEscapedHex::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);
    // Get the input stream sets.
    std::vector<PabloAST *> bix4Data = getInputStreamSet("bix4Data");
    //
    // Data bits must move ahead 1 position to make way for the
    // final semicolon.
    std::vector<PabloAST *> bixDataAhead1(4);
    for (unsigned i = 0; i < 4; i++) {
        bixDataAhead1[i] = pb.createLookahead(bix4Data[i], 1);
    }
    // Now convert the 4-bit bixnum values to an 8-bit hex-ASCII basis.
    BixNum hexBasis = Bix4toASCII_Hex(pb, bixDataAhead1);

    PabloAST * spreadMask = getInputStreamSet("spreadMask")[0];
    // The spreadmask for a group of 10 is .1.1111111
    PabloAST * startOf8 = pb.createEveryNth(spreadMask, pb.getInteger(8), "startOf8");
    PabloAST * startOf10 = pb.createInFile(pb.createNot(pb.createOr(spreadMask, pb.createLookahead(spreadMask, 2)), "startOf10"));
    startOf8 = pb.createAnd(startOf8, pb.createNot(pb.createAdvance(startOf10, 1)), "startOf8_2");

    // Mark positions for prefixing encoded values with "\x{"
    PabloAST * escapePos = pb.createOr(startOf10, startOf8);
    PabloAST * pos2 = pb.createAdvance(escapePos, 1);
    PabloAST * pos3 = pb.createAdvance(pos2, 1);
    // Mark the position of the final digit, as well as the
    // position to receive the trailing semicolon.
    PabloAST * lastDigitPos = pb.createOr(pb.createAdvance(startOf10, 8),
                                         pb.createAdvance(startOf8, 6));
    PabloAST * closingPos = pb.createAdvance(lastDigitPos, 1);

    // At each escapePos position, we need a \ (0x5c)
    // Note: hexBasis value is digit 0 (0x30).
    hexBasis[6] = pb.createOr(hexBasis[6], escapePos);
    hexBasis[5] = pb.createXor(hexBasis[5], escapePos);  // flip bit 5
    hexBasis[3] = pb.createOr(hexBasis[3], escapePos);
    hexBasis[2] = pb.createOr(hexBasis[2], escapePos);

    // At pos2, encode ASCII x = 0x78, given hexBasis value is 0x30.
    hexBasis[6] = pb.createOr(hexBasis[6], pos2);
    hexBasis[3] = pb.createOr(hexBasis[3], pos2);

    // At pos3, encode ASCII { = 0x7b, given hexBasis value is 0x30.
    hexBasis[6] = pb.createOr(hexBasis[6], pos3);
    hexBasis[3] = pb.createOr(hexBasis[3], pos3);
    hexBasis[1] = pb.createOr(hexBasis[1], pos3);
    hexBasis[0] = pb.createOr(hexBasis[0], pos3);

    // At closingPos, encode ASCII } = 0x7d, aware that any
    // hex value may be at this location.
    hexBasis[6] = pb.createOr(hexBasis[6], closingPos);
    hexBasis[5] = pb.createOr(hexBasis[5], closingPos);
    hexBasis[4] = pb.createOr(hexBasis[4], closingPos);
    hexBasis[3] = pb.createOr(hexBasis[3], closingPos);
    hexBasis[2] = pb.createOr(hexBasis[2], closingPos);
    hexBasis[1] = pb.createAnd(hexBasis[1], pb.createNot(closingPos));
    hexBasis[0] = pb.createOr(hexBasis[0], closingPos);

    //
    // Now create a filter mask to delete all leading zeroes (except for
    // the least significant position - &#x0; encodes codepoint 0.
    PabloAST * isZero = bnc.EQ(bixDataAhead1, 0);
    PabloAST * mayDelete = pb.createAnd(isZero, pb.createNot(lastDigitPos));
    PabloAST * firstZero = pb.createAnd(isZero, pb.createAdvance(pos3, 1));
    PabloAST * toDelete = pb.createAnd(pb.createMatchStar(firstZero, mayDelete), mayDelete);
    PabloAST * filterMask = pb.createInFile(pb.createNot(toDelete));
    //
    writeOutputStreamSet("hexBasis", hexBasis);
    writeOutputStreamSet("filterMask", std::vector<PabloAST *>{filterMask});
}

void kernel::PerlHexPipeline(PipelineBuilder & P, StreamSet * U21_in, StreamSet * U21_out) {

    StreamSet * Bix2x16 = P.CreateStreamSet(16);
    P.CreateKernelCall<BixSplit>(U21_in, Bix2x16);

    StreamSet * Bix4x8 = P.CreateStreamSet(8);
    P.CreateKernelCall<BixSplit>(Bix2x16, Bix4x8);

    StreamSet * Bix8x4 = P.CreateStreamSet(4);
    P.CreateKernelCall<BixSplit>(Bix4x8, Bix8x4);

    // When a codepoint above 0xFFFFF exists, we need 10 total
    // positions for the hex encoding.  Nine positions are needed
    // for codepoints in the range 0x10000-0xFFFFF.  However,
    // it is safe and simpler to expand to length 10 for both.
    StreamSet * insertMarks = P.CreateStreamSet(1);
    P.CreateKernelCall<CHexInsertMarks>(Bix8x4, insertMarks);

    StreamSet * spreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, insertMarks, spreadMask, kernel::InsertPosition::Before);

    StreamSet * expandedBix4 = P.CreateStreamSet(4);
    SpreadByMask(P, spreadMask, Bix8x4, expandedBix4);

    StreamSet * Hex8 = P.CreateStreamSet(8);
    StreamSet * filterMask = P.CreateStreamSet(1);

    P.CreateKernelCall<PerlEscapedHex>(expandedBix4, spreadMask, Hex8, filterMask);

    StreamSet * EncodedU8 = P.CreateStreamSet(8);
    FilterByMask(P, filterMask, Hex8, EncodedU8);

    P.CreateKernelCall<ZeroExtendASCII_to_U21>(EncodedU8, U21_out);
}

HexDecode::HexDecode(LLVMTypeSystemInterface & ts,
                                     StreamSet * sourceBasis, StreamSet * hexSpans,
                                     StreamSet * decoded)
        : PabloKernel(ts, "HexDecode" + sourceBasis->shapeString(),
                      {Binding{"sourceBasis", sourceBasis}, Binding{"hexSpans", hexSpans, FixedRate(), LookAhead(1)}},
                      {Binding{"decoded", decoded}}) {
                      assert(sourceBasis->getNumElements() == 21);
                      }

void HexDecode::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    // Get the input stream sets.
    std::vector<PabloAST *> sourceBasis = getInputStreamSet("sourceBasis");
    PabloAST * hexSpans = getInputStreamSet("hexSpans")[0];
    //
    std::vector<Var *> outputVars(sourceBasis.size());
    for (unsigned i = 0; i < sourceBasis.size(); i++) {
        outputVars[i] = pb.createVar("outVar" + std::to_string(i), sourceBasis[i]);
    }
    cc::Parabix_CC_Compiler_Builder ccc(sourceBasis);

    auto nested = pb.createScope();

    BixNumCompiler bnc(nested);
    // The value of a hex digit: its low 4 bits, plus 9 for the letters.
    PabloAST * hex_letter = ccc.compileCC(re::makeCC(re::makeCC('A', 'F'), re::makeCC('a', 'f')), nested);
    BixNum low4 = bnc.Truncate(sourceBasis, 4);
    BixNum hexVal = bnc.Select(hex_letter, bnc.AddModular(low4, 9), low4);

    // We accumulate the 21 basis bits at the last position of the hexspan
    PabloAST * spanEnd = nested.createAnd(hexSpans, nested.createNot(nested.createLookahead(hexSpans, 1)));

    // The value of the digits of the span up to each position, by doubling: of
    // the last 2 (basis8), 4 (basis16) and 6 (basis21) digits, each part taken
    // where the span covers the positions it comes from.
    PabloAST * runOf2 = nested.createAnd(hexSpans, nested.createAdvance(hexSpans, 1));
    BixNum basis8 = bnc.ZeroExtend(hexVal, 8);
    for (unsigned i = 0; i < 4; i++) {
        basis8[i+4] = nested.createAnd(runOf2, nested.createAdvance(hexVal[i], 1));
    }

    PabloAST * runOf3 = nested.createAnd(hexSpans, nested.createAdvance(runOf2, 1));
    BixNum basis16 = bnc.ZeroExtend(basis8, 16);
    for (unsigned i = 0; i < 8; i++) {
        basis16[i+8] = nested.createAnd(runOf3, nested.createAdvance(basis8[i], 2));
    }

    PabloAST * runOf5 = nested.createAnd(runOf2, nested.createAdvance(runOf3, 2));
    BixNum basis21 = bnc.ZeroExtend(basis16, 21);
    for (unsigned i = 0; i < 5; i++) {
        basis21[i+16] = nested.createAnd(runOf5, nested.createAdvance(basis8[i], 4));
    }
    basis21 = bnc.Select(spanEnd, basis21, sourceBasis);

    for (unsigned i = 0; i < 21; i++) {
        nested.createAssign(outputVars[i], basis21[i]);
    }

    pb.createIf(hexSpans, nested);
    for (unsigned i = 0; i < sourceBasis.size(); i++) {
        pb.createAssign(pb.createExtract(getOutput(0), i), outputVars[i]);
    }
}

DecimalDecode::DecimalDecode(LLVMTypeSystemInterface & ts, StreamSet * sourceBasis, StreamSet * decSpans,
                             StreamSet * decoded, StreamSet * overflow)
        : PabloKernel(ts, "DecimalDecode" + sourceBasis->shapeString(),
                      {Binding{"sourceBasis", sourceBasis}, Binding{"decSpans", decSpans, FixedRate(), LookAhead(1)}},
                      {Binding{"decoded", decoded}, Binding{"overflow", overflow}}) {
    assert(sourceBasis->getNumElements() == 21);
}

void DecimalDecode::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> sourceBasis = getInputStreamSet("sourceBasis");
    PabloAST * decSpans = getInputStreamSet("decSpans")[0];
    BixNumCompiler bnc(pb);
    //  27 bits hold the values of 8 decimal digits.
    constexpr unsigned W = 27;
    auto advance = [&](BixNum v, unsigned n) {
        for (PabloAST *& b : v) b = pb.createAdvance(b, n);
        return v;
    };
    PabloAST * spanEnd = pb.createAnd(decSpans, pb.createNot(pb.createLookahead(decSpans, 1)));
    //  The value of the digits of the span up to each position, by doubling: of
    //  the last 2, 4 and 8 digits, each part taken where the span covers the
    //  positions it comes from (as for HexDecode).
    BixNum digit = bnc.ZeroExtend(bnc.Truncate(sourceBasis, 4), W);
    PabloAST * runOf2 = pb.createAnd(decSpans, pb.createAdvance(decSpans, 1));
    BixNum value2 = bnc.AddModular(digit, bnc.MulModular(bnc.Select(runOf2, advance(digit, 1)), 10));
    PabloAST * runOf3 = pb.createAnd(decSpans, pb.createAdvance(runOf2, 1));
    BixNum value4 = bnc.AddModular(value2, bnc.MulModular(bnc.Select(runOf3, advance(value2, 2)), 100));
    PabloAST * runOf5 = pb.createAnd(runOf2, pb.createAdvance(runOf3, 2));
    BixNum value8 = bnc.AddModular(value4, bnc.MulModular(bnc.Select(runOf5, advance(value4, 4)), 10000));
    PabloAST * overflow = pb.createAnd(spanEnd, bnc.UGT(value8, 0x10FFFF));
    BixNum decoded = bnc.Select(pb.createAnd(spanEnd, pb.createNot(overflow)), bnc.Truncate(value8, 21), sourceBasis);
    for (unsigned i = 0; i < 21; i++) {
        pb.createAssign(pb.createExtract(getOutputStreamVar("decoded"), pb.getInteger(i)), decoded[i]);
    }
    pb.createAssign(pb.createExtract(getOutputStreamVar("overflow"), pb.getInteger(0)), overflow);
}

//  The escapes of the given forms (see HexEscapeForm): the spans of their hex
//  and decimal digits, the positions to remove (the prefixes, the digits other
//  than the last, the terminators, and the whole of an escape of a value beyond
//  10FFFF, of which hex values are recognized here), and the last digits of
//  the other escapes (Ends).  An escape is recognized at its first position,
//  from the classes of the characters ahead (within the mask).
class HexEscapes final : public pablo::PabloKernel {
public:
    HexEscapes(LLVMTypeSystemInterface & ts, unsigned forms, StreamSet * U21, StreamSet * mask,
               StreamSet * hexSpans, StreamSet * decSpans, StreamSet * remove, StreamSet * ends)
        : PabloKernel(ts, "HexEscapes" + std::to_string(forms) + (mask ? "m" : ""),
                      [&] {
                          Bindings b{Binding{"U21", U21, FixedRate(), LookAhead(Lookahead)}};
                          if (mask) b.emplace_back("mask", mask, FixedRate(), LookAhead(Lookahead));
                          return b;
                      }(),
                      {Binding{"hexSpans", hexSpans}, Binding{"decSpans", decSpans}, Binding{"remove", remove},
                       Binding{"ends", ends}}),
          mForms(forms), mHasMask(mask != nullptr) {}
protected:
    void generatePabloMethod() override;
private:
    static constexpr unsigned Lookahead = 10;      // the longest escape: 10 positions
    const unsigned mForms;
    const bool mHasMask;
};

void HexEscapes::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    const std::vector<PabloAST *> basis = getInputStreamSet("U21");
    PabloAST * const mask = mHasMask ? getInputStreamSet("mask")[0] : nullptr;
    //  The classes of the characters k positions ahead (within the mask).
    std::vector<std::unique_ptr<cc::Parabix_CC_Compiler_Builder>> ahead(Lookahead + 1);
    std::map<std::pair<unsigned, std::string>, PabloAST *> memo;
    auto cls = [&](unsigned k, re::CC * cc) -> PabloAST * {
        auto & m = memo[{k, cc->canonicalName()}];
        if (m) return m;
        if (!ahead[k]) {
            std::vector<PabloAST *> bits(basis.size());
            for (unsigned b = 0; b < basis.size(); b++) bits[b] = k ? pb.createLookahead(basis[b], k) : basis[b];
            ahead[k] = std::make_unique<cc::Parabix_CC_Compiler_Builder>(bits);
        }
        PabloAST * c = ahead[k]->compileCC(cc, pb);
        if (mask) c = pb.createAnd(c, k ? pb.createLookahead(mask, k) : mask);
        return m = c;
    };
    re::CC * const hex = re::makeCC(re::makeCC(re::makeCC('0', '9'), re::makeCC('A', 'F')), re::makeCC('a', 'f'));
    re::CC * const dec = re::makeCC('0', '9');
    re::CC * const hexNonzero = re::makeCC(re::makeCC(re::makeCC('1', '9'), re::makeCC('A', 'F')), re::makeCC('a', 'f'));
    re::CC * const hexAtLeast2 = re::makeCC(re::makeCC(re::makeCC('2', '9'), re::makeCC('A', 'F')), re::makeCC('a', 'f'));
    re::CC * const one = re::makeCC('1');
    struct Form {
        unsigned flag;
        std::vector<re::CC *> prefix;
        bool decimal;
        unsigned minDigits, maxDigits;
        re::CC * terminator;            // or nullptr
    };
    const std::vector<Form> forms = {
        {HexEscapeJava | HexEscapeC, {re::makeCC('\\'), re::makeCC('u')}, false, 4, 4, nullptr},
        {HexEscapeC, {re::makeCC('\\'), re::makeCC('U')}, false, 8, 8, nullptr},
        {HexEscapePerl, {re::makeCC('\\'), re::makeCC('x'), re::makeCC('{')}, false, 1, 6, re::makeCC('}')},
        {HexEscapeUnicode, {re::makeCC('U'), re::makeCC('+')}, false, 4, 6, nullptr},
        {HexEscapeXML, {re::makeCC('&'), re::makeCC('#'), re::makeCC('x')}, false, 1, 6, re::makeCC(';')},
        {HexEscapeXML10, {re::makeCC('&'), re::makeCC('#')}, true, 1, 7, re::makeCC(';')},
    };
    PabloAST * hexSpans = pb.createZeroes();
    PabloAST * decSpans = pb.createZeroes();
    PabloAST * remove = pb.createZeroes();
    PabloAST * ends = pb.createZeroes();
    for (const Form & f : forms) {
        if ((f.flag & mForms) == 0) continue;
        const unsigned a = f.prefix.size();
        re::CC * const digit = f.decimal ? dec : hex;
        PabloAST * prefix = pb.createOnes();
        for (unsigned k = 0; k < a; k++) prefix = pb.createAnd(prefix, cls(k, f.prefix[k]));
        for (unsigned n = f.minDigits; n <= f.maxDigits; n++) {
            //  An escape of n digits starting here: the digits, then the
            //  terminator, or (without one) a character other than a digit,
            //  unless the escape has the most digits (taken as they come).
            PabloAST * start = prefix;
            for (unsigned k = 0; k < n; k++) start = pb.createAnd(start, cls(a + k, digit));
            if (f.terminator) {
                start = pb.createAnd(start, cls(a + n, f.terminator));
            } else if (n < f.maxDigits) {
                start = pb.createAnd(start, pb.createNot(cls(a + n, digit)));
            }
            //  A hex value beyond 10FFFF: of 6 digits, 2 or more, or 1 then not 0;
            //  of 8, also not two leading zeros.
            PabloAST * over = pb.createZeroes();
            if (!f.decimal && n >= 6) {
                const unsigned d = a + n - 6;
                over = pb.createOr(cls(d, hexAtLeast2), pb.createAnd(cls(d, one), cls(d + 1, hexNonzero)));
                for (unsigned k = a; k < d; k++) over = pb.createOr(over, cls(k, hexNonzero));
                over = pb.createAnd(start, over);
            }
            for (unsigned k = 0; k < a; k++) remove = pb.createOr(remove, pb.createAdvance(start, k));
            for (unsigned k = 0; k < n; k++) {
                PabloAST * const at = pb.createAdvance(start, a + k);
                if (f.decimal) decSpans = pb.createOr(decSpans, at); else hexSpans = pb.createOr(hexSpans, at);
                if (k + 1 < n) remove = pb.createOr(remove, at);
            }
            remove = pb.createOr(remove, pb.createAdvance(over, a + n - 1));
            if (f.terminator) remove = pb.createOr(remove, pb.createAdvance(start, a + n));
            ends = pb.createOr(ends, pb.createAdvance(pb.createAnd(start, pb.createNot(over)), a + n - 1));
        }
    }
    pb.createAssign(pb.createExtract(getOutputStreamVar("hexSpans"), pb.getInteger(0)), hexSpans);
    pb.createAssign(pb.createExtract(getOutputStreamVar("decSpans"), pb.getInteger(0)), decSpans);
    pb.createAssign(pb.createExtract(getOutputStreamVar("remove"), pb.getInteger(0)), remove);
    pb.createAssign(pb.createExtract(getOutputStreamVar("ends"), pb.getInteger(0)), ends);
}

//  The positions kept (not removed, nor marked in overflow), and the ends of the
//  escapes decoded (not in overflow).
class HexEscapesKept final : public pablo::PabloKernel {
public:
    HexEscapesKept(LLVMTypeSystemInterface & ts, StreamSet * remove, StreamSet * overflow, StreamSet * ends,
                   StreamSet * keep, StreamSet * decodedEnds)
        : PabloKernel(ts, "HexEscapesKept",
                      {Binding{"remove", remove}, Binding{"overflow", overflow}, Binding{"ends", ends}},
                      {Binding{"keep", keep}, Binding{"decodedEnds", decodedEnds}}) {}
protected:
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        PabloAST * const overflow = getInputStreamSet("overflow")[0];
        PabloAST * const removed = pb.createOr(getInputStreamSet("remove")[0], overflow);
        pb.createAssign(pb.createExtract(getOutputStreamVar("keep"), pb.getInteger(0)), pb.createInFile(pb.createNot(removed)));
        pb.createAssign(pb.createExtract(getOutputStreamVar("decodedEnds"), pb.getInteger(0)),
                        pb.createAnd(getInputStreamSet("ends")[0], pb.createNot(overflow)));
    }
};

//  The surrogates decoded (at Ends): a high surrogate followed by a low one
//  makes the codepoint of the pair (the low one is then not kept), and a lone
//  surrogate becomes U+FFFD.
class CombineSurrogates final : public pablo::PabloKernel {
public:
    CombineSurrogates(LLVMTypeSystemInterface & ts, StreamSet * U21, StreamSet * ends, StreamSet * combined, StreamSet * keep)
        : PabloKernel(ts, "CombineSurrogates",
                      {Binding{"U21", U21, FixedRate(), LookAhead(1)}, Binding{"ends", ends, FixedRate(), LookAhead(1)}},
                      {Binding{"combined", combined}, Binding{"keep", keep}}) {}
protected:
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        const std::vector<PabloAST *> basis = getInputStreamSet("U21");
        PabloAST * const ends = getInputStreamSet("ends")[0];
        std::vector<PabloAST *> next(basis.size());
        for (unsigned b = 0; b < basis.size(); b++) next[b] = pb.createLookahead(basis[b], 1);
        cc::Parabix_CC_Compiler_Builder here(basis);
        cc::Parabix_CC_Compiler_Builder ahead(next);
        PabloAST * const surrogate = pb.createAnd(ends, here.compileCC(re::makeCC(0xD800, 0xDFFF), pb));
        PabloAST * const high = pb.createAnd(ends, here.compileCC(re::makeCC(0xD800, 0xDBFF), pb));
        PabloAST * const lowNext = pb.createAnd(pb.createLookahead(ends, 1), ahead.compileCC(re::makeCC(0xDC00, 0xDFFF), pb));
        PabloAST * const pair = pb.createAnd(high, lowNext);
        PabloAST * const second = pb.createAdvance(pair, 1);
        PabloAST * const lone = pb.createAnd(surrogate, pb.createNot(pb.createOr(pair, second)));
        //  0x10000 + (high - D800) * 0x400 + (low - DC00).
        BixNum pairValue(21);
        for (unsigned b = 0; b < 10; b++) {
            pairValue[b] = next[b];
            pairValue[b + 10] = basis[b];
        }
        pairValue[20] = pb.createZeroes();
        pairValue = bnc.AddModular(pairValue, 0x10000);
        BixNum out = bnc.Select(pair, pairValue, bnc.Select(lone, bnc.ZeroExtend(bnc.Create(0xFFFD), 21), basis));
        for (unsigned i = 0; i < 21; i++) {
            pb.createAssign(pb.createExtract(getOutputStreamVar("combined"), pb.getInteger(i)), out[i]);
        }
        pb.createAssign(pb.createExtract(getOutputStreamVar("keep"), pb.getInteger(0)), pb.createInFile(pb.createNot(second)));
    }
};

void kernel::HexAnyPipeline(PipelineBuilder & P, unsigned forms, StreamSet * U21, StreamSet * mask, StreamSet * U21_out,
                            std::vector<StreamSet *> & keeps) {
    StreamSet * const hexSpans = P.CreateStreamSet(1);
    StreamSet * const decSpans = P.CreateStreamSet(1);
    StreamSet * const remove = P.CreateStreamSet(1);
    StreamSet * const ends = P.CreateStreamSet(1);
    P.CreateKernelCall<HexEscapes>(forms, U21, mask, hexSpans, decSpans, remove, ends);
    StreamSet * decoded = U21;
    if (forms & ~HexEscapeXML10) {
        StreamSet * const hexDecoded = P.CreateStreamSet(21, 1);
        P.CreateKernelCall<HexDecode>(decoded, hexSpans, hexDecoded);
        decoded = hexDecoded;
    }
    StreamSet * overflow = nullptr;
    if (forms & HexEscapeXML10) {
        StreamSet * const decDecoded = P.CreateStreamSet(21, 1);
        overflow = P.CreateStreamSet(1);
        P.CreateKernelCall<DecimalDecode>(decoded, decSpans, decDecoded, overflow);
        decoded = decDecoded;
    } else {
        overflow = P.CreateRepeatingStreamSet(1, std::vector<uint64_t>{0});
    }
    StreamSet * const keep = P.CreateStreamSet(1);
    StreamSet * const decodedEnds = P.CreateStreamSet(1);
    P.CreateKernelCall<HexEscapesKept>(remove, overflow, ends, keep, decodedEnds);
    StreamSet * const kept = P.CreateStreamSet(21, 1);
    FilterByMask(P, keep, decoded, kept);
    StreamSet * const keptEnds = P.CreateStreamSet(1);
    FilterByMask(P, keep, decodedEnds, keptEnds);
    keeps.push_back(keep);
    StreamSet * const combined = P.CreateStreamSet(21, 1);
    StreamSet * const keep2 = P.CreateStreamSet(1);
    P.CreateKernelCall<CombineSurrogates>(kept, keptEnds, combined, keep2);
    FilterByMask(P, keep2, combined, U21_out);
    keeps.push_back(keep2);
}

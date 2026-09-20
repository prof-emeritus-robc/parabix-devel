/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/basis/s2p_kernel.h>
#include <sstream>
#include <kernel/core/callback.h>
#include <kernel/core/kernel_builder.h>
#include <pablo/pabloAST.h>
#include <pablo/builder.hpp>
#include <pablo/pe_pack.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/pipeline/driver/driver.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <toolchain/toolchain.h>
#include <boost/intrusive/detail/math.hpp>
#include <llvm/Support/CommandLine.h>

using boost::intrusive::detail::floor_log2;

using namespace llvm;

enum TranspositionMode {BytePack, Ideal, Split, Pablo};
static cl::opt<TranspositionMode>
S2P_Mode("S2P_Mode", cl::ValueOptional,
         cl::values(clEnumValN(BytePack, "BytePack", "Bytepack transposition algorithm (default)."),
                    clEnumValN(Ideal, "Ideal", "Idealized transposition algorithm."),
                    clEnumValN(Split, "Split", "Split 3-stage transposition algorithm."),
                    clEnumValN(Pablo, "Pablo", "Pablo transposition algorithm.")),
         cl::cat(codegen::CodeGenOptions), cl::init(BytePack));


namespace kernel {

void s2p_step(KernelBuilder & b, Value * s0, Value * s1, Value * hi_mask, unsigned shift, Value * &p0, Value * &p1) {
    Value * t0 = b.hsimd_packh(16, s0, s1);
    Value * t1 = b.hsimd_packl(16, s0, s1);
    p0 = b.simd_if(1, hi_mask, t0, b.simd_srli(16, t1, shift));
    p1 = b.simd_if(1, hi_mask, b.simd_slli(16, t0, shift), t1);
}

void s2p_bitpairs(KernelBuilder & b, Value * input[], Value * bitpairs[]) {
    Constant * himask2 = b.simd_himask(2);
    for (unsigned i = 0; i < 4; i++) {
        Value * s0 = input[2 * i];
        Value * s1 = input[2 * i + 1];
        s2p_step(b, s0, s1, himask2, 1, bitpairs[2*i], bitpairs[2*i+1]);
    }
}

void s2p_bitquads(KernelBuilder & b, Value * bitpairs[], Value * bitquads[]) {
    Value * bit66442200[4];
    Value * bit77553311[4];
    for (unsigned i = 0; i < 4; i++) {
        bit77553311[i] = bitpairs[2*i];
        bit66442200[i] = bitpairs[2*i + 1];
    }
    Value * bit44440000[2];
    Value * bit66662222[2];
    Value * bit55551111[2];
    Value * bit77773333[2];
    Constant * himask4 = b.simd_himask(4);
    for (unsigned j = 0; j < 2; j++) {
        s2p_step(b, bit66442200[2*j], bit66442200[2*j+1],
                 himask4, 2, bit66662222[j], bit44440000[j]);
        bitquads[j] = bit44440000[j];
        bitquads[4+j] = bit66662222[j];
        s2p_step(b, bit77553311[2*j], bit77553311[2*j+1],
                 himask4, 2, bit77773333[j], bit55551111[j]);
        bitquads[2+j] = bit55551111[j];
        bitquads[6+j] = bit77773333[j];
    }
}

void s2p_completion_from_quads(KernelBuilder & b, Value * bitquads[], Value * output[]) {
    Constant * himask8 = b.simd_himask(8);
    for (unsigned i = 0; i < 4; i++) {
        s2p_step(b, bitquads[2*i], bitquads[2*i + 1], himask8, 4, output[i+4], output[i]);
    }
}

void s2p_completion_from_pairs(KernelBuilder & b, Value * bitpairs[], Value * output[]) {
    Value * bitquads[8];
    s2p_bitquads(b, bitpairs, bitquads);
    s2p_completion_from_quads(b, bitquads, output);
}

void s2p(KernelBuilder & b, Value * input[], Value * output[]) {
    Value * bitpairs[8];
    s2p_bitpairs(b, input, bitpairs);
    s2p_completion_from_pairs(b, bitpairs, output);
}

/* Alternative transposition model, but small field width packs are problematic. */
void s2p_ideal(KernelBuilder & b, Value * input[], Value * output[]) {
    Value * hi_nybble[4];
    Value * lo_nybble[4];
    for (unsigned i = 0; i<4; i++) {
        Value * s0 = input[2*i];
        Value * s1 = input[2*i+1];
        hi_nybble[i] = b.hsimd_packh(8, s0, s1);
        lo_nybble[i] = b.hsimd_packl(8, s0, s1);
    }
    Value * pair76[2];
    Value * pair54[2];
    Value * pair32[2];
    Value * pair10[2];
    for (unsigned i = 0; i<2; i++) {
        pair76[i] = b.hsimd_packh(4, hi_nybble[2*i], hi_nybble[2*i+1]);
        pair54[i] = b.hsimd_packl(4, hi_nybble[2*i], hi_nybble[2*i+1]);
        pair32[i] = b.hsimd_packh(4, lo_nybble[2*i], lo_nybble[2*i+1]);
        pair10[i] = b.hsimd_packl(4, lo_nybble[2*i], lo_nybble[2*i+1]);
    }
    output[7] = b.hsimd_packh(2, pair76[0], pair76[1]);
    output[6] = b.hsimd_packl(2, pair76[0], pair76[1]);
    output[5] = b.hsimd_packh(2, pair54[0], pair54[1]);
    output[4] = b.hsimd_packl(2, pair54[0], pair54[1]);
    output[3] = b.hsimd_packh(2, pair32[0], pair32[1]);
    output[2] = b.hsimd_packl(2, pair32[0], pair32[1]);
    output[1] = b.hsimd_packh(2, pair10[0], pair10[1]);
    output[0] = b.hsimd_packl(2, pair10[0], pair10[1]);
}

// Transposition of each group of 64 bits.
Value * s2p_bytes(KernelBuilder & b, Value * r) {
    Value * b7531 = b.simd_select_hi(2, r);
    Value * b6420 = b.simd_select_lo(2, r);
    Value * b7531_2 = b.simd_slli(16, b7531, 7);
    Value * b6420_2 = b.simd_srli(16, b7531, 7);
    Value * pairs = b.simd_if(1, b.simd_himask(16), b.simd_or(b7531, b7531_2), b.simd_or(b6420, b6420_2));
    Value * b7362 = b.simd_select_hi(4, pairs);
    Value * b5140 = b.simd_select_lo(4, pairs);
    Value * b7362_2 = b.simd_slli(32, b7362, 14);
    Value * b5140_2 = b.simd_srli(32, b5140, 14);
    Value * quads = b.simd_if(1, b.simd_himask(32), b.simd_or(b7362, b7362_2), b.simd_or(b5140, b5140_2));
    Value * b7654 = b.simd_select_hi(8, quads);
    Value * b3210 = b.simd_select_lo(8, quads);
    Value * b7654_2 = b.simd_slli(64, b7654, 28);
    Value * b3210_2 = b.simd_srli(64, b3210, 28);
    return b.simd_if(1, b.simd_himask(64), b.simd_or(b7654, b7654_2), b.simd_or(b3210, b3210_2));
}

void S2PKernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * s2pLoop = b.CreateBasicBlock("s2pLoop");
    BasicBlock * s2pBody = nullptr;     // conditional block dependent on mZeroMask
    BasicBlock * s2pStore =  nullptr;   // conditional block dependent on mZeroMask
    BasicBlock * s2pDone = b.CreateBasicBlock("s2pDone");
    Constant * const sz_ZERO = b.getSize(0);
    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    const StreamSet * byteStreamSet = b.getInputStreamSet("byteStream");
    const auto fieldWidth = byteStreamSet->getFieldWidth();

    b.CreateBr(s2pLoop);

    b.SetInsertPoint(s2pLoop);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 2); // block offset from the base block, e.g. 0, 1, 2, ...
    blockOffsetPhi->addIncoming(sz_ZERO, entry);
    Value * zeroMask = nullptr;
    Value * bytepack[8];
    Value * basisbits[8];
    if (mZeroMask) {
        zeroMask = b.loadInputStreamBlock("mZeroMask", sz_ZERO, blockOffsetPhi);
        for (unsigned i = 0; i < fieldWidth; ++i) {
            basisbits[i] = b.allZeroes();
        }
        s2pBody = b.CreateBasicBlock("s2pBody");
        s2pStore = b.CreateBasicBlock("s2pStore");
        b.CreateCondBr(b.bitblock_any(zeroMask), s2pBody, s2pStore);
        b.SetInsertPoint(s2pBody);
   }
    for (unsigned i = 0; i < 8; i++) {
        bytepack[i] = b.loadInputStreamPack("byteStream", sz_ZERO, b.getInt32(i), blockOffsetPhi);
    }
    if (S2P_Mode == Ideal) {
        s2p_ideal(b, bytepack, basisbits);
    } else {
        s2p(b, bytepack, basisbits);
    }
    if (mZeroMask) {
        b.CreateBr(s2pStore);
        b.SetInsertPoint(s2pStore);
    }
    for (unsigned i = 0; i < fieldWidth; ++i) {
        if (mZeroMask) {
            basisbits[i] = b.simd_and(basisbits[i], zeroMask);
        }
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    blockOffsetPhi->addIncoming(nextBlk, s2pLoop);
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);

    b.CreateCondBr(moreToDo, s2pLoop, s2pDone);

    b.SetInsertPoint(s2pDone);
}

inline Bindings S2PKernel::makeInputBindings(StreamSet * codeUnitStream, StreamSet * zeroMask) {
    if (zeroMask) {
        return {Binding{"byteStream", codeUnitStream, FixedRate(), Principal()},
                Binding{"zeroMask", zeroMask}};
    } else {
        return {Binding{"byteStream", codeUnitStream, FixedRate(), Principal()}};
    }
}

inline Bindings S2PKernel::makeOutputBindings(StreamSet * const BasisBits) {
    return {Binding("basisBits", BasisBits)};
}

inline std::string S2PKernel::makeCacheName(StreamSet * const BasisBits,
                                           StreamSet * zeroMask) {
    std::stringstream ss;
    ss << "s2p";
    if (zeroMask) ss << "z";
    ss << BasisBits->getNumElements();
    if (S2P_Mode == Ideal) {
        ss << "_ideal";
    }
    return ss.str();
}

S2PKernel::S2PKernel(LLVMTypeSystemInterface & ts,
                     StreamSet * const codeUnitStream,
                     StreamSet * const BasisBits,
                     StreamSet * zeroMask)
: MultiBlockKernel(ts, makeCacheName(BasisBits, zeroMask)
, makeInputBindings(codeUnitStream, zeroMask)
, makeOutputBindings(BasisBits)
, {}, {}, {})
, mZeroMask(zeroMask != nullptr) {
    assert (codeUnitStream->getFieldWidth() == BasisBits->getNumElements());
}

class BitPairsKernel final : public MultiBlockKernel {
public:
    BitPairsKernel(LLVMTypeSystemInterface & ts,
              StreamSet * const codeUnitStream,
              StreamSet * const bitPairs);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

class BitQuadsKernel final : public MultiBlockKernel {
public:
    BitQuadsKernel(LLVMTypeSystemInterface & ts,
              StreamSet * const bitPairs,
              StreamSet * const bitQuads);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

class S2P_CompletionKernel final : public MultiBlockKernel {
public:
    S2P_CompletionKernel(LLVMTypeSystemInterface & ts,
                         StreamSet * const bitPacks,
                         StreamSet * const BasisBits,
                         bool completionFromQuads = false);
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
    bool mCompletionFromQuads;
};

void BitPairsKernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * bitPairLoop = b.CreateBasicBlock("bitPairLoop");
    BasicBlock * bitPairFinalize = b.CreateBasicBlock("bitPairFinalize");
    Constant * const ZERO = b.getSize(0);
    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    b.CreateBr(bitPairLoop);
    b.SetInsertPoint(bitPairLoop);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 2);
    blockOffsetPhi->addIncoming(ZERO, entry);
    Value * bytepack[8];
    for (unsigned i = 0; i < 8; i++) {
        bytepack[i] = b.loadInputStreamPack("byteStream", ZERO, b.getInt32(i), blockOffsetPhi);
    }
    Value * bitpairs[8];
    s2p_bitpairs(b, bytepack, bitpairs);
    for (unsigned i = 0; i < 8; ++i) {
        b.storeOutputStreamBlock("bitPairs", b.getInt32(i), blockOffsetPhi, bitpairs[i]);
        //b.CallPrintRegister("bp bitpairs[" + std::to_string(i) + "]", bitpairs[i]);
    }
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    blockOffsetPhi->addIncoming(nextBlk, bitPairLoop);
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);

    b.CreateCondBr(moreToDo, bitPairLoop, bitPairFinalize);
    b.SetInsertPoint(bitPairFinalize);
}

BitPairsKernel::BitPairsKernel(LLVMTypeSystemInterface & ts,
                               StreamSet * const codeUnitStream,
                               StreamSet * const bitPairs)
: MultiBlockKernel(ts, "BitPairs"
, {Binding{"byteStream", codeUnitStream, FixedRate(), Principal()}}
                   , {Binding{"bitPairs", bitPairs, FixedRate(), RoundUpTo(2 * ts.getBitBlockWidth())}}, {}, {}, {}) {
    setStride(2 * ts.getBitBlockWidth());
}

void BitQuadsKernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * bitQuadLoop = b.CreateBasicBlock("bitQuadLoop");
    BasicBlock * bitQuadFinalize = b.CreateBasicBlock("bitQuadFinalize");
    Constant * const ZERO = b.getSize(0);
    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    b.CreateBr(bitQuadLoop);
    b.SetInsertPoint(bitQuadLoop);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 2);
    blockOffsetPhi->addIncoming(ZERO, entry);
    Value * bitpairs[8];
    for (unsigned i = 0; i < 8; i++) {
        bitpairs[i] = b.loadInputStreamBlock("bitPairs", b.getInt32(i), blockOffsetPhi);
        //b.CallPrintRegister("bq bitpairs[" + std::to_string(i) + "]", bitpairs[i]);
    }
    Value * bitquads[8];
    s2p_bitquads(b, bitpairs, bitquads);
    for (unsigned i = 0; i < 8; ++i) {
        b.storeOutputStreamBlock("bitQuads", b.getInt32(i), blockOffsetPhi, bitquads[i]);
        //b.CallPrintRegister("bq bitquads[" + std::to_string(i) + "]", bitquads[i]);
    }
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    blockOffsetPhi->addIncoming(nextBlk, bitQuadLoop);
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);

    b.CreateCondBr(moreToDo, bitQuadLoop, bitQuadFinalize);
    b.SetInsertPoint(bitQuadFinalize);
}

BitQuadsKernel::BitQuadsKernel(LLVMTypeSystemInterface & ts,
                               StreamSet * const bitPairs,
                               StreamSet * const bitQuads)
: MultiBlockKernel(ts, "BitQuads"
, {Binding{"bitPairs", bitPairs, FixedRate(), Principal()}}
                   , {Binding{"bitQuads", bitQuads, FixedRate(), RoundUpTo(2 * ts.getBitBlockWidth())}}, {}, {}, {}) {
    setStride(2 * ts.getBitBlockWidth());
}

void S2P_CompletionKernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * s2pLoop = b.CreateBasicBlock("s2pLoop");
    BasicBlock * s2pFinalize = b.CreateBasicBlock("s2pFinalize");
    Constant * const ZERO = b.getSize(0);
    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    b.CreateBr(s2pLoop);
    b.SetInsertPoint(s2pLoop);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 2); // block offset from the base block, e.g. 0, 1, 2, ...
    blockOffsetPhi->addIncoming(ZERO, entry);
    Value * basisbits[8];
    Value * bitPacks[8];
    for (unsigned i = 0; i < 8; i++) {
        bitPacks[i] = b.loadInputStreamBlock("bitPacks", b.getInt32(i), blockOffsetPhi);
    }
    if (mCompletionFromQuads) {
        s2p_completion_from_quads(b, bitPacks, basisbits);
    } else {
        s2p_completion_from_pairs(b, bitPacks, basisbits);
    }
    for (unsigned i = 0; i < 8; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    blockOffsetPhi->addIncoming(nextBlk, s2pLoop);
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);
    b.CreateCondBr(moreToDo, s2pLoop, s2pFinalize);
    b.SetInsertPoint(s2pFinalize);
}

S2P_CompletionKernel::S2P_CompletionKernel(LLVMTypeSystemInterface & ts,
                                           StreamSet * const bitPacks,
                                           StreamSet * const BasisBits,
                                           bool completionFromQuads)
    : MultiBlockKernel(ts, completionFromQuads ? "S2PfromQuads" : "S2PfromPairs",
                       {Binding{"bitPacks", bitPacks, FixedRate(), Principal()}},
                       {Binding{"basisBits", BasisBits}}, {}, {}, {}), mCompletionFromQuads(completionFromQuads) {
        setStride(2 * ts.getBitBlockWidth());
    }

void Staged_S2P(PipelineBuilder &P,
                StreamSet * ByteStream, StreamSet * BasisBits,
                bool completionFromQuads) {
    StreamSet * BitPairs = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<BitPairsKernel>(ByteStream, BitPairs);
    if (completionFromQuads) {
        StreamSet * BitQuads = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BitQuadsKernel>(BitPairs, BitQuads);
        P.CreateKernelCall<S2P_CompletionKernel>(BitQuads, BasisBits, completionFromQuads);
    } else {
        P.CreateKernelCall<S2P_CompletionKernel>(BitPairs, BasisBits, completionFromQuads);
    }
    P.AssertEqualLength(BasisBits, ByteStream);
}

void Selected_S2P(PipelineBuilder & P, StreamSet * ByteStream, StreamSet * BasisBits) {
    if (S2P_Mode == Pablo) {
        P.CreateKernelCall<S2P_PabloKernel>(ByteStream, BasisBits);
    } else if (S2P_Mode == Split) {
        Staged_S2P(P, ByteStream, BasisBits);
    } else {
        P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    }
}

S2P_i21_3xi8::S2P_i21_3xi8(LLVMTypeSystemInterface & ts, StreamSet * const i32Stream, StreamSet * const i8stream0, StreamSet * const i8stream1, StreamSet * const i8stream2)
: MultiBlockKernel(ts, "s2p_i21_3xi8",
{Binding{"i32Stream", i32Stream, FixedRate(), Principal()}},
                   {Binding{"i8stream0", i8stream0}, Binding{"i8stream1", i8stream1}, Binding{"i8stream2", i8stream2}}, {}, {}, {})  {}

void S2P_i21_3xi8::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * processBlock = b.CreateBasicBlock("s2p21_loop");
    BasicBlock * s2pDone = b.CreateBasicBlock("s2p21_done");
    Constant * const ZERO = b.getSize(0);

    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    b.CreateBr(processBlock);

    b.SetInsertPoint(processBlock);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 3); // block offset from the base block, e.g. 0, 1, 2, ...
    blockOffsetPhi->addIncoming(ZERO, entry);
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);
    Value * UTF32units[4];
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 4; j++) {
            UTF32units[j] = b.loadInputStreamPack("i32Stream", ZERO, b.getInt32(4 * i + j), blockOffsetPhi);
        }
        Value * u32lo16_0 = b.hsimd_packl(32, UTF32units[0], UTF32units[1]);
        Value * u32lo16_1 = b.hsimd_packl(32, UTF32units[2], UTF32units[3]);
        Value * u32hi16_0 = b.hsimd_packh(32, UTF32units[0], UTF32units[1]);
        Value * u32hi16_1 = b.hsimd_packh(32, UTF32units[2], UTF32units[3]);
        Value * u32byte0 = b.bitCast(b.hsimd_packl(16, u32lo16_0, u32lo16_1));
        Value * u32byte1 = b.bitCast(b.hsimd_packh(16, u32lo16_0, u32lo16_1));
        Value * u32byte2 = b.bitCast(b.hsimd_packl(16, u32hi16_0, u32hi16_1));
        Value * idx = b.getInt32(i);
        b.storeOutputStreamPack("i8stream0", ZERO, idx, blockOffsetPhi, u32byte0);
        b.storeOutputStreamPack("i8stream1", ZERO, idx, blockOffsetPhi, u32byte1);
        b.storeOutputStreamPack("i8stream2", ZERO, idx, blockOffsetPhi, u32byte2);
    }
    BasicBlock * const processBlockExit = b.GetInsertBlock();
    blockOffsetPhi->addIncoming(nextBlk, processBlockExit);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(s2pDone);
}

S2P_3xi8_21xi1::S2P_3xi8_21xi1(LLVMTypeSystemInterface & ts, StreamSet * const i8stream0, StreamSet * const i8stream1, StreamSet * const i8stream2, StreamSet * const BasisBits)
: MultiBlockKernel(ts, "s2p_3xi8_21xi1",
{Binding{"i8stream0", i8stream0}, Binding{"i8stream1", i8stream1}, Binding{"i8stream2", i8stream2}},
{Binding{"basisBits", BasisBits}}, {}, {}, {})  {}

void S2P_3xi8_21xi1::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * processBlock = b.CreateBasicBlock("s2p21_loop");
    BasicBlock * write13_zeroes = b.CreateBasicBlock("write13_zeroes");
    BasicBlock * continue_s2p = b.CreateBasicBlock("continue_s2p");
    BasicBlock * write5_zeroes = b.CreateBasicBlock("write5_zeroes");
    BasicBlock * finish_s2p = b.CreateBasicBlock("finish_s2p");
    BasicBlock * s2pDone = b.CreateBasicBlock("s2p21_done");
    Constant * const ZERO = b.getSize(0);
    Constant * ZERO_BLOCK = b.allZeroes();

    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    b.CreateBr(processBlock);

    b.SetInsertPoint(processBlock);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 4); // block offset from the base block, e.g. 0, 1, 2, ...
    blockOffsetPhi->addIncoming(ZERO, entry);
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);
    Value * u32byte0[8];
    Value * u32byte1[8];
    Value * u32byte2[8];
    for (unsigned i = 0; i < 8; i++) {
        Value * idx = b.getInt32(i);
        u32byte0[i] = b.loadInputStreamPack("i8stream0", ZERO, idx, blockOffsetPhi);
        u32byte1[i] = b.loadInputStreamPack("i8stream1", ZERO, idx, blockOffsetPhi);
        u32byte2[i] = b.loadInputStreamPack("i8stream2", ZERO, idx, blockOffsetPhi);
    }
    Value * basisbits[24];
    Value * anybyte1 = u32byte1[0];
    Value * anybyte2 = u32byte2[0];
    for (unsigned i = 1; i < 8; i++) {
        anybyte1 = b.simd_or(anybyte1, u32byte1[i]);
        anybyte2 = b.simd_or(anybyte2, u32byte2[i]);
    }
    Value * anybyte1or2 = b.simd_or(anybyte1, anybyte2, "anybyte1or2");
    s2p(b, u32byte0, basisbits);
    for (unsigned i = 0; i < 8; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    b.CreateCondBr(b.bitblock_any(anybyte1or2), continue_s2p, write13_zeroes);
    b.SetInsertPoint(write13_zeroes);
    for (unsigned i = 8; i < 21; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, ZERO_BLOCK);
    }
    blockOffsetPhi->addIncoming(nextBlk, write13_zeroes);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(continue_s2p);
    s2p(b, u32byte1, &basisbits[8]);
    for (unsigned i = 8; i < 16; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    b.CreateCondBr(b.bitblock_any(anybyte2), finish_s2p, write5_zeroes);
    b.SetInsertPoint(write5_zeroes);
    for (unsigned i = 16; i < 21; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, ZERO_BLOCK);
    }
    blockOffsetPhi->addIncoming(nextBlk, write5_zeroes);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(finish_s2p);
    s2p(b, u32byte2, &basisbits[16]);
    for (unsigned i = 16; i < 21; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    BasicBlock * const processBlockExit = b.GetInsertBlock();
    blockOffsetPhi->addIncoming(nextBlk, processBlockExit);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(s2pDone);
}

S2P_21Kernel::S2P_21Kernel(LLVMTypeSystemInterface & ts, StreamSet * const codeUnitStream, StreamSet * const BasisBits)
: MultiBlockKernel(ts, "s2p_21",
{Binding{"codeUnitStream", codeUnitStream, FixedRate(), Principal()}},
    {Binding{"basisBits", BasisBits}}, {}, {}, {})  {}

void S2P_21Kernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    BasicBlock * entry = b.GetInsertBlock();
    BasicBlock * processBlock = b.CreateBasicBlock("s2p21_loop");
    BasicBlock * write13_zeroes = b.CreateBasicBlock("write13_zeroes");
    BasicBlock * continue_s2p = b.CreateBasicBlock("continue_s2p");
    BasicBlock * write5_zeroes = b.CreateBasicBlock("write5_zeroes");
    BasicBlock * finish_s2p = b.CreateBasicBlock("finish_s2p");
    BasicBlock * s2pDone = b.CreateBasicBlock("s2p21_done");
    Constant * const ZERO = b.getSize(0);
    Constant * ZERO_BLOCK = b.allZeroes();

    Value * numOfBlocks = numOfStrides;
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(floor_log2(getStride()/b.getBitBlockWidth())));
    }
    b.CreateBr(processBlock);

    b.SetInsertPoint(processBlock);
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 4); // block offset from the base block, e.g. 0, 1, 2, ...
    blockOffsetPhi->addIncoming(ZERO, entry);
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);
    Value * u32byte0[8];
    Value * u32byte1[8];
    Value * u32byte2[8];
    for (unsigned i = 0; i < 8; i++) {
        Value * UTF32units[4];
        for (unsigned j = 0; j < 4; j++) {
            UTF32units[j] = b.loadInputStreamPack("codeUnitStream", ZERO, b.getInt32(4 * i + j), blockOffsetPhi);
        }
        Value * u32lo16_0 = b.hsimd_packl(32, UTF32units[0], UTF32units[1]);
        Value * u32lo16_1 = b.hsimd_packl(32, UTF32units[2], UTF32units[3]);
        Value * u32hi16_0 = b.hsimd_packh(32, UTF32units[0], UTF32units[1]);
        Value * u32hi16_1 = b.hsimd_packh(32, UTF32units[2], UTF32units[3]);
        u32byte0[i] = b.hsimd_packl(16, u32lo16_0, u32lo16_1);
        u32byte1[i] = b.hsimd_packh(16, u32lo16_0, u32lo16_1);
        u32byte2[i] = b.hsimd_packl(16, u32hi16_0, u32hi16_1);
    #ifdef VALIDATE_U32
        //  Validation should ensure that none of the high 11 bits are
        //  set for any UTF-32 code unit.   We simply combine the bits
        //  of code units together with bitwise-or, and then perform a
        //  single check at the end.
        u32_check = simd_or(u32_check, simd_or(u32hi16_0, u32hi16_1));
    #endif
    }
    Value * basisbits[24];
    Value * anybyte1 = u32byte1[0];
    Value * anybyte2 = u32byte2[0];
    for (unsigned i = 1; i < 8; i++) {
        anybyte1 = b.simd_or(anybyte1, u32byte1[i]);
        anybyte2 = b.simd_or(anybyte2, u32byte2[i]);
    }
    Value * anybyte1or2 = b.simd_or(anybyte1, anybyte2, "anybyte1or2");
    s2p(b, u32byte0, basisbits);
    for (unsigned i = 0; i < 8; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    b.CreateCondBr(b.bitblock_any(anybyte1or2), continue_s2p, write13_zeroes);
    b.SetInsertPoint(write13_zeroes);
    for (unsigned i = 8; i < 21; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, ZERO_BLOCK);
    }
    blockOffsetPhi->addIncoming(nextBlk, write13_zeroes);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(continue_s2p);
    s2p(b, u32byte1, &basisbits[8]);
    for (unsigned i = 8; i < 16; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    b.CreateCondBr(b.bitblock_any(anybyte2), finish_s2p, write5_zeroes);
    b.SetInsertPoint(write5_zeroes);
    for (unsigned i = 16; i < 21; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, ZERO_BLOCK);
    }
    blockOffsetPhi->addIncoming(nextBlk, write5_zeroes);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(finish_s2p);
    s2p(b, u32byte2, &basisbits[16]);
    for (unsigned i = 16; i < 21; ++i) {
        b.storeOutputStreamBlock("basisBits", b.getInt32(i), blockOffsetPhi, basisbits[i]);
    }
    BasicBlock * const processBlockExit = b.GetInsertBlock();
    blockOffsetPhi->addIncoming(nextBlk, processBlockExit);
    b.CreateCondBr(moreToDo, processBlock, s2pDone);
    b.SetInsertPoint(s2pDone);
}

void S2P_PabloKernel::generatePabloMethod() {
    pablo::PabloBlock * const pb = getEntryScope();
    const unsigned steps = floor_log2(mCodeUnitWidth);
    SmallVector<std::vector<PabloAST *>, 8> streamSet(steps + 1);
    for (unsigned i = 0; i <= steps; i++) {
        streamSet[i].resize(1<<i);
    }
    streamSet[0][0] = pb->createExtract(getInputStreamVar("codeUnitStream"), pb->getInteger(0));
    unsigned streamWidth = mCodeUnitWidth;
    for (unsigned i = 1; i <= steps; i++) {
        for (unsigned j = 0; j < streamSet[i-1].size(); j++) {
            auto strm = streamSet[i-1][j];
            streamSet[i][2*j] = pb->createPackL(pb->getInteger(streamWidth), strm);
            streamSet[i][2*j+1] = pb->createPackH(pb->getInteger(streamWidth), strm);
        }
        streamWidth = streamWidth/2;
    }
    for (unsigned bit = 0; bit < mCodeUnitWidth; bit++) {
        pb->createAssign(pb->createExtract(getOutputStreamVar("basisBits"), pb->getInteger(bit)), streamSet[steps][bit]);
    }
}

S2P_PabloKernel::S2P_PabloKernel(LLVMTypeSystemInterface & ts, StreamSet * const codeUnitStream, StreamSet * const BasisBits)
: PabloKernel(ts, "s2p_pablo" + std::to_string(codeUnitStream->getFieldWidth()),
// input
{Binding{"codeUnitStream", codeUnitStream}},
// output
{Binding{"basisBits", BasisBits}}),
mCodeUnitWidth(codeUnitStream->getFieldWidth()) {
    assert (codeUnitStream->getFieldWidth() == BasisBits->getNumElements());
}

}

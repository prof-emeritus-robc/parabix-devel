#pragma once

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
*/

#include <idisa/idisa_sse_builder.h>
#include <toolchain/toolchain.h>

namespace IDISA {

constexpr unsigned AVX_width = 256;

constexpr unsigned AVX512_width = 512;

class IDISA_AVX_Builder : public IDISA_SSE2_Builder {
public:
    static const unsigned NativeBitBlockWidth = AVX_width;
    IDISA_AVX_Builder(llvm::LLVMContext & C, const FeatureSet &featureSet, unsigned vectorWidth, unsigned laneWidth);

    virtual std::string getBuilderUniqueName() override;

    llvm::Value * hsimd_signmask(unsigned fw, llvm::Value * a) override;
    llvm::Value * CreateZeroHiBitsFrom(llvm::Value * bits, llvm::Value * pos, const llvm::Twine Name = "") override;
    llvm::Value * CreatePextract(llvm::Value * v, llvm::Value * mask, const llvm::Twine Name = "") override;
    llvm::Value * CreatePdeposit(llvm::Value * v, llvm::Value * mask, const llvm::Twine Name = "") override;

    ~IDISA_AVX_Builder() override {}

};

class IDISA_AVX2_Builder : public IDISA_AVX_Builder {
public:
    static const unsigned NativeBitBlockWidth = AVX_width;
    IDISA_AVX2_Builder(llvm::LLVMContext & C, const FeatureSet & featureSet, unsigned vectorWidth, unsigned laneWidth);

    virtual std::string getBuilderUniqueName() override;
    llvm::Value * hsimd_packh(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packl(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packus(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packss(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * esimd_mergeh(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * esimd_mergel(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packh_in_lanes(unsigned lanes, unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packl_in_lanes(unsigned lanes, unsigned fw, llvm::Value * a, llvm::Value * b) override;
    std::pair<llvm::Value *, llvm::Value *> bitblock_add_with_carry(llvm::Value * a, llvm::Value * b, llvm::Value * carryin) override;
    std::pair<llvm::Value *, llvm::Value *> bitblock_advance(llvm::Value * a, llvm::Value * shiftin, unsigned shift) override;
    std::pair<llvm::Value *, llvm::Value *> bitblock_indexed_advance(llvm::Value * a, llvm::Value * index_strm, llvm::Value * shiftin, unsigned shift) override;
    llvm::Value * hsimd_signmask(unsigned fw, llvm::Value * a) override;
    llvm::Value * mvmd_shuffle2(unsigned fw, llvm::Value * table0, llvm::Value * table1, llvm::Value * index_vector) override;
    llvm::Value * mvmd_compress(unsigned fw, llvm::Value * a, llvm::Value * select_mask) override;
    llvm::Value * mvmd_expand(unsigned fw, llvm::Value * a, llvm::Value * select_mask) override;
    llvm::Value * mvmd_srl(unsigned fw, llvm::Value * a, llvm::Value * shift, const bool safe = false) override;
    llvm::Value * mvmd_sll(unsigned fw, llvm::Value * a, llvm::Value * shift, const bool safe = false) override;
    llvm::Value * mvmd_shuffle(unsigned fw, llvm::Value * data_table, llvm::Value * index_vector) override;
    std::vector<llvm::Value *> simd_pext(unsigned fw, std::vector<llvm::Value *> v, llvm::Value * extract_mask) override;
    llvm::Value * simd_pdep(unsigned fw, llvm::Value * v, llvm::Value * deposit_mask) override;

    ~IDISA_AVX2_Builder() override {}
};

class IDISA_AVX512F_Builder : public IDISA_AVX2_Builder {
public:
    static const unsigned NativeBitBlockWidth = AVX512_width;
    IDISA_AVX512F_Builder(llvm::LLVMContext & C, const FeatureSet & featureSet, unsigned vectorWidth, unsigned laneWidth);

    virtual std::string getBuilderUniqueName() override;
    llvm::Value * hsimd_packh(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packl(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packus(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * hsimd_packss(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * esimd_mergeh(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * esimd_mergel(unsigned fw, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * simd_popcount(unsigned fw, llvm::Value * a) override;
    llvm::Value * mvmd_slli(unsigned fw, llvm::Value * a, unsigned shift) override;
    llvm::Value * mvmd_dslli(unsigned fw, llvm::Value * a, llvm::Value * b, unsigned shift) override;
    llvm::Value * hsimd_signmask(unsigned fw, llvm::Value * a) override;
    llvm::Value * mvmd_shuffle(unsigned fw, llvm::Value * data_table, llvm::Value * index_vector) override;
    llvm::Value * mvmd_shuffle2(unsigned fw, llvm::Value * table0, llvm::Value * table1, llvm::Value * index_vector) override;
    llvm::Value * mvmd_compress(unsigned fw, llvm::Value * a, llvm::Value * select_mask) override;
    llvm::Value * mvmd_expand(unsigned fw, llvm::Value * a, llvm::Value * select_mask) override;
    llvm::Value * mvmd_srl(unsigned fw, llvm::Value * a, llvm::Value * shift, const bool safe) override;
    llvm::Value * mvmd_sll(unsigned fw, llvm::Value * a, llvm::Value * shift, const bool safe) override;
    llvm::Value * simd_if(unsigned fw, llvm::Value * cond, llvm::Value * a, llvm::Value * b) override;
    llvm::Value * simd_ternary(unsigned char mask, llvm::Value * a, llvm::Value * b, llvm::Value * c) override;
    std::pair<llvm::Value *, llvm::Value *> bitblock_advance(llvm::Value * a, llvm::Value * shiftin, unsigned shift) override;

    ~IDISA_AVX512F_Builder() override {
    }

};

}

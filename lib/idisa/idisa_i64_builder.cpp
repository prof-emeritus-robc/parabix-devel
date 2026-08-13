/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <idisa/idisa_i64_builder.h>
#include <sstream>

using namespace llvm;

namespace IDISA {
    
std::string IDISA_I64_Builder::getBuilderUniqueName() {
    std::stringstream uname;
    uname << "C";
    if (mBitBlockWidth != 64) {
        uname << "_" << mBitBlockWidth;
    }
    if (IDISA::IDISA_Experiment != "") {
        uname << IDISA::IDISA_Experiment;
    }
    return uname.str();
}

Value * IDISA_I64_Builder::hsimd_packh(unsigned fw, Value * a, Value * b) {
    unsigned vec_width = getVectorBitWidth(a);
    Value * a_ = a;
    Value * b_ = b;
    for (unsigned w = fw; w < vec_width; w *= 2) {
        Value * himask_odd = simd_and(simd_himask(vec_width, w), simd_himask(vec_width, 2*w));  // high half of odd fields
        Value * himask_even = simd_and(simd_himask(vec_width, w), simd_lomask(vec_width, 2*w));  // high half of even fields
        b_ = simd_or(simd_and(b_, himask_odd), simd_slli(vec_width, simd_and(b_, himask_even), w/2));
        a_ = simd_or(simd_and(a_, himask_odd), simd_slli(vec_width, simd_and(a_, himask_even), w/2));
    }
    return simd_or(b_, simd_srli(vec_width, a_, vec_width/2));
}

Value * IDISA_I64_Builder::hsimd_packl(unsigned fw, Value * a, Value * b) {
    unsigned vec_width = getVectorBitWidth(a);
    Value * a_ = a;
    Value * b_ = b;
    for (unsigned w = fw; w < vec_width; w *= 2) {
        Value * lomask_odd = simd_and(simd_lomask(vec_width, w), simd_himask(vec_width, 2*w));  // high half of odd fields
        Value * lomask_even = simd_and(simd_lomask(vec_width, w), simd_lomask(vec_width, 2*w));  // high half of even fields
        b_ = simd_or(simd_and(b_, lomask_even), simd_srli(vec_width, simd_and(b_, lomask_odd), w/2));
        a_ = simd_or(simd_and(a_, lomask_even), simd_srli(vec_width, simd_and(a_, lomask_odd), w/2));
    }
    return simd_or(simd_slli(vec_width, b_, vec_width/2), a_);
}

}

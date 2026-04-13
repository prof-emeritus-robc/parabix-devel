/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "normalize.h"

StreamSet * applyNormalization(PipelineBuilder & P,
                               StreamSet * BasisBits,
                               NormalizationMode mode) {
    
    //applyNFC() and applyNFD() to lib/kernel/unicode/normalization.cpp                            
    // if (mode == NormNFC) return applyNFC(P, BasisBits);
    // if (mode == NormNFD) return applyNFD(P, BasisBits);

    return BasisBits;  // pass-through until library functions are available
}

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

}

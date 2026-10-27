/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <pablo/parse/kernel_signature.h>

#include <llvm/Support/raw_ostream.h>
#include <pablo/parse/pablo_type.h>

namespace pablo {
namespace parse {

std::string PabloKernelSignature::asString() const noexcept {
    std::string str;
    llvm::raw_string_ostream out(str);
    out << "kernel " << getName() << " :: [";
    for (size_t i = 0; i < getInputBindings().size(); ++i) {
        std::string name;
        pablo::parse::PabloType * type;
        std::tie(name, type) = getInputBindings()[i];
        out << type->asString() << " " << name;
        if (i != getInputBindings().size() - 1) {
            out << ", ";
        }
    }
    out << "] -> [";
    for (size_t i = 0; i < getOutputBindings().size(); ++i) {
        std::string name;
        pablo::parse::PabloType * type;
        std::tie(name, type) = getOutputBindings()[i];
        out << type->asString() << " " << name;
        if (i != getOutputBindings().size() - 1) {
            out << ", ";
        }
    }
    out << "]";
    return out.str();
}

} // namespace pablo::parse
} // namespace pablo

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>
namespace pablo { namespace parse { class PabloType; } }

namespace pablo {
namespace parse {

class PabloKernelSignature {
public:
    using SignatureBinding = std::pair<std::string, PabloType *>;
    using SignatureBindings = std::vector<SignatureBinding>;

    PabloKernelSignature(std::string name, SignatureBindings inputBindings, SignatureBindings outputBindings)
    : mName(std::move(name))
    , mInputBindings(std::move(inputBindings))
    , mOutputBindings(std::move(outputBindings))
    {}

    std::string getName() const noexcept {
        return mName;
    }

    SignatureBindings const & getInputBindings() const noexcept {
        return mInputBindings;
    }

    SignatureBindings const & getOutputBindings() const noexcept {
        return mOutputBindings;
    }

    std::string asString() const noexcept;

private:
    std::string         mName;
    SignatureBindings   mInputBindings;
    SignatureBindings   mOutputBindings;
};

} // namespace pablo::parse
} // namespace pablo

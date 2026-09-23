/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <re/transforms/re_transformer.h>
#include <re/transforms/name_intro.h>
#include <re/alphabet/alphabet.h>

namespace re {

struct ReferenceInfo; class RE;

struct FixedReferenceTransformer : public NameIntroduction {
public:
    FixedReferenceTransformer(const ReferenceInfo & info, const cc::Alphabet & alpha = cc::Unicode) :
        NameIntroduction("FixedReferenceTransformer"), mRefInfo(info), mAlphabet(alpha) {}
    RE * transformReference(Reference * r) override;
private:
    const ReferenceInfo & mRefInfo;
    const cc::Alphabet & mAlphabet;
};

}

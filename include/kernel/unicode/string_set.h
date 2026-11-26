/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  StringSet kernels: the longest matches of a set with strings, as ICU
//  matches a UnicodeSet (doc/ldml-entity-groups-plan.md §4).
//
//  For each length k of a match (StringSetSpec::lengths, ascending), output
//  stream i (lengths[i] = k) marks the positions where the longest member of
//  the set matching there has length k: forward, the positions where the match
//  begins; backward (for a before context, matched from the right), those
//  where it ends.  The streams are mutually exclusive.
//
//  The set is given as the terms of re::unambiguousStringClass (with
//  PossessiveChoice::Longest), at most one of which matches at each position:
//  a sequence of sets of characters, and guards (sequences that must not
//  follow it, or backward, precede it).  The sets are in the alphabet of the
//  kernel's basis: codepoints over the 21-bit Unicode basis, or the codes of a
//  multiplexed alphabet over the bits of its code (makeStringSetSpec, code).

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <pablo/pablo.h>
#include <pablo/toolchain/pablo_toolchain.h>
#include <ucd/core/unicode_set.h>

namespace kernel {

struct StringSetSpec {
    struct Term {
        std::vector<UCD::UnicodeSet> seq;
        std::vector<std::vector<UCD::UnicodeSet>> guards;
    };
    std::vector<unsigned> lengths;
    std::vector<Term> terms;
    bool backward = false;
    //  The spec in text (sets as ranges), for the kernel's signature.
    std::string signature() const;
};

//  The spec of the set of the single characters chars and the strings (of two
//  or more characters).  code maps a set of characters to the set of the
//  basis's alphabet that compiles it (e.g. the codes of a multiplexed
//  alphabet); the identity if not given.  Every set it is applied to is chars
//  less the characters with which strings begin (backward: end), or a single
//  character.
StringSetSpec makeStringSetSpec(const UCD::UnicodeSet & chars, const std::vector<std::u32string> & strings,
                                bool backward = false,
                                const std::function<UCD::UnicodeSet (const UCD::UnicodeSet &)> & code = nullptr);

class StringSetKernel final : public pablo::PabloKernel {
public:
    //  Basis: the 21-bit Unicode basis, or the bits of a code (the sets of the
    //  spec as its codes); Lengths: one stream per length of the spec.
    StringSetKernel(LLVMTypeSystemInterface & ts, StringSetSpec spec, StreamSet * Basis, StreamSet * Lengths);
protected:
    bool hasSignature() const override {return true;}
    llvm::StringRef getSignature() const override {return mSignature;}
    void generatePabloMethod() override;
private:
    const StringSetSpec mSpec;
    const std::string mSignature;
};

}

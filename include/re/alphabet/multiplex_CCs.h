/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <string>
#include <utility>
#include <vector>
#include <re/alphabet/alphabet.h>

namespace re { class CC; }

namespace cc {

using CC_Set = std::vector<re::CC *>;

//
// A multiplexed alphabet partitions the codepoints of a source alphabet into
// the exclusive sets of a set of CCs: the sets of codepoints that belong to
// exactly the same CCs.  Each exclusive set has a code: 0 for the codepoints
// of no CC, and 1, 2, ... for the others.  The codes are represented by
// ceil(log2(n + 1)) basis CCs (getMultiplexedCCs) for n exclusive sets other
// than code 0.  Any union of exclusive sets (for example, a union, an
// intersection or a complement of the CCs, which may include code 0) is
// expressible as a CC of codes (transformCC).
//
class MultiplexedAlphabet final : public Alphabet {
public:
    static inline bool classof(const Alphabet * a) {
        return a->getClassTypeId() == ClassTypeId::MultiplexedAlphabet;
    }
    static inline bool classof(const void *) {return false;}
    
    const unsigned getSize() const override {return mCodeCount;}

    const Alphabet * getSourceAlphabet() const {
        return mSourceAlphabet;
    }
    
    const CC_Set & getMultiplexedCCs() const {
        return mMultiplexedCCs;
    }

    // The ranges of the partition, in increasing order: each range begins at
    // the first codepoint of its pair, ends before the next range (or at
    // UNICODE_MAX), and has the code of the second.
    const std::vector<std::pair<UCD::codepoint_t, unsigned>> & getPartition() const {
        return mPartition;
    }

    // The code of the exclusive set of a codepoint.
    unsigned codeOf(UCD::codepoint_t cp) const;

    // The codes of the exclusive sets whose union is the set s, in increasing
    // order, or false if s is not a union of exclusive sets.
    bool codes(const UCD::UnicodeSet & s, std::vector<unsigned> & codes) const;

    bool expressible(const UCD::UnicodeSet & s) const;

    // The CC of the codes of a CC of the source alphabet; a fatal error if it
    // is not expressible.
    re::CC * transformCC(const re::CC * sourceCC) const;

    // The partition (its ranges and their codes), as a string for signatures:
    // two multiplexed alphabets with the same signature give the same codes.
    const std::string & getPartitionSignature() const {
        return mPartitionSignature;
    }
    
    friend MultiplexedAlphabet * makeMultiplexedAlphabet(const std::string alphabetName, const CC_Set CCs);
protected:
    MultiplexedAlphabet(const std::string alphabetName, const CC_Set CCs);

private:
    const Alphabet * mSourceAlphabet;
    const CC_Set mUnicodeSets;
    std::vector<std::pair<UCD::codepoint_t, unsigned>> mPartition;
    unsigned mCodeCount;
    CC_Set mMultiplexedCCs;
    std::string mPartitionSignature;
};

inline MultiplexedAlphabet * makeMultiplexedAlphabet(const std::string alphabetName, const CC_Set CCs) {
    return new MultiplexedAlphabet(alphabetName, CCs);
}
}


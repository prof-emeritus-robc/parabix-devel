/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <re/alphabet/alphabet.h>

namespace re { class CC; }

namespace cc {

using CC_Set = std::vector<re::CC *>;

class MultiplexedAlphabet final : public Alphabet {
public:
    static inline bool classof(const Alphabet * a) {
        return a->getClassTypeId() == ClassTypeId::MultiplexedAlphabet;
    }
    static inline bool classof(const void *) {return false;}
    
    const unsigned getSize() const override {return mUnicodeSets.size() + 1;}

    const Alphabet * getSourceAlphabet() const {
        return mSourceAlphabet;
    }
    
    const CC_Set & getMultiplexedCCs() const {
        return mMultiplexedCCs;
    }
    
    re::CC * transformCC(const re::CC * sourceCC) const;
    
    friend MultiplexedAlphabet * makeMultiplexedAlphabet(const std::string alphabetName, const CC_Set CCs);
protected:
    MultiplexedAlphabet(const std::string alphabetName, const CC_Set CCs);

private:
    const Alphabet * mSourceAlphabet;
    const CC_Set mUnicodeSets;
    std::vector<std::vector<unsigned>> mExclusiveSetIDs;
    CC_Set mMultiplexedCCs;

    unsigned long findTargetCCIndex(const re::CC * sourceCC) const;
};

inline MultiplexedAlphabet * makeMultiplexedAlphabet(const std::string alphabetName, const CC_Set CCs) {
    return new MultiplexedAlphabet(alphabetName, CCs);
}
}



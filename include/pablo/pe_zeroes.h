/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/pabloAST.h>

namespace pablo {

class Zeroes final : public PabloAST {
    friend class PabloBlock;
    friend class PabloKernel;
public:
    static inline bool classof(const PabloAST * expr) {
        assert (expr);
        return expr->getClassTypeId() == ClassTypeId::Zeroes;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Zeroes() {
    }
    inline bool operator==(const Zeroes &) const {
        return true;
    }
    virtual bool operator==(const PabloAST & other) const {
        return llvm::isa<Zeroes>(other);
    }
protected:
    Zeroes(llvm::Type * const type)
    : PabloAST(ClassTypeId::Zeroes, type) {
    }
};

}


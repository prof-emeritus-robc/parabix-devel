/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/ast/pabloAST.h>

namespace pablo {

class Ones final : public PabloAST {
    friend class PabloBlock;
    friend class PabloKernel;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Ones;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Ones() {
    }
    inline bool operator==(const Ones &) const {
        return true;
    }
    virtual bool operator==(const PabloAST & other) const {
        return llvm::isa<Ones>(other);
    }
protected:
    Ones(llvm::Type * const type)
    : PabloAST(ClassTypeId::Ones, type) {
    }
};

}


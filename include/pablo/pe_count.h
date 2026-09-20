/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/pabloAST.h>

namespace pablo {

class Count final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Count;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Count() {
    }
    inline PabloAST * getExpr() const {
        return getOperand(0);
    }
protected:
    explicit Count(PabloAST * expr, const String * name, llvm::Type * type)
    : Statement(ClassTypeId::Count, type, {expr}, name) {

    }
private:
};

}


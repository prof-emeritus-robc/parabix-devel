/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/pabloAST.h>
#include <pablo/pe_integer.h>

namespace pablo {

class EveryNth final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::EveryNth;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~EveryNth() {
    }
    inline PabloAST * getExpr() const {
        return getOperand(0);
    }
    inline Integer * getN() const {
        return llvm::cast<Integer>(getOperand(1));
    }
protected:
    explicit EveryNth(PabloAST * expr, PabloAST * n, const String * name)
    : Statement(ClassTypeId::EveryNth, expr->getType(), {expr, n}, name) {
        assert(llvm::isa<Integer>(n) && llvm::cast<Integer>(n)->value() != 0);
    }
private:
};

}


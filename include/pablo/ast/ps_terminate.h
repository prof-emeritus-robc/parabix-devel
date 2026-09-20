/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/ast/pabloAST.h>

namespace pablo {

class TerminateAt final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::TerminateAt;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~TerminateAt() {
    }
    inline PabloAST * getExpr() const {
        return getOperand(0);
    }
    inline int32_t getSignalCode() const {
        return llvm::cast<Integer>(getOperand(1))->value();
    }
protected:
    explicit TerminateAt(PabloAST * strm, PabloAST * code, const String * name)
    : Statement(ClassTypeId::TerminateAt, strm->getType(), {strm, code}, name) {
        setSideEffecting();
        assert(llvm::isa<Integer>(code));
    }
};

}


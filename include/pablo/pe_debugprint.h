/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/pabloAST.h>

namespace pablo {

class DebugPrint final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::DebugPrint;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~DebugPrint(){
    }
    PabloAST * getExpr() const {
        return getOperand(0);
    }
protected:
    DebugPrint(PabloAST * expr, const String * name)
    : Statement(ClassTypeId::DebugPrint, expr->getType(), {expr}, name) {
        setSideEffecting(true);
    }
};

}


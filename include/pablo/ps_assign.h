/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/pe_string.h>
#include <pablo/pe_var.h>

namespace pablo {

class Assign final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Assign;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Assign() {
    }
    inline Var * getVariable() const {
        return llvm::cast<Var>(getOperand(0));
    }
    inline PabloAST * getValue() const {
        return getOperand(1);
    }
    inline void setValue(PabloAST * value) {
        return setOperand(1, value);
    }
protected:
    explicit Assign(Var * variable, PabloAST * expr)
    : Statement(ClassTypeId::Assign, nullptr, {variable, expr}, nullptr) {

    }
};

}


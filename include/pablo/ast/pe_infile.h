/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/ast/pabloAST.h>

namespace pablo {

class InFile final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::InFile;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~InFile(){
    }
    PabloAST * getExpr() const {
        return getOperand(0);
    }
protected:
    InFile(PabloAST * expr, const String * name)
    : Statement(ClassTypeId::InFile, expr->getType(), {expr}, name) {

    }
};

class AtEOF final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::AtEOF;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~AtEOF(){
    }
    PabloAST * getExpr() const {
        return getOperand(0);
    }
protected:
    AtEOF(PabloAST * expr, const String * name)
    : Statement(ClassTypeId::AtEOF, expr->getType(), {expr}, name) {

    }
};
    
}


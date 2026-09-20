#pragma once

#include <pablo/ast/pabloAST.h>
#include <pablo/ast/pe_integer.h>

namespace pablo {

#ifdef USE_THREAD_UNSAFE_CANONICALIZATION
#define BOOLEAN_CANONICALIZE(x, y) {(x->getNodeId() < y->getNodeId()) ? x : y, (x->getNodeId() < y->getNodeId()) ? y : x}
#else
#define BOOLEAN_CANONICALIZE(x, y) {x, y}
#endif

class And final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::And;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~And() { }
protected:
    And(llvm::Type * const type, PabloAST * expr1, PabloAST * expr2, const String * name)
    : Statement(ClassTypeId::And, type, BOOLEAN_CANONICALIZE(expr1, expr2), name)
    {

    }
};

class Or final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Or;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Or() { }
protected:
    Or(llvm::Type * const type, PabloAST * expr1, PabloAST * expr2, const String * name)
    : Statement(ClassTypeId::Or, type, BOOLEAN_CANONICALIZE(expr1, expr2), name)
    {

    }
};

class Xor final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Xor;
    }
    static inline bool classof(const void *) {
        return false;
    }
protected:
    Xor(llvm::Type * const type, PabloAST * expr1, PabloAST * expr2, const String * name)
    : Statement(ClassTypeId::Xor, type, BOOLEAN_CANONICALIZE(expr1, expr2), name)
    {

    }
};

class Not final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Not;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Not() {
    }
    PabloAST * getExpr() const {
        return getOperand(0);
    }
protected:
    Not(PabloAST * expr, const String * name)
    : Statement(ClassTypeId::Not, expr->getType(), {expr}, name)
    {

    }
};

class Sel final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Sel;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Sel() {
    }
    inline PabloAST * getCondition() const {
        return getOperand(0);
    }
    inline PabloAST * getTrueExpr() const {
        return getOperand(1);
    }
    inline PabloAST * getFalseExpr() const {
        return getOperand(2);
    }
protected:
    Sel(PabloAST * condExpr, PabloAST * trueExpr, PabloAST * falseExpr, const String * name)
    : Statement(ClassTypeId::Sel, trueExpr->getType(), {condExpr, trueExpr, falseExpr}, name) {

    }
};

#ifdef USE_THREAD_UNSAFE_CANONICALIZATION
#undef BOOLEAN_CANONICALIZE
#endif

class Ternary final : public Statement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::Ternary;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~Ternary() {
    }
    inline Integer * getMask() const {
        return llvm::cast<Integer>(getOperand(0));
    }
    inline PabloAST * getA() const {
        return getOperand(1);
    }
    inline PabloAST * getB() const {
        return getOperand(2);
    }
    inline PabloAST * getC() const {
        return getOperand(3);
    }
protected:
    Ternary(PabloAST * mask, PabloAST * a, PabloAST * b, PabloAST * c, const String * name)
    : Statement(ClassTypeId::Ternary, a->getType(), {mask, a, b, c}, name) {
        assert(llvm::isa<Integer>(mask));
        assert(llvm::cast<Integer>(mask)->value() <= 0xFF);
    }
};

}


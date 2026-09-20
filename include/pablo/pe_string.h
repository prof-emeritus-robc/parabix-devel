#pragma once

#include <pablo/pabloAST.h>
#include <llvm/ADT/StringRef.h>

namespace pablo {

class String : public PabloAST, public llvm::StringRef {
    friend class SymbolGenerator;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::String;
    }
    static inline bool classof(const void *) {
        return false;
    } 
    virtual ~String() { }
protected:
    String(llvm::Type * type, const llvm::StringRef str) noexcept
    : PabloAST(ClassTypeId::String, type)
    , llvm::StringRef(str.data(), str.size()) {

    }
};

}


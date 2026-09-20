/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <pablo/ast/pabloAST.h>

namespace pablo {

class MatchStar final : public CarryProducingStatement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::MatchStar;
    }
    static inline bool classof(const void *) {
        return false;
    }
    inline PabloAST * getMarker() const {
        return getOperand(0);
    }
    inline PabloAST * getCharClass() const  {
        return getOperand(1);
    }
    virtual ~MatchStar() {}
protected:
    MatchStar(PabloAST * marker,  PabloAST * cc, const String * name)
    : CarryProducingStatement(ClassTypeId::MatchStar, marker->getType(), {marker, cc}, name) {
    }
};

}


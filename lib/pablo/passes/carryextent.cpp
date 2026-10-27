/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <pablo/passes/carryextent.hpp>

#include <pablo/compiler/pablo_kernel.h>
#include <pablo/ast/codegenstate.h>
#include <pablo/ast/branch.h>
#include <pablo/ast/pe_advance.h>
#include <pablo/ast/pe_var.h>
#include <pablo/ast/ps_assign.h>
#include <pablo/ast/pablo_intrinsic.h>
#include <llvm/ADT/DenseMap.h>

using namespace llvm;

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

namespace pablo {

using TypeId = PabloAST::ClassTypeId;

namespace {

struct CarryExtentAnalysis {

    uint64_t get(const PabloAST * const expr) const {
        uint64_t e = 0;
        const auto f = mExtent.find(expr);
        if (f != mExtent.end()) {
            e = f->second;
        }
        // An Extract reads an element of an array whose other elements may be assigned.
        if (LLVM_UNLIKELY(isa<Extract>(expr))) {
            e = std::max(e, get(cast<Extract>(expr)->getArray()));
        }
        return e;
    }

    void update(const PabloAST * const expr, const uint64_t e) {
        auto & current = mExtent[expr];
        current = std::max(current, e);
    }

    // Returns the first disqualifying statement, if any.  Without While loops, a single
    // pass in program order suffices: a Var read before a later Assign reads its earlier value.
    const Statement * visit(const PabloBlock * const block) {
        for (const Statement * stmt : *block) {
            switch (stmt->getClassTypeId()) {
                case TypeId::Advance:
                    BEGIN_SCOPED_REGION
                    const auto e = get(cast<Advance>(stmt)->getExpression()) + cast<Advance>(stmt)->getAmount();
                    update(stmt, e);
                    mMaxAdvanceExtent = std::max(mMaxAdvanceExtent, e);
                    END_SCOPED_REGION
                    break;
                case TypeId::IndexedAdvance:
                case TypeId::ScanThru:
                case TypeId::AdvanceThenScanThru:
                case TypeId::ScanTo:
                case TypeId::AdvanceThenScanTo:
                case TypeId::MatchStar:
                case TypeId::While:
                // Count and EveryNth accumulate state across blocks.
                case TypeId::Count:
                case TypeId::EveryNth:
                case TypeId::TerminateAt:
                // PackH and PackL move bits between positions.
                case TypeId::PackH:
                case TypeId::PackL:
                    return stmt;
                case TypeId::IntrinsicCall:
                    if (cast<IntrinsicCall>(stmt)->isCarryProducing()) {
                        return stmt;
                    }
                    update(stmt, maxOfOperands(stmt));
                    break;
                case TypeId::If:
                    if (const Statement * const d = visit(cast<If>(stmt)->getBody())) {
                        return d;
                    }
                    break;
                case TypeId::Assign:
                    BEGIN_SCOPED_REGION
                    const Var * const var = cast<Assign>(stmt)->getVariable();
                    const auto e = get(cast<Assign>(stmt)->getValue());
                    update(var, e);
                    if (LLVM_UNLIKELY(isa<Extract>(var))) {
                        update(cast<Extract>(var)->getArray(), e);
                    }
                    END_SCOPED_REGION
                    break;
                default:
                    update(stmt, maxOfOperands(stmt));
            }
        }
        return nullptr;
    }

    uint64_t maxOfOperands(const Statement * const stmt) const {
        uint64_t e = 0;
        for (unsigned i = 0; i < stmt->getNumOperands(); ++i) {
            e = std::max(e, get(stmt->getOperand(i)));
        }
        return e;
    }

    DenseMap<const PabloAST *, uint64_t> mExtent;
    // The carry-out of Advance(x, n) is the last n positions of x, which are correct
    // after zero-carry processing of k blocks when E(x) + n <= k * BlockWidth.
    uint64_t mMaxAdvanceExtent = 0;
};

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief analyze
 ** ------------------------------------------------------------------------------------------------------------- */
CarryExtentResult CarryExtent::analyze(const PabloKernel * const kernel) {
    CarryExtentAnalysis analysis;
    CarryExtentResult result;
    result.Disqualifier = analysis.visit(kernel->getEntryScope());
    if (result.Disqualifier) {
        result.Qualifies = false;
    } else {
        result.Extent = analysis.mMaxAdvanceExtent;
    }
    return result;
}

}

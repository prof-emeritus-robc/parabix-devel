/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <set>
#include <re/adt/adt_forward_decl.h>
#include <re/adt/memoization.h>

namespace re {

//
// The mode for processing Names and PropertyExpressions.
// If a definition is required but absent, an error is reported.
//
enum class NameProcessingMode {
    None,                 //  Leave unanalyzed/unmodified.
    RequireDefinition,    //  Require a definition but do not process.
    ProcessDefinition,    //  Process the definition, if present
    ProcessRequired       //  Require and process a definiton.
};

enum class InspectionMode {TraverseNonUnique, IgnoreNonUnique};

class RE_Inspector {
public:
    void inspectRE(RE * r);
protected:
    RE_Inspector(const NameProcessingMode m = NameProcessingMode::RequireDefinition,
                 const InspectionMode ignoreNonUnique = InspectionMode::IgnoreNonUnique) :
         mNameMode(m), mIgnoreNonUnique(ignoreNonUnique) {}
    virtual ~RE_Inspector() {}
    void inspect(RE * r);
    virtual void inspectName(Name * n);
    virtual void inspectCapture(Capture * c);
    virtual void inspectReference(Reference * r);
    virtual void inspectStart(Start * s);
    virtual void inspectEnd(End * e);
    virtual void inspectAny(Any * a);
    virtual void inspectCC(CC * cc);
    virtual void inspectSeq(Seq * s);
    virtual void inspectAlt(Alt * a);
    virtual void inspectRep(Rep * rep);
    virtual void inspectIntersect(Intersect * e);
    virtual void inspectDiff(Diff * d);
    virtual void inspectRange(Range * rg);
    virtual void inspectGroup(Group * g);
    virtual void inspectAssertion(Assertion * a);
    virtual void inspectPermute(Permute * p);
    virtual void inspectInterleavable(Interleavable * s);
    virtual void inspectPropertyExpression(PropertyExpression * pe);
private:
    const NameProcessingMode mNameMode;
    const InspectionMode mIgnoreNonUnique;
    std::set<RE *, MemoizerComparator> mMap;
};

}

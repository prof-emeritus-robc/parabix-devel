/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <re/adt/adt_forward_decl.h>

namespace re {

class RE;
class Name;

enum class NameTransformationMode {None, TransformDefinition};

class RE_Transformer {
public:
    RE * transformRE(RE * r, NameTransformationMode m = NameTransformationMode::None);
protected:
    RE_Transformer(std::string transformationName)
    : mTransformationName(std::move(transformationName)),
      mNameTransform(NameTransformationMode::None) {}

    virtual ~RE_Transformer() {}
    virtual RE * transform(RE * r);
    virtual RE * transformName(Name * n);
    virtual RE * transformCapture(Capture * c);
    virtual RE * transformReference(Reference * r);
    virtual RE * transformStart(Start * s);
    virtual RE * transformEnd(End * e);
    virtual RE * transformAny(Any * a);
    virtual RE * transformCC(CC * cc);
    virtual RE * transformSeq(Seq * s);
    virtual RE * transformAlt(Alt * a);
    virtual RE * transformRep(Rep * rep);
    virtual RE * transformIntersect(Intersect * e);
    virtual RE * transformDiff(Diff * d);
    virtual RE * transformRange(Range * rg);
    virtual RE * transformGroup(Group * g);
    virtual RE * transformAssertion(Assertion * a);
    virtual RE * transformPermute(Permute * p);
    virtual RE * transformInterleavable(Interleavable * s);
    virtual RE * transformPropertyExpression(PropertyExpression * pe);
    virtual void showProcessing();
protected:
    const std::string mTransformationName;
    NameTransformationMode mNameTransform;
    RE * mInitialRE;
};

}

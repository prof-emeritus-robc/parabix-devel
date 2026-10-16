/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/transforms/capture_removal.h>

#include <re/adt/adt.h>
#include <re/analysis/re_inspector.h>
#include <re/transforms/re_transformer.h>

#include <util/small_flat_set.hpp>

using namespace llvm;

namespace re {

using ReferenceSet = boost::container::flat_set<std::string>;

struct ReferenceCollector  final : public RE_Inspector {
    ReferenceCollector(ReferenceSet & references)
    : RE_Inspector(NameProcessingMode::None, InspectionMode::IgnoreNonUnique), mReferences(references) {}

    void inspectReference(Reference * r) override {
        auto name = r -> getName();
        mReferences.insert(r->getName());
    }

private:
    ReferenceSet & mReferences;
};

struct UnneededCaptureRemoval  : public RE_Transformer {
    UnneededCaptureRemoval(ReferenceSet & references)
    : RE_Transformer("UnneededCaptureRemoval"), mReferences(references) {}

    RE * transformCapture(Capture * c) override {
        auto name = c->getName();
        auto x = c->getCapturedRE();
        auto t = transform(x);
        if (mReferences.count(c->getName()) == 0) {
            return t;
        } else if (t != x) {
            return makeCapture(name, t);
        }
        return c;
    }

private:
    ReferenceSet & mReferences;
};

RE * removeUnneededCaptures(RE * r) {
    ReferenceSet refs;
    ReferenceCollector(refs).inspectRE(r);
    return UnneededCaptureRemoval(refs).transformRE(r);
}

RE * removeUnneededCaptures(RE * capturingRE, RE * referencingRE) {
    ReferenceSet refs;
    ReferenceCollector(refs).inspectRE(referencingRE);
    return UnneededCaptureRemoval(refs).transformRE(capturingRE);
}
}

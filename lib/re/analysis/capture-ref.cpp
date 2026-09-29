/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/analysis/capture-ref.h>
#include <re/adt/adt.h>
#include <re/analysis/re_analysis.h>
#include <re/alphabet/alphabet.h>
#include <llvm/Support/raw_ostream.h>
#include <limits.h>

using namespace llvm;

namespace re {

using CapturePostfixMap = std::map<std::string, std::vector<RE *>>;

void updateCaptures(RE * re, CapturePostfixMap & cm) {
    for (auto & mapping : cm) {
        mapping.second.push_back(re);
    }
    if (Capture * c = dyn_cast<Capture>(re)) {
        cm.emplace(c->getName(), std::vector<RE *>{});
    }
}

void update1reference(Reference * ref, CapturePostfixMap & cm, ReferenceInfo & info) {
    std::string refName = ref->getName();
    std::string instanceName = ref->getInstanceName();
    auto f = cm.find(refName);
    if (f != cm.end()) {
        RE * twixt = makeSeq(f->second.begin(), f->second.end());
        info.twixtREs.emplace(instanceName, twixt);
    } else {
        llvm::report_fatal_error("reference without capture");
    }
}
void updateReferenceInfo(RE * re, CapturePostfixMap & cm, ReferenceInfo & info) {
    if (Capture * c = dyn_cast<Capture>(re)) {
        updateReferenceInfo(c->getCapturedRE(), cm, info);
        info.captureRefs.emplace(c->getName(), std::vector<std::string>{});
    } else if (Seq * seq = dyn_cast<Seq>(re)) {
        if (!seq->empty()) {
            CapturePostfixMap cm1 = cm;  // copy
            RE * r1 = seq->front();
            updateReferenceInfo(r1, cm1, info);
            for (auto it = seq->begin()+1; it != seq->end(); it++)  {
                updateCaptures(r1, cm1);
                r1 = *it;
                updateReferenceInfo(r1, cm1, info);
            }
        }
    } else if (Alt * alt = dyn_cast<Alt>(re)) {
        for (auto & ai : *alt) {
            updateReferenceInfo(ai, cm, info);
        }
    } else if (Rep * rep = dyn_cast<Rep>(re)) {
        updateReferenceInfo(rep->getRE(), cm, info);
    } else if (Diff * diff = dyn_cast<Diff>(re)) {
        updateReferenceInfo(diff->getLH(), cm, info);
        updateReferenceInfo(diff->getRH(), cm, info);
    } else if (Intersect * ix = dyn_cast<Intersect>(re)) {
        updateReferenceInfo(ix->getLH(), cm, info);
        updateReferenceInfo(ix->getRH(), cm, info);
    } else if (Reference * ref = dyn_cast<Reference>(re)) {
        update1reference(ref, cm, info);
    } else if (Assertion * a = dyn_cast<Assertion>(re)) {
        updateReferenceInfo(a->getAsserted(), cm, info);
    } else if (PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        RE * defn = pe->getResolvedRE();
        if (defn && isa<Reference>(defn)) {
            update1reference(cast<Reference>(defn), cm, info);
        }
    } else if (!isa<Any, CC, Range, End, Name, Group, Start, Permute, Interleavable>(re)) {
        UnexpectedRE("updateReferenceInfo", re);
    }
}

ReferenceInfo buildReferenceInfo(RE * re) {
    ReferenceInfo info;
    CapturePostfixMap cm;
    updateReferenceInfo(re, cm, info);
    return info;
}
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  The NFD status of a transform: what is known of the normalization of its
//  output.
//
//  - Always: the output is in NFD, whatever the input.
//  - Preserving: the output is in NFD when the input is.
//  - NotGuaranteed: neither is shown.
//
//  Alongside the status, filterSafe says whether the transform also preserves
//  NFD when it is applied within a filter: to each run of the characters of a
//  filter by itself, the other characters unchanged, the whole text being in
//  NFD before.  A transform that preserves NFD may still not be filter safe,
//  as the edges of a run's output meet the characters outside it (NFKD of a
//  run may end with a mark, before a mark of a lower combining class outside).
//
//  The status of a transform's forward rules (as ExtractForwardRules or
//  ExtractReverseBackwardRules gives them) is derived from its steps, each
//  within the global filter (the filter rule), if any:
//  - A conversion group is Preserving and filter safe if Is_NFD_Preserving
//    holds for it (its check holds for the whole text, within any filter), and
//    NotGuaranteed otherwise.
//  - A transform rule :: [F] X ; has the status of X (a built-in transform, or
//    the transform of the registry, determined in turn), within its filter.
//    As in ICU, the filter of a filtered transform rule that begins a block of
//    transform rules (at the start of the rules, or after a conversion group),
//    without a global filter, also applies to the unfiltered rules of the rest
//    of the block (see doc/ICU-uconv-CLDR-issues.md); as a filter only weakens
//    the status, this holds as well where the filter is not carried (xlit).
//  - Within a filter E (a step's own filter and the global filter), a step
//    that is Always or Preserving is Preserving if it is filter safe, or if E
//    includes every mark (the characters outside E are then starters, and no
//    mark of a run meets one outside it); otherwise it is NotGuaranteed.
//  - The built-in transforms (unfiltered): Any-NFD (filter safe: it leaves
//    NFD as it is), Any-NFKD, the Any-Hex forms and Any-Name (ASCII, filter
//    safe), and Any-Remove (empty) are Always; Any-Null and Any-FCD (which
//    leaves NFD as it is) are Preserving and filter safe; Any-Lower,
//    Any-Upper, Any-Title and Any-CaseFold are Preserving and filter safe if
//    the full case mappings (lc, uc, tc and lc, cf) of the characters
//    without a canonical decomposition give such characters in canonical
//    order, beginning and ending with starters for a starter, and for a mark
//    of class k, beginning with a starter or a mark of class at least k and
//    ending with a starter or a mark of class at most k; the others (Any-NFC,
//    Any-NFKC, Any-FCC, Hex-Any, Name-Any, Any-BreakInternal) and unknown
//    transforms are NotGuaranteed.
//  - The transform is Always if some step is Always and every step after the
//    last such step is Always or Preserving; otherwise Preserving if every
//    step is; otherwise NotGuaranteed.  It is filter safe if every step is
//    Always or Preserving and filter safe.
//  A transform that invokes itself (through others) is NotGuaranteed.

#pragma once

#include <map>
#include <string>
#include <vector>
#include <ldml/transform_rules.h>
#include <ldml/transform_registry.h>

namespace ldml {

enum class NFD_Status {Always, Preserving, NotGuaranteed};

const char * NFD_StatusName(NFD_Status s);

struct NFD_StatusReport {
    NFD_Status status = NFD_Status::NotGuaranteed;
    bool filterSafe = false;
    //  Why: the step that makes the output always NFD, or that is not shown to
    //  preserve NFD (and why).
    std::string reason;
};

class NFD_StatusAnalyzer {
public:
    explicit NFD_StatusAnalyzer(const TransformRegistry & registry) : mRegistry(registry) {}
    //  The status of a registered transform (cached).
    NFD_StatusReport Determine_NFD_Status(const TransformEntry * entry);
    //  The status of a transform by name (NotGuaranteed if unknown).
    NFD_StatusReport Determine_NFD_Status(const std::string & name);
    //  The status of the forward rules of a transform; the transforms it
    //  invokes are looked up in the registry.
    NFD_StatusReport Determine_NFD_Status(const std::vector<Rule *> & forwardRules);
private:
    NFD_StatusReport builtIn(const TransformEntry * entry);
    NFD_StatusReport invoked(const TransformID & id);
    const TransformRegistry & mRegistry;
    std::map<const TransformEntry *, NFD_StatusReport> mCache;
    std::map<const TransformEntry *, bool> mActive;
};

}

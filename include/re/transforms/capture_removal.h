/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <vector>

#include <llvm/Support/Compiler.h>

namespace re {

class RE;

//
// Transform a regular expression by removing any captures
// that are unreferenced within the RE.  For example,
// in the RE  "([a-z])(ab)*\1" the first capture is needed
// while the second is not, so the transformed RE becomes
// equivalent to replacement of the second capture by a
// noncapturing group: "([a-z])(?:ab)*\1"
//
RE * removeUnneededCaptures(RE * r);

// Transform a capturingRE by removing any captures that
// are unreferenced either in the referencingRE.
//
RE * removeUnneededCaptures(RE * capturingRE, RE * referencingRE);

// Transform a capturingRE by removing any captures that are
// unreferenced in all of the referencingREs (which may include
// the capturingRE itself, for its back references).
RE * removeUnneededCaptures(RE * capturingRE, const std::vector<RE *> & referencingREs);

}


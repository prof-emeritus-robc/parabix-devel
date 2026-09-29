/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

namespace re { class RE; class Name;}

namespace re {

RE * makeComplement(RE * s);
RE * makeZerowidthComplement(RE * s);
RE * makeWordBoundary();
RE * makeWordNonBoundary();
RE * makeWordBegin();
RE * makeWordEnd();

}

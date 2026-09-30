/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>

namespace parabix {

// An identifier of the code currently running in this process: a hash of the
// linker-generated build IDs (Mach-O LC_UUID, ELF NT_GNU_BUILD_ID) of the main
// executable and every loaded Parabix library.  Relinking any of them with
// different contents changes the identifier, so it is suitable as the key of
// on-disk caches of JIT-compiled code.  Computed once; thread safe.
const std::string & getBuildIdentity();

}

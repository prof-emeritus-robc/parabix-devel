/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  Locations of the files Parabix programs use at run time.
 */

#pragma once

#include <string>
#include <llvm/ADT/StringRef.h>

namespace codegen {

// The installation directories holding files of Parabix (in a "parabix"
// subdirectory): programs run by other programs, and read-only data.
enum class InstallDir { Libexec, Data };

// The path of a file installed with Parabix, e.g. "cachejanitord" in
// Libexec or "pablo/json.pablo" in Data; empty if there is none.
//
// The file is looked for relative to the running program first: an
// installation keeps the relation between its bin directory and the others
// (bin/../libexec/parabix with the default directories), wherever it is
// installed, and the build tree is laid out the same way (<build>/bin,
// <build>/libexec/parabix, <build>/share/parabix).  Then in the directories
// of the configured installation prefix.
std::string findInstalledFile(InstallDir dir, llvm::StringRef relativePath);

// The default object cache directory: $PARABIX_OBJECT_CACHE_DIR if set,
// otherwise ~/.parabix/objcache.
std::string getDefaultObjectCacheDir();

}

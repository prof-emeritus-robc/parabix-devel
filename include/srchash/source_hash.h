/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>

namespace parabix {

// Source hashes of the loaded images (the executable and the Parabix
// libraries).  At build time every module and executable gets a generated
// <target>_srchash.cpp (see cmake/ParabixSourceHash.cmake) holding a hash of
// its sources and headers and those of every Parabix module it depends on;
// a static initializer in that file registers the hash for the image it is
// linked into.  Looking up the image that holds the code (or vtable) of a
// kernel then gives a hash that changes exactly when the source that kernel
// can depend on changes.

// Registers HASH as a source hash of TARGET, linked into the image holding
// ANCHOR.  Called by the generated files; returns true.
bool registerSourceHash(const void * anchor, const char * target, const char * hash);

// The source hash of the image holding ADDRESS.  When several targets were
// linked into one image (static builds), their hashes are joined in target
// order.  Empty if no hash was registered for that image.
std::string getImageSourceHash(const void * address);

// The file name (without directories) of the image holding ADDRESS.
std::string getImageName(const void * address);

// The linker build ID (Mach-O LC_UUID, ELF NT_GNU_BUILD_ID) of the image
// holding ADDRESS, in hex.  Empty if the image carries none.
std::string getImageBuildID(const void * address);

}

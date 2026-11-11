/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <toolchain/install_paths.h>

#include <toolchain/toolchain.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <cstdlib>

// Set in lib/toolchain/CMakeLists.txt from the GNUInstallDirs directories.
#if !defined(PARABIX_BINDIR_TO_LIBEXECDIR) || !defined(PARABIX_BINDIR_TO_DATADIR) \
    || !defined(PARABIX_INSTALL_LIBEXECDIR) || !defined(PARABIX_INSTALL_DATADIR)
#error "installation directories not defined"
#endif

using namespace llvm;

namespace codegen {

// The directory of the running program, with symbolic links resolved (a tool
// may be run through a link, e.g., in libexec/parabix/bin).
static SmallString<256> programDirectory() {
    SmallString<256> dir;
    const std::string exe = sys::fs::getMainExecutable(ProgramName ? ProgramName : "",
                                                        reinterpret_cast<void *>(&findInstalledFile));
    if (exe.empty() || sys::fs::real_path(exe, dir)) {
        dir = exe;
    }
    sys::path::remove_filename(dir);
    return dir;
}

std::string findInstalledFile(InstallDir dir, StringRef relativePath) {
    const bool libexec = (dir == InstallDir::Libexec);
    SmallString<256> path = programDirectory();
    if (!path.empty()) {
        sys::path::append(path, libexec ? PARABIX_BINDIR_TO_LIBEXECDIR : PARABIX_BINDIR_TO_DATADIR, "parabix", relativePath);
        sys::path::remove_dots(path, true);
        if (sys::fs::exists(path)) {
            return std::string(path.str());
        }
    }
    path = libexec ? PARABIX_INSTALL_LIBEXECDIR : PARABIX_INSTALL_DATADIR;
    sys::path::append(path, "parabix", relativePath);
    if (sys::fs::exists(path)) {
        return std::string(path.str());
    }
    return std::string();
}

std::string getDefaultObjectCacheDir() {
    if (const char * const dir = std::getenv("PARABIX_OBJECT_CACHE_DIR")) {
        if (*dir) {
            return dir;
        }
    }
    SmallString<256> path;
    sys::path::home_directory(path);
    sys::path::append(path, ".parabix", "objcache");
    return std::string(path.str());
}

}

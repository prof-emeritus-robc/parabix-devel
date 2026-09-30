/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <objcache/build_identity.h>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/SHA1.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#elif defined(__linux__)
#include <link.h>
#include <elf.h>
#endif

namespace parabix {

namespace {

// (image name, identity bytes) for each image whose code may generate kernels.
using ImageIdentities = std::vector<std::pair<std::string, std::string>>;

std::string baseName(const char * path) {
    const char * const slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool isParabixLibrary(const std::string & name) {
    // Matches libparabix_<module>.{dylib,so} and the single-artifact libparabix.*
    return name.compare(0, 10, "libparabix") == 0;
}

// Identity from file metadata, for images that carry no linker build ID.
std::string fileIdentity(const char * path) {
    struct stat st;
    if (path == nullptr || stat(path, &st) != 0) {
        return std::string{};
    }
    std::string id;
    id.append(reinterpret_cast<const char *>(&st.st_size), sizeof(st.st_size));
    #if defined(__APPLE__)
    id.append(reinterpret_cast<const char *>(&st.st_mtimespec), sizeof(st.st_mtimespec));
    #else
    id.append(reinterpret_cast<const char *>(&st.st_mtim), sizeof(st.st_mtim));
    #endif
    return id;
}

#if defined(__APPLE__)

std::string machoUUID(const mach_header * header) {
    const bool is64 = (header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64);
    const uint8_t * cmd = reinterpret_cast<const uint8_t *>(header) + (is64 ? sizeof(mach_header_64) : sizeof(mach_header));
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const load_command * const lc = reinterpret_cast<const load_command *>(cmd);
        if (lc->cmd == LC_UUID) {
            const uuid_command * const uc = reinterpret_cast<const uuid_command *>(lc);
            return std::string(reinterpret_cast<const char *>(uc->uuid), sizeof(uc->uuid));
        }
        cmd += lc->cmdsize;
    }
    return std::string{};
}

ImageIdentities collectImageIdentities() {
    ImageIdentities images;
    const uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; ++i) {
        const mach_header * const header = _dyld_get_image_header(i);
        const char * const path = _dyld_get_image_name(i);
        if (header == nullptr || path == nullptr) continue;
        const std::string name = baseName(path);
        if (header->filetype != MH_EXECUTE && !isParabixLibrary(name)) continue;
        std::string id = machoUUID(header);
        if (id.empty()) id = fileIdentity(path);
        images.emplace_back(name, std::move(id));
    }
    return images;
}

#elif defined(__linux__)

std::string elfBuildID(const dl_phdr_info * info) {
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_NOTE) continue;
        const uint8_t * p = reinterpret_cast<const uint8_t *>(info->dlpi_addr + phdr.p_vaddr);
        const uint8_t * const end = p + phdr.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) * const note = reinterpret_cast<const ElfW(Nhdr) *>(p);
            const uint8_t * const name = p + sizeof(ElfW(Nhdr));
            const uint8_t * const desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0) {
                return std::string(reinterpret_cast<const char *>(desc), note->n_descsz);
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return std::string{};
}

ImageIdentities collectImageIdentities() {
    ImageIdentities images;
    dl_iterate_phdr([](dl_phdr_info * info, size_t, void * data) -> int {
        auto & images = *static_cast<ImageIdentities *>(data);
        const bool isMain = (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0');
        const std::string name = isMain ? std::string{"<main>"} : baseName(info->dlpi_name);
        if (!isMain && !isParabixLibrary(name)) return 0;
        std::string id = elfBuildID(info);
        if (id.empty()) id = fileIdentity(isMain ? "/proc/self/exe" : info->dlpi_name);
        images.emplace_back(name, std::move(id));
        return 0;
    }, &images);
    return images;
}

#else

ImageIdentities collectImageIdentities() {
    return ImageIdentities{};
}

#endif

std::string computeBuildIdentity() {
    ImageIdentities images = collectImageIdentities();
    std::sort(images.begin(), images.end());
    bool complete = !images.empty();
    llvm::SHA1 hash;
    for (const auto & image : images) {
        if (image.second.empty()) complete = false;
        hash.update(image.first);
        hash.update(llvm::StringRef("\0", 1));
        hash.update(image.second);
    }
    if (!complete) {
        // Some image could not be identified: fall back to the configured
        // version string so that the result is at least stable per build.
        hash.update(PARABIX_VERSION);
    }
    const auto digest = hash.final();
    return llvm::toHex(llvm::ArrayRef<uint8_t>(digest.data(), 8), /*LowerCase=*/true);
}

} // anonymous namespace

const std::string & getBuildIdentity() {
    static const std::string identity = computeBuildIdentity();
    return identity;
}

}

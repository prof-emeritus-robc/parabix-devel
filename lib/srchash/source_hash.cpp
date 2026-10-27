/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <srchash/source_hash.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <dlfcn.h>

#if defined(__APPLE__)
#include <mach-o/loader.h>
#elif defined(__linux__)
#include <link.h>
#include <elf.h>
#endif

namespace parabix {

namespace {

struct Registration {
    std::string hash;
    std::string modules;
};

// image base address -> (target name -> registration)
using Registry = std::map<const void *, std::map<std::string, Registration>>;

// Registration runs in static initializers of every image, so the registry
// is created on first use rather than depending on initialization order.
std::pair<Registry &, std::mutex &> registry() {
    static Registry R;
    static std::mutex M;
    return {R, M};
}

const void * imageBase(const void * address) {
    Dl_info info;
    if (address == nullptr || dladdr(address, &info) == 0) {
        return nullptr;
    }
    return info.dli_fbase;
}

std::string toHex(const uint8_t * bytes, const size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        hex.push_back(digits[bytes[i] >> 4]);
        hex.push_back(digits[bytes[i] & 15]);
    }
    return hex;
}

} // anonymous namespace

bool registerSourceHash(const void * anchor, const char * target, const char * hash, const char * modules) {
    auto R = registry();
    std::lock_guard<std::mutex> L(R.second);
    R.first[imageBase(anchor)][target] = Registration{hash, modules};
    return true;
}

std::string getImageSourceHash(const void * address) {
    const void * const base = imageBase(address);
    if (base == nullptr) {
        return std::string{};
    }
    auto R = registry();
    std::lock_guard<std::mutex> L(R.second);
    const auto f = R.first.find(base);
    if (f == R.first.end()) {
        return std::string{};
    }
    const auto & targets = f->second;
    if (targets.size() == 1) {
        return targets.begin()->second.hash;
    }
    std::string joined;
    for (const auto & t : targets) {
        joined.append(t.first);
        joined.push_back(':');
        joined.append(t.second.hash);
        joined.push_back(',');
    }
    return joined;
}

std::string getImageModules(const void * address) {
    const void * const base = imageBase(address);
    if (base == nullptr) {
        return std::string{};
    }
    auto R = registry();
    std::lock_guard<std::mutex> L(R.second);
    const auto f = R.first.find(base);
    if (f == R.first.end()) {
        return std::string{};
    }
    std::string modules;
    for (const auto & t : f->second) {
        if (!modules.empty()) modules.push_back(' ');
        modules.append(t.second.modules);
    }
    return modules;
}

std::string getImageName(const void * address) {
    Dl_info info;
    if (address == nullptr || dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
        return std::string{};
    }
    const char * const slash = std::strrchr(info.dli_fname, '/');
    return slash ? slash + 1 : info.dli_fname;
}

#if defined(__APPLE__)

std::string getImageBuildID(const void * address) {
    const mach_header * const header = static_cast<const mach_header *>(imageBase(address));
    if (header == nullptr) {
        return std::string{};
    }
    const bool is64 = (header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64);
    const uint8_t * cmd = reinterpret_cast<const uint8_t *>(header) + (is64 ? sizeof(mach_header_64) : sizeof(mach_header));
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const load_command * const lc = reinterpret_cast<const load_command *>(cmd);
        if (lc->cmd == LC_UUID) {
            const uuid_command * const uc = reinterpret_cast<const uuid_command *>(lc);
            return toHex(uc->uuid, sizeof(uc->uuid));
        }
        cmd += lc->cmdsize;
    }
    return std::string{};
}

#elif defined(__linux__)

namespace {

struct BuildIDSearch {
    uintptr_t address;
    std::string id;
};

bool containsAddress(const dl_phdr_info * info, const uintptr_t address) {
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) & phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) continue;
        const uintptr_t start = info->dlpi_addr + phdr.p_vaddr;
        if (address >= start && address < start + phdr.p_memsz) {
            return true;
        }
    }
    return false;
}

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
                return toHex(desc, note->n_descsz);
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return std::string{};
}

} // anonymous namespace

std::string getImageBuildID(const void * address) {
    BuildIDSearch search{reinterpret_cast<uintptr_t>(address), std::string{}};
    dl_iterate_phdr([](dl_phdr_info * info, size_t, void * data) -> int {
        auto & search = *static_cast<BuildIDSearch *>(data);
        if (!containsAddress(info, search.address)) return 0;
        search.id = elfBuildID(info);
        return 1;
    }, &search);
    return search.id;
}

#else

std::string getImageBuildID(const void *) {
    return std::string{};
}

#endif

}

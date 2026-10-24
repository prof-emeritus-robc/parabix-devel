/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// make_circular_buffer under each --CircularBufferImpl: every copy of the buffer must show
// the same pages (a write through any copy is read back through all of them), with and
// without the underflow copy, and the whole mapping must be freeable by munmap. On macOS
// the default buffer must be anonymous memory (Mach vm_remap), not the file-backed mapping
// of a temp file in /tmp, which --CircularBufferImpl=memfd still selects.

#include <toolchain/toolchain.h>
#include <idisa/CBuilder.h>
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef __APPLE__
#include <mach/mach.h>
#undef PAGE_SIZE  // mach's macro (vm_page_size) would hide CBuilder::PAGE_SIZE
#endif

extern "C" uint8_t * make_circular_buffer(const size_t size, const size_t hasUnderflow);

static unsigned failures = 0;

static void check(bool ok, const char * what, const char * impl, size_t size, size_t hasUnderflow) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s: impl=%s size=%zu underflow=%zu\n", what, impl, size, hasUnderflow);
        failures++;
    }
}

#ifdef __APPLE__
// whether the pages at addr come from a pager outside the VM system (a file)
static bool isFileBacked(const uint8_t * const addr) {
    vm_address_t region = reinterpret_cast<vm_address_t>(addr);
    vm_size_t regionSize = 0;
    vm_region_extended_info_data_t info;
    mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &region, &regionSize, VM_REGION_EXTENDED_INFO,
                     reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
        return false;
    }
    return info.external_pager != 0;
}
#endif

static void testBuffer(const char * impl, const size_t size, const size_t hasUnderflow) {
    const size_t m = 2 + hasUnderflow;
    uint8_t * const p = make_circular_buffer(size, hasUnderflow);
    uint8_t * const base = p - hasUnderflow * size;
    // fill copy 0 and read it back through every copy
    for (size_t i = 0; i < size; i += 4093) p[i] = uint8_t(i * 7 + 1);
    p[size - 1] = 0xA5;
    bool same = true;
    for (size_t c = 0; c < m; c++) {
        const uint8_t * const q = base + c * size;
        for (size_t i = 0; i < size; i += 4093) same &= (q[i] == uint8_t(i * 7 + 1));
        same &= (q[size - 1] == 0xA5);
    }
    check(same, "copy 0 not mirrored", impl, size, hasUnderflow);
    // a write through the last copy shows in copy 0
    base[(m - 1) * size + 17] = 0x3C;
    check(p[17] == 0x3C, "last copy not mirrored", impl, size, hasUnderflow);
    // a write that wraps past the end of copy 0 lands at its start
    p[size + 1] = 0x5A;
    check(p[1] == 0x5A, "wrap write lost", impl, size, hasUnderflow);
    #ifdef __APPLE__
    const bool wantFile = (codegen::CircularBufferImpl == codegen::CircularBufferImplOptions::Memfd);
    for (size_t c = 0; c < m; c++) {
        check(isFileBacked(base + c * size) == wantFile, wantFile ? "memfd copy not file-backed" : "default copy file-backed",
              impl, size, hasUnderflow);
    }
    #endif
    check(munmap(base, m * size) == 0, "munmap", impl, size, hasUnderflow);
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv);
    const size_t page = CBuilder::PAGE_SIZE;
    const size_t sizes[] = {page, 4 * page, 64 * page};
    const struct { codegen::CircularBufferImplOptions opt; const char * name; } impls[] = {
        {codegen::CircularBufferImplOptions::Default, "default"},
        {codegen::CircularBufferImplOptions::Memfd, "memfd"},
    };
    for (const auto & impl : impls) {
        codegen::CircularBufferImpl = impl.opt;
        for (const size_t size : sizes) {
            for (size_t hasUnderflow = 0; hasUnderflow < 2; hasUnderflow++) {
                testBuffer(impl.name, size, hasUnderflow);
            }
        }
    }
    if (failures) {
        std::fprintf(stderr, "%u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("circular buffer tests passed\n");
    return EXIT_SUCCESS;
}

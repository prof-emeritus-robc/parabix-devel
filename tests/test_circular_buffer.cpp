/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

// make_circular_buffer under each --CircularBufferImpl: every copy of the buffer must show
// the same pages (a write through any copy is read back through all of them), with and
// without the underflow copy, and the whole mapping must be freeable by munmap. On macOS
// the default buffer must be anonymous memory (Mach vm_remap), not the file-backed mapping
// of a temp file in /tmp, which --CircularBufferImpl=memfd still selects. Then the reuse
// pool (--CircularBufferPool): a released buffer comes back for the same size only, still
// mirrored, within the pool's limit, and not after releaseCircularBufferPool().

#include <toolchain/toolchain.h>
#include <kernel/core/streamset.h>
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
extern "C" void release_circular_buffer(uint8_t * const addr, const size_t mappedLength);

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

static bool mirrored(uint8_t * const p, const size_t size) {
    p[size + 3] = 0x77;
    return p[3] == 0x77;
}

static void testPool(const size_t page) {
    const char * const impl = "pool";
    codegen::CircularBufferImpl = codegen::CircularBufferImplOptions::Default;
    codegen::CircularBufferPoolMiB = 1;
    const size_t size = 4 * page;
    uint8_t * const a = make_circular_buffer(size, 0);
    a[0] = a[size - 1] = 0x42;
    release_circular_buffer(a, 2 * size);
    // another size misses the pool; the same size gets the released buffer back
    uint8_t * const b = make_circular_buffer(2 * size, 0);
    check(b != a, "other size reused", impl, size, 0);
    uint8_t * const c = make_circular_buffer(size, 0);
    check(c == a, "same size not reused", impl, size, 0);
    check(c[0] == 0 && c[size - 1] == 0, "reused buffer not zeroed", impl, size, 0);
    check(mirrored(c, size), "reused buffer not mirrored", impl, size, 0);
    // poisoned on reuse when asked
    release_circular_buffer(c, 2 * size);
    codegen::PoisonReusedBuffers = true;
    uint8_t * const d = make_circular_buffer(size, 0);
    codegen::PoisonReusedBuffers = false;
    check(d == a && d[0] == 0xA5 && d[size - 1] == 0xA5, "reused buffer not poisoned", impl, size, 0);
    release_circular_buffer(d, 2 * size);
    release_circular_buffer(b, 4 * size);
    // from here on a pooled buffer comes back as 0xA5 and a fresh one as zeros (the address
    // alone can't tell: the VM often maps a fresh buffer where the last one was unmapped)
    codegen::PoisonReusedBuffers = true;
    // past the 1 MiB limit a released buffer is unmapped, not kept
    const size_t big = (1 << 20) - page;
    uint8_t * const e = make_circular_buffer(big, 0);
    release_circular_buffer(e, 2 * big);
    uint8_t * const f = make_circular_buffer(big, 0);
    check(f[0] == 0 && mirrored(f, big), "over-limit buffer kept", impl, big, 0);
    munmap(f, 2 * big);
    // after a drain the pool hands out nothing it held
    kernel::releaseCircularBufferPool();
    uint8_t * const g = make_circular_buffer(size, 0);
    check(g[0] == 0 && mirrored(g, size), "pooled buffer survived the drain", impl, size, 0);
    codegen::PoisonReusedBuffers = false;
    munmap(g, 2 * size);
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv);
    codegen::CircularBufferPoolMiB = 0;  // fresh buffers, so that each impl's mapping is checked
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
    testPool(page);
    if (failures) {
        std::fprintf(stderr, "%u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("circular buffer tests passed\n");
    return EXIT_SUCCESS;
}

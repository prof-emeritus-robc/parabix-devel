/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <toolchain/fileutil.h>
#include <string>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <util/aligned_allocator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;
bool NoOSFileCaching;
static cl::opt<bool, true> OptNoOSCaching("disable-file-caching", cl::location(NoOSFileCaching),
                                         cl::desc("Disable OS file caching."), cl::init(false));

bool MMapPreference;
static cl::opt<bool, true> PreferMMap("PreferMMap", cl::location(MMapPreference), cl::init(false), cl::desc("Prefer mmap for file input."));

bool canMMap(const std::string & fileName) {
    if (fileName == "-") return false;
    namespace fs = boost::filesystem;
    fs::path p(fileName);
    boost::system::error_code errc;
    fs::file_status s = fs::status(p, errc);
    return !errc && is_regular_file(s);
}

AlignedFileBuffer::AlignedFileBuffer()
: mDidMMap(false)
, mBuffer(nullptr)
, mAlignedSize(0)
, mBufSize(0) {

}

// If the file cannot be read, the buffer is left empty: getBufSize() returns 0 and getBuf() nullptr.
void AlignedFileBuffer::load(const std::string & fileName, bool preferMMap) {
    mDidMMap = false;
    mBuffer = nullptr;
    mBufSize = 0;
    bool useMMap = preferMMap && canMMap(fileName);
    int32_t fd;
    int flags = O_RDONLY;
#ifdef __linux__
    if (NoOSFileCaching) {
        flags |= O_DIRECT;
    }
#endif
    if (fileName == "-") {
        fd = STDIN_FILENO;
    } else {
        fd = open(fileName.c_str(), flags);
        if (fd == -1) {
            return;
        }
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return;
    }
    useMMap = useMMap && (st.st_size > 0);
    if (useMMap) {
        auto mmap_ptr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (LLVM_LIKELY(mmap_ptr != MAP_FAILED)) {
            mBuffer = reinterpret_cast<char *>(mmap_ptr);
            mBufSize = st.st_size;
            mDidMMap = true;
        }
    } else {
        mAlignedSize = (st.st_size + mAlignment - 1) & -mAlignment;
        mBuffer = mAlloc.allocate(mAlignedSize, 0);
        if (mBuffer == nullptr) {
            report_fatal_error("Unable to allocate aligned file buffer");
        }
        // read may return fewer bytes than requested, and on macOS fails for more than INT_MAX bytes
        constexpr size_t MaximumReadSize = 1UL << 30;
        const size_t size = st.st_size;
        size_t total = 0;
        while (total < size) {
            const auto n = read(fd, mBuffer + total, std::min(size - total, MaximumReadSize));
            if (n > 0) {
                total += n;
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                mAlloc.deallocate(mBuffer, 0);
                mBuffer = nullptr;
                total = 0;
                break;
            }
        }
        mBufSize = total;
    }
    close(fd);
}

size_t AlignedFileBuffer::getBufSize() {
    return mBufSize;
}

char * AlignedFileBuffer::getBuf() {
    return mBuffer;
}

void AlignedFileBuffer::release() {
    if (mDidMMap) {
        munmap(reinterpret_cast<void *>(mBuffer), mBufSize);
    } else if (mBuffer) {
        mAlloc.deallocate(mBuffer, 0);
    }
    mDidMMap = false;
    mBuffer = nullptr;
    mBufSize = 0;
}


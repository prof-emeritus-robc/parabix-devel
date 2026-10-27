/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Pipelines whose source is CreateEmptyStreamSet: the kernels consuming the
//  empty stream sets terminate, and the file written has no bytes.

#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <llvm/Support/raw_ostream.h>
#include <toolchain/toolchain.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

using namespace kernel;

typedef void (*TestFunctionType)(const char * outputFile);

//  Run the pipeline f, writing to a temporary file, and check that the file is empty.
static bool checkEmpty(const char * testName, TestFunctionType f) {
    char path[] = "/tmp/test_emptystreamset_XXXXXX";
    const int fd = mkstemp(path);
    if (fd == -1) {
        llvm::errs() << testName << " test failed: cannot create a temporary file\n";
        return true;
    }
    close(fd);
    f(path);
    struct stat st;
    const bool failed = stat(path, &st) != 0 || st.st_size != 0;
    if (failed) {
        llvm::errs() << testName << " test failed: " << st.st_size << " bytes written\n";
    }
    unlink(path);
    return failed;
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {});
    CPUDriver driver("test");
    bool hasError = false;
    {
        //  An empty byte stream, transposed to basis bits and back.
        auto P = CreatePipeline(driver, Input<const char *>("outputFile"));
        StreamSet * const Bytes = P.CreateEmptyStreamSet(1, 8);
        StreamSet * const Basis = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<S2PKernel>(Bytes, Basis);
        StreamSet * const Out = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(Basis, Out);
        P.CreateKernelCall<FileSink>(P.getInputScalar("outputFile"), Out);
        hasError |= checkEmpty("S2P/P2S", reinterpret_cast<TestFunctionType>(P.compile()));
    }
    {
        //  Empty basis bits filtered by an empty mask.
        auto P = CreatePipeline(driver, Input<const char *>("outputFile"));
        StreamSet * const Mask = P.CreateEmptyStreamSet(1, 1);
        StreamSet * const Basis = P.CreateEmptyStreamSet(8, 1);
        StreamSet * const Filtered = P.CreateStreamSet(8, 1);
        FilterByMask(P, Mask, Basis, Filtered);
        StreamSet * const Out = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(Filtered, Out);
        P.CreateKernelCall<FileSink>(P.getInputScalar("outputFile"), Out);
        hasError |= checkEmpty("FilterByMask", reinterpret_cast<TestFunctionType>(P.compile()));
    }
    return hasError ? -1 : 0;
}

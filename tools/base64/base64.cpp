/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/util/radix64.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <llvm/Support/CommandLine.h>
#include <iostream>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef ENABLE_PAPI
#include <util/papi_helper.hpp>
#endif


using namespace llvm;
using namespace codegen;

static cl::OptionCategory base64Options("base64 Options",
                                            "Transcoding control options.");

static cl::list<std::string> inputFiles(cl::Positional, cl::desc("<input file ...>"), cl::ZeroOrMore, cl::cat(base64Options));
static toolio::RegisterInputOptions InputOptions;
static toolio::RegisterOutputOptions OutputOptions;

using namespace kernel;

typedef void (*base64FunctionType)(const uint32_t useMMap, const uint32_t fd, const uint32_t outputFD);

auto base64PipelineGen(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>("useMMap"), Input<uint32_t>("fd"), Input<uint32_t>("outputFD"));
    Scalar * const fileDescriptor = P.getInputScalar("fd");
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<FDSourceKernel>(P.getInputScalar("useMMap"), fileDescriptor, ByteStream);
    StreamSet * const Expanded3_4Out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<expand3_4Kernel>(ByteStream, Expanded3_4Out);
    StreamSet * const Radix64out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<radix64Kernel>(Expanded3_4Out, Radix64out);
    StreamSet * const base64 = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<base64Kernel>(Radix64out, base64);
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), base64);
    return P.compile();
}

size_t file_size(const int fd) {
    struct stat st;
    if (LLVM_UNLIKELY(fstat(fd, &st) != 0)) {
        st.st_size = 0;
    }
    return st.st_size;
}

// Encodes a file ("-": the standard input), returning false (after reporting
// the error) if it cannot be read.
bool base64(base64FunctionType fn_ptr, const std::string & fileName) {
    toolio::Input in;
    if (!toolio::openInput(fileName, in)) {
        return false;
    }
    fn_ptr(in.useMMap, in.fd, toolio::outputFD());
    toolio::closeInput(in);
    return true;
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&base64Options, &toolio::InputOutputOptions, codegen::codegen_flags()});
    if (!toolio::openOutput()) {
        return 1;
    }
    std::vector<std::string> files(inputFiles.begin(), inputFiles.end());
    if (files.empty()) {
        files.push_back("-");
    }
    // (Opened before compiling, the standard input from a pipe is read meanwhile.)
    if (std::find(files.begin(), files.end(), "-") != files.end()) {
        toolio::startReadAhead(STDIN_FILENO);
    }

    CPUDriver driver("base64");
    auto fn_ptr = base64PipelineGen(driver);
    #ifdef REPORT_PAPI_TESTS
    papi::PapiCounter<4> jitExecution{{PAPI_L3_TCM, PAPI_L3_TCA, PAPI_TOT_INS, PAPI_TOT_CYC}};
    // papi::PapiCounter<3> jitExecution{{PAPI_FUL_ICY, PAPI_STL_CCY, PAPI_RES_STL}};
    jitExecution.start();
    #endif
    bool allRead = true;
    for (const std::string & fileName : files) {
        allRead &= base64(fn_ptr, fileName);
    }
    #ifdef REPORT_PAPI_TESTS
    jitExecution.stop();
    jitExecution.write(std::cerr);
    #endif
    if (!toolio::finishOutput()) {
        return 1;
    }
    return allRead ? 0 : 1;
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <boost/filesystem.hpp>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/core/streamset.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <fileselect/file_select.h>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;

static cl::OptionCategory matchpriorFlags("Command Flags", "matchprior options");
static argv::RegisterFileSelectOptions FileSelectOptions;
static toolio::RegisterInputOptions InputOptions;
static toolio::RegisterOutputOptions OutputOptions;
// The report, written to the output (toolio::out()) when complete
static std::ostringstream Report;

static cl::list<std::string> inputFiles(cl::Positional, cl::desc("<input file ...>"), cl::ZeroOrMore, cl::cat(matchpriorFlags));

static cl::opt<unsigned> priorDistance("distance", cl::desc("distance for prior match (default 2)"), cl::init(2),  cl::cat(matchpriorFlags));

static int defaultDisplayColumnWidth = 10;  // default field width


using namespace kernel;

class MatchPriorKernel final: public pablo::PabloKernel {
public:
    MatchPriorKernel(LLVMTypeSystemInterface & ts, StreamSet * const countable, Scalar * countResult);
protected:
    void generatePabloMethod() override;
};

MatchPriorKernel::MatchPriorKernel (LLVMTypeSystemInterface & ts, StreamSet * const countable, Scalar * countResult)
    : pablo::PabloKernel(ts, "matchprior_" + std::to_string(priorDistance),
    {Binding{"countable", countable}},
    {},
    {},
    {Binding{"matchCount", countResult}}) {
}

void MatchPriorKernel::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<pablo::PabloAST *> basis = getInputStreamSet("countable");

    pablo::PabloAST * mismatches = pb.createZeroes();
    for (unsigned i = 0; i < 8; i++) {
        mismatches = pb.createOr(mismatches,
                                 pb.createXor(basis[i], pb.createAdvance(basis[i], priorDistance)),
                                 "mismatches_" + std::to_string(priorDistance) + "_to_bit" + std::to_string(i));
    }
    pablo::PabloAST * matchesprior = pb.createInFile(pb.createNot(mismatches), "matchesprior");
    pablo::Var * matchCount = getOutputScalarVar("matchCount");
    pb.createAssign(matchCount, pb.createCount(matchesprior));
}

typedef uint64_t (*MatchPriorFunctionType)(uint32_t useMMap, uint32_t fd);

// The size of the input read (fstat gives none for a pipe)
static uint64_t InputSize = 0;
extern "C" void record_input_size(uint64_t size) {
    InputSize = size;
}

MatchPriorFunctionType mpPipelineGen(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>{"useMMap"}, Input<uint32_t>{"fd"}, Output<uint64_t>{"countResult"});
    Scalar * const fileDescriptor = P.getInputScalar("fd");
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    Kernel * const sourceK = P.CreateKernelCall<FDSourceKernel>(P.getInputScalar("useMMap"), fileDescriptor, ByteStream);
    StreamSet * BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    P.CreateKernelCall<MatchPriorKernel>(BasisBits, P.getOutputScalar("countResult"));
    P.CreateCall("record_input_size", record_input_size, {sourceK->getOutputScalarAt(0)});
    return P.compile();
}

// Reports a file, returning false (after reporting the error) if it cannot be read.
bool mp(MatchPriorFunctionType fn_ptr, const fs::path & path, const std::string & name) {
    const int fd = argv::openInputFile(path);
    if (LLVM_UNLIKELY(fd == -1)) {
        return false;
    }
    uint64_t countResult = fn_ptr(toolio::useMMapFor(fd), fd);
    Report << std::setw(defaultDisplayColumnWidth);
    Report << countResult << std::setw(defaultDisplayColumnWidth);
    Report << InputSize;
    if (!name.empty()) Report << " " << name;
    Report << std::endl;
    close(fd);
    return true;
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&matchpriorFlags, &argv::File_Select_Options, &toolio::InputOutputOptions, &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});
    if (!toolio::openOutput()) {
        return 1;
    }
    CPUDriver driver("mp");
    argv::FileSelectConfig config;
    config.stdinAsPath = true;
    config.useIgnoreFiles = false;
    const argv::FileSelection allFiles(driver, inputFiles, config);
    const auto fileCount = allFiles.size();

    auto matchPriorFunctionPtr = mpPipelineGen(driver);
    bool allRead = true;
    for (unsigned i = 0; i < fileCount; ++i) {
        allRead &= mp(matchPriorFunctionPtr, allFiles[i], allFiles.displayName(i));
    }
    toolio::out() << Report.str();
    if (!toolio::finishOutput()) {
        return 1;
    }
    return allRead ? 0 : 1;
}

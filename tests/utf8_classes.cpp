/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  utf8_classes: character classes computed on the UTF-8 bytes of a file
//  (kernel::CharClassesKernel over the basis bits), for comparison with a
//  reference (utf8_classes_test.py).  The classes (at most 8) are read from the
//  file given by -classes, one class per line as hexadecimal codepoint ranges
//  ("4E00-4E2F 7ED9").  The output has one byte for each byte of the input: bit
//  i is set where class i is marked, at the last byte of each character
//  (-mode=advance) or at its first byte (-mode=lookahead).

#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/unicode/charclasses.h>
#include <re/adt/re_cc.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace llvm;
using namespace kernel;

static cl::OptionCategory utf8ClassesOptions("utf8_classes Options", "utf8_classes options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(utf8ClassesOptions));
static cl::opt<std::string> classesFile("classes", cl::desc("The classes, one per line, as hexadecimal codepoint ranges"),
                                        cl::Required, cl::cat(utf8ClassesOptions));
static cl::opt<std::string> movement("mode", cl::desc("advance (mark the last byte) or lookahead (the first byte)"),
                                     cl::init("advance"), cl::cat(utf8ClassesOptions));

typedef void (*UTF8ClassesFunctionType)(uint32_t fd);

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&utf8ClassesOptions, codegen::codegen_flags()});
    std::vector<re::CC *> ccs;
    std::ifstream in(classesFile);
    if (!in) {
        errs() << "utf8_classes: cannot open " << classesFile << "\n";
        return 1;
    }
    for (std::string line; std::getline(in, line); ) {
        re::CC * const cc = re::makeCC(&cc::Unicode);
        std::istringstream ranges(line);
        for (std::string r; ranges >> r; ) {
            const size_t dash = r.find('-');
            const unsigned long lo = std::stoul(r.substr(0, dash), nullptr, 16);
            const unsigned long hi = dash == std::string::npos ? lo : std::stoul(r.substr(dash + 1), nullptr, 16);
            cc->insert_range(static_cast<UCD::codepoint_t>(lo), static_cast<UCD::codepoint_t>(hi));
        }
        ccs.push_back(cc);
    }
    if (ccs.empty() || ccs.size() > 8) {
        errs() << "utf8_classes: from 1 to 8 classes\n";
        return 1;
    }
    pablo::BitMovementMode mode;
    if (movement == "advance") {
        mode = pablo::BitMovementMode::Advance;
    } else if (movement == "lookahead") {
        mode = pablo::BitMovementMode::LookAhead;
    } else {
        errs() << "utf8_classes: -mode is advance or lookahead\n";
        return 1;
    }
    while (ccs.size() < 8) ccs.push_back(re::makeCC(&cc::Unicode));
    CPUDriver driver("utf8_classes");
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(P.getInputScalar("fd"), bytes);
    StreamSet * const basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    StreamSet * const classes = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<CharClassesKernel>(ccs, basis, classes, mode);
    StreamSet * const out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(classes, out);
    P.CreateKernelCall<StdOutKernel>(out);
    UTF8ClassesFunctionType fn = P.compile();
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        errs() << "utf8_classes: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

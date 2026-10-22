/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  region_reversal: the characters of a UTF-8 file with each region reversed
//  in place (kernel::RegionReversal over the 21-bit codepoints), for
//  comparison with a reference (region_reversal_test.py).  The barriers
//  between regions are the line breaks (unless -noregions) and the characters
//  given by -b; the region limit is --reverse-region-limit.

#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/region_reversal.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/utf8gen.h>
#include <re/adt/re_cc.h>
#include <re/toolchain/toolchain.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ConvertUTF.h>
#include <llvm/Support/raw_ostream.h>
#include <fcntl.h>
#include <unistd.h>

using namespace llvm;
using namespace kernel;

static cl::OptionCategory regionReversalOptions("region_reversal Options", "region_reversal options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(regionReversalOptions));
static cl::opt<std::string> barrierChars("b", cl::desc("Characters that are barriers between regions (UTF-8)"),
                                         cl::cat(regionReversalOptions));
static cl::opt<bool> noRegions("noregions", cl::desc("Line breaks are not barriers"), cl::cat(regionReversalOptions));

typedef void (*RegionReversalFunctionType)(uint32_t fd);

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&regionReversalOptions, codegen::codegen_flags()});
    re::CC * const barriers = re::makeCC(&cc::Unicode);
    if (!noRegions) barriers->insert('\n');
    const std::string & b = barrierChars;
    for (size_t i = 0; i < b.size(); ) {
        UTF32 cp;
        const UTF8 * src = reinterpret_cast<const UTF8 *>(b.data() + i);
        const UTF8 * const srcEnd = reinterpret_cast<const UTF8 *>(b.data() + b.size());
        UTF32 * dst = &cp;
        if (ConvertUTF8toUTF32(&src, srcEnd, &dst, dst + 1, strictConversion) == sourceIllegal) {
            errs() << "region_reversal: -b is not valid UTF-8\n";
            return 1;
        }
        barriers->insert(cp);
        i = src - reinterpret_cast<const UTF8 *>(b.data());
    }
    CPUDriver driver("region_reversal");
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(P.getInputScalar("fd"), bytes);
    StreamSet * const basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    StreamSet * const u21u8 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(basis, u21u8);
    StreamSet * const u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(basis, u8index);
    StreamSet * const u21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, u21u8, u21);
    StreamSet * const barrierMarks = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{barriers}, u21, barrierMarks);
    StreamSet * const reversed = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<RegionReversal>(barrierMarks, u21, reversed, re::ReverseRegionLimit);
    StreamSet * const u8basis = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, reversed, u8basis);
    StreamSet * const out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(u8basis, out);
    P.CreateKernelCall<StdOutKernel>(out);
    RegionReversalFunctionType fn = P.compile();
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        errs() << "region_reversal: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

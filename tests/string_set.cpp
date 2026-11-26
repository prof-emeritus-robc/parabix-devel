/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  string_set: the longest matches of a set with strings in the codepoints of a
//  UTF-8 file (kernel::StringSetKernel), for comparison with a reference
//  (string_set_test.py).  The set is read from the file given by -set: its
//  first line holds the single characters as hexadecimal codepoint ranges
//  ("61-7A 3B1"), each further line a string as hexadecimal codepoints
//  ("65 31E").  The output has one byte for each codepoint of the input: bit i
//  is set where the longest match has the i-th of the set's lengths
//  (ascending), beginning there (or with -backward, ending there).  With -mux,
//  the kernel reads the bits of a multiplexed code of the set's classes (its
//  single characters, and each character of its strings) instead of the
//  21-bit basis.

#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/string_set.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8_support.h>
#include <re/adt/re_cc.h>
#include <re/alphabet/multiplex_CCs.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

using namespace llvm;
using namespace kernel;

static cl::OptionCategory stringSetOptions("string_set Options", "string_set options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(stringSetOptions));
static cl::opt<std::string> setFile("set", cl::desc("The set: its single characters, then one string per line"),
                                    cl::Required, cl::cat(stringSetOptions));
static cl::opt<bool> backward("backward", cl::desc("Mark the ends of the longest matches ending at each position"),
                              cl::init(false), cl::cat(stringSetOptions));
static cl::opt<bool> mux("mux", cl::desc("Compute the matches over a multiplexed code of the set's classes"),
                         cl::init(false), cl::cat(stringSetOptions));

typedef void (*StringSetFunctionType)(uint32_t fd);

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&stringSetOptions, codegen::codegen_flags()});
    std::ifstream in(setFile);
    if (!in) {
        errs() << "string_set: cannot open " << setFile << "\n";
        return 1;
    }
    UCD::UnicodeSet chars;
    std::vector<std::u32string> strings;
    std::string line;
    if (std::getline(in, line)) {
        std::istringstream ranges(line);
        for (std::string r; ranges >> r; ) {
            const size_t dash = r.find('-');
            const unsigned long lo = std::stoul(r.substr(0, dash), nullptr, 16);
            const unsigned long hi = dash == std::string::npos ? lo : std::stoul(r.substr(dash + 1), nullptr, 16);
            chars.insert_range(static_cast<UCD::codepoint_t>(lo), static_cast<UCD::codepoint_t>(hi));
        }
    }
    while (std::getline(in, line)) {
        std::istringstream cps(line);
        std::u32string s;
        for (std::string c; cps >> c; ) s.push_back(static_cast<char32_t>(std::stoul(c, nullptr, 16)));
        if (s.size() >= 2) strings.push_back(s);
    }
    std::unique_ptr<cc::MultiplexedAlphabet> mpx;
    std::function<UCD::UnicodeSet (const UCD::UnicodeSet &)> code;
    if (mux) {
        cc::CC_Set classes;
        if (!chars.empty()) classes.push_back(re::makeCC(UCD::UnicodeSet(chars), &cc::Unicode));
        for (const std::u32string & s : strings) {
            for (char32_t c : s) classes.push_back(re::makeCC(static_cast<UCD::codepoint_t>(c), &cc::Unicode));
        }
        mpx.reset(cc::makeMultiplexedAlphabet("string_set", classes));
        code = [&](const UCD::UnicodeSet & s) -> UCD::UnicodeSet {
            return *mpx->transformCC(re::makeCC(UCD::UnicodeSet(s), &cc::Unicode));
        };
    }
    const StringSetSpec spec = makeStringSetSpec(chars, strings, backward, code);
    if (spec.lengths.size() > 8) {
        errs() << "string_set: at most 8 lengths\n";
        return 1;
    }
    CPUDriver driver("string_set");
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(P.getInputScalar("fd"), bytes);
    StreamSet * const basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    StreamSet * const u21_u8indexed = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(basis, u21_u8indexed);
    StreamSet * const u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(basis, u8index);
    StreamSet * const u21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, u21_u8indexed, u21);
    StreamSet * setBasis = u21;
    if (mux) {
        std::vector<re::CC *> bits(mpx->getMultiplexedCCs().begin(), mpx->getMultiplexedCCs().end());
        setBasis = P.CreateStreamSet(bits.size(), 1);
        P.CreateKernelCall<CharClassesKernel>(bits, u21, setBasis);
    }
    StreamSet * const lengths = P.CreateStreamSet(spec.lengths.size(), 1);
    P.CreateKernelCall<StringSetKernel>(spec, setBasis, lengths);
    StreamSet * const out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(lengths, out);
    P.CreateKernelCall<StdOutKernel>(out);
    StringSetFunctionType fn = P.compile();
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        errs() << "string_set: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  match_starts: the starts of the matches of up to 8 regular expressions
//  within the lines of a UTF-8 file, computed by the MatchStarts mode of the
//  regular expression engine (RE_PipelineBuilder::matchStartsPipeline), for
//  comparison with a reference (match_starts_test.py).
//
//  Output: a line for each RE giving its mode ("byte" or "unicode"), then a
//  byte for each byte of the file, whose bit i marks the starts of RE i.  With
//  full Unicode indexing, a start is marked at the last byte of its character.

#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/re/regexp_engine.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <pablo/pablo.h>
#include <kernel/util/linebreak_kernel.h>
#include <re/parse/parser.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <fcntl.h>
#include <unistd.h>

using namespace llvm;
using namespace kernel;

static cl::OptionCategory matchStartsOptions("match_starts Options", "match_starts options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(matchStartsOptions));
static cl::list<std::string> patterns("e", cl::desc("A regular expression (up to 8)"), cl::OneOrMore, cl::cat(matchStartsOptions));

typedef void (*MatchStartsFunctionType)(uint32_t fd);

//  The streams of the REs, in order, as 8 streams (the rest zero).
class PackStarts final : public pablo::PabloKernel {
public:
    PackStarts(LLVMTypeSystemInterface & ts, const std::vector<StreamSet *> & starts, StreamSet * packed)
    : pablo::PabloKernel(ts, "PackStarts" + std::to_string(starts.size()), [&] {
          Bindings inputs;
          for (unsigned i = 0; i < starts.size(); i++) inputs.emplace_back("starts" + std::to_string(i), starts[i]);
          return inputs;
      }(), {Binding{"packed", packed}}), mCount(starts.size()) {}
protected:
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        std::vector<pablo::PabloAST *> packed;
        for (unsigned i = 0; i < 8; i++) {
            packed.push_back(i < mCount ? getInputStreamSet("starts" + std::to_string(i))[0] : pb.createZeroes());
        }
        writeOutputStreamSet("packed", packed);
    }
private:
    const unsigned mCount;
};

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&matchStartsOptions, codegen::codegen_flags()});
    if (patterns.size() > 8) {
        errs() << "match_starts: at most 8 regular expressions\n";
        return 1;
    }
    CPUDriver driver("match_starts");
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(P.getInputScalar("fd"), bytes);
    StreamSet * const basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    StreamSet * const breaks = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnixLinesKernelBuilder>(bytes, breaks, UnterminatedLineAtEOF::Add1);
    StreamSet * const lineStarts = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<LineStartsKernel>(breaks, lineStarts);
    std::vector<StreamSet *> results;
    for (const std::string & pattern : patterns) {
        RE_ModeOptions opts;
        opts.goal = re::RE_Goal::MatchStarts;
        const PreparedRE prepared = prepareRE(re::RE_Parser::parse(pattern), opts);
        RE_PipelineBuilder engine(P, RE_context{&cc::UTF8, basis, lineStarts, breaks});
        engine.setLineBreakStream(breaks);
        StreamSet * starts = P.CreateStreamSet(1);
        engine.matchStartsPipeline(prepared, starts);
        if (engine.usesUnicodeIndexing()) {
            StreamSet * const spread = P.CreateStreamSet(1);
            SpreadByMask(P, engine.getU8Index(), starts, spread);
            starts = spread;
        }
        outs() << (engine.usesUnicodeIndexing() ? "unicode" : "byte") << "\n";
        results.push_back(starts);
    }
    StreamSet * const marks = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<PackStarts>(results, marks);
    StreamSet * const out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(marks, out);
    P.CreateKernelCall<StdOutKernel>(out);
    MatchStartsFunctionType fn = P.compile();
    outs().flush();
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        errs() << "match_starts: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

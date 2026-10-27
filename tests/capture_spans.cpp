/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  capture_spans: the boundaries of captures in the matches of a search and
//  replace (RE_PipelineBuilder::captureSpansPipeline) within a UTF-8 file, for
//  comparison with a reference (capture_spans_test.py).  Each -e PATTERN is
//  prepared with the replacement $1...$n (n: its number of captures), and its
//  capture -c N is reported (at most 2 pairs).
//
//  Output: a byte for each byte of the file, whose bits 3k, 3k + 1 and 3k + 2
//  mark the start, the coverage and the follow of the capture of pair k, at
//  the last byte of their characters (full Unicode indexing).  With
//  -noregions, the file is not divided into lines (match regions), and the
//  text is extended by one position (one more byte).

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
#include <re/analysis/replacement_analysis.h>
#include <re/parse/parser.h>
#include <re/parse/replacement_parser.h>
#include <toolchain/toolchain.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <fcntl.h>
#include <unistd.h>

using namespace llvm;
using namespace kernel;

static cl::OptionCategory captureSpansOptions("capture_spans Options", "capture_spans options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(captureSpansOptions));
static cl::list<std::string> patterns("e", cl::desc("A search pattern (up to 2)"), cl::OneOrMore, cl::cat(captureSpansOptions));
static cl::list<unsigned> captureNumbers("c", cl::desc("The capture reported, for each pattern"), cl::OneOrMore,
                                         cl::cat(captureSpansOptions));
static cl::opt<bool> noRegions("noregions", cl::desc("Without match regions (lines)"), cl::cat(captureSpansOptions));

typedef void (*CaptureSpansFunctionType)(uint32_t fd);

//  The streams of the pairs, in order, as 8 streams (the rest zero).
class PackSpans final : public pablo::PabloKernel {
public:
    PackSpans(LLVMTypeSystemInterface & ts, const std::vector<StreamSet *> & spans, StreamSet * packed)
    : pablo::PabloKernel(ts, "PackSpans" + std::to_string(spans.size()), [&] {
          Bindings inputs;
          for (unsigned i = 0; i < spans.size(); i++) inputs.emplace_back("spans" + std::to_string(i), spans[i]);
          return inputs;
      }(), {Binding{"packed", packed}}), mCount(spans.size()) {}
protected:
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        std::vector<pablo::PabloAST *> packed;
        for (unsigned i = 0; i < mCount; i++) {
            for (pablo::PabloAST * s : getInputStreamSet("spans" + std::to_string(i))) packed.push_back(s);
        }
        while (packed.size() < 8) packed.push_back(pb.createZeroes());
        writeOutputStreamSet("packed", packed);
    }
private:
    const unsigned mCount;
};

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&captureSpansOptions, codegen::codegen_flags()});
    if ((patterns.size() > 2) || (patterns.size() != captureNumbers.size())) {
        errs() << "capture_spans: one capture (-c) for each of at most 2 patterns (-e)\n";
        return 1;
    }
    CPUDriver driver("capture_spans");
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(P.getInputScalar("fd"), bytes);
    StreamSet * const basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    StreamSet * breaks = nullptr;
    StreamSet * lineStarts = nullptr;
    if (!noRegions) {
        breaks = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<UnixLinesKernelBuilder>(bytes, breaks, UnterminatedLineAtEOF::Add1);
        lineStarts = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<LineStartsKernel>(breaks, lineStarts);
    }
    std::vector<StreamSet *> results;
    for (unsigned k = 0; k < patterns.size(); k++) {
        re::RE * const search = re::RE_Parser::parse(patterns[k]);
        const auto captures = re::capturesByName(search);
        std::string text, error;
        for (unsigned c = 1; c <= captures.size(); c++) text += "$" + std::to_string(c);
        re::RE * const replacement = re::parseReplacement(text, search, error);
        if (replacement == nullptr) {
            errs() << "capture_spans: " << error << "\n";
            return 1;
        }
        const PreparedSearchReplace sr = prepareSearchReplace(search, replacement);
        RE_PipelineBuilder engine(P, RE_context{&cc::UTF8, basis, lineStarts, breaks});
        if (breaks) engine.setLineBreakStream(breaks);
        StreamSet * spans = P.CreateStreamSet(3);
        engine.captureSpansPipeline(sr, {"\\" + std::to_string(captureNumbers[k])}, spans);
        StreamSet * const spread = P.CreateStreamSet(3);
        SpreadByMask(P, engine.getU8Index(), spans, spread);
        results.push_back(spread);
    }
    StreamSet * const marks = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<PackSpans>(results, marks);
    StreamSet * const out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(marks, out);
    P.CreateKernelCall<StdOutKernel>(out);
    CaptureSpansFunctionType fn = P.compile();
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        errs() << "capture_spans: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

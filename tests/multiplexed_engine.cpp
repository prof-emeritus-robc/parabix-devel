/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  multiplexed_engine: the regular expression engine over the codes of a
//  multiplexed alphabet (RE_context with a cc::MultiplexedAlphabet of Unicode)
//  compared with the engine over the 21-bit Unicode basis, for up to 4
//  regular expressions within the lines of a UTF-8 file (for
//  multiplexed_engine_test.py).  The alphabet is that of the CCs of the RE as
//  prepared (including the definitions of names); its basis is computed from
//  the Unicode basis.
//
//  Each pattern is given with its kind: "e:" the ends of the matches
//  (matchSearchPipeline), "s:" their starts (matchStartsPipeline), "cN:" the
//  start, coverage and follow of capture N of a search and replace
//  (captureSpansPipeline).
//
//  Output: a byte for each character position of the results, whose bit i
//  marks a difference of the results of pattern i over the two bases, and
//  bit 4 + i a result of pattern i over the Unicode basis.

#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/re/regexp_engine.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/util/linebreak_kernel.h>
#include <pablo/pablo.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/analysis/collect_ccs.h>
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

static cl::OptionCategory multiplexedEngineOptions("multiplexed_engine Options", "multiplexed_engine options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(multiplexedEngineOptions));
static cl::list<std::string> patterns("e", cl::desc("A kind (e:, s: or cN:) and a regular expression (up to 4)"),
                                      cl::OneOrMore, cl::cat(multiplexedEngineOptions));
static cl::opt<bool> noRegions("noregions", cl::desc("The text as a whole, without line regions"),
                               cl::cat(multiplexedEngineOptions));

typedef void (*MultiplexedEngineFunctionType)(uint32_t fd);

//  For each pattern, the differences of its results over the two bases (any
//  stream), and its results over the Unicode basis, as 8 streams.
class CompareResults final : public pablo::PabloKernel {
public:
    CompareResults(LLVMTypeSystemInterface & ts, const std::vector<StreamSet *> & unicode,
                   const std::vector<StreamSet *> & multiplexed, StreamSet * packed)
    : pablo::PabloKernel(ts, "CompareResults" + signature(unicode), [&] {
          Bindings inputs;
          for (unsigned i = 0; i < unicode.size(); i++) {
              inputs.emplace_back("u" + std::to_string(i), unicode[i]);
              inputs.emplace_back("m" + std::to_string(i), multiplexed[i]);
          }
          return inputs;
      }(), {Binding{"packed", packed}}), mCount(unicode.size()) {}
protected:
    static std::string signature(const std::vector<StreamSet *> & results) {
        std::string s;
        for (StreamSet * r : results) s += "_" + std::to_string(r->getNumElements());
        return s;
    }
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        std::vector<pablo::PabloAST *> diffs(4, pb.createZeroes());
        std::vector<pablo::PabloAST *> found(4, pb.createZeroes());
        for (unsigned i = 0; i < mCount; i++) {
            const std::vector<pablo::PabloAST *> u = getInputStreamSet("u" + std::to_string(i));
            const std::vector<pablo::PabloAST *> m = getInputStreamSet("m" + std::to_string(i));
            for (unsigned j = 0; j < u.size(); j++) {
                diffs[i] = pb.createOr(diffs[i], pb.createXor(u[j], m[j]));
                found[i] = pb.createOr(found[i], u[j]);
            }
        }
        diffs.insert(diffs.end(), found.begin(), found.end());
        writeOutputStreamSet("packed", diffs);
    }
private:
    const unsigned mCount;
};

int main(int argc, char * argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&multiplexedEngineOptions, codegen::codegen_flags()});
    if (patterns.size() > 4) {
        errs() << "multiplexed_engine: at most 4 regular expressions\n";
        return 1;
    }
    CPUDriver driver("multiplexed_engine");
    auto P = CreatePipeline(driver, Input<uint32_t>("fd"));
    StreamSet * const bytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(P.getInputScalar("fd"), bytes);
    StreamSet * const basis = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(bytes, basis);
    StreamSet * breaks = nullptr;
    if (!noRegions) {
        breaks = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<UnixLinesKernelBuilder>(bytes, breaks, UnterminatedLineAtEOF::Add1);
    }
    // The Unicode basis, and the regions, at the positions of the characters.
    StreamSet * const u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(basis, u8index, breaks);
    StreamSet * const u21_u8indexed = P.CreateStreamSet(21);
    P.CreateKernelCall<UTF8_Decoder>(basis, u21_u8indexed);
    StreamSet * const u21 = P.CreateStreamSet(21);
    FilterByMask(P, u8index, u21_u8indexed, u21);
    StreamSet * follows = nullptr;
    StreamSet * starts = nullptr;
    if (breaks) {
        follows = P.CreateStreamSet(1);
        FilterByMask(P, u8index, breaks, follows);
        starts = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<LineStartsKernel>(follows, starts);
    }
    std::vector<StreamSet *> unicodeResults;
    std::vector<StreamSet *> multiplexedResults;
    for (const std::string & given : patterns) {
        const auto colon = given.find(':');
        if (colon == std::string::npos) {
            errs() << "multiplexed_engine: a pattern must have a kind (e:, s: or cN:)\n";
            return 1;
        }
        const std::string kind = given.substr(0, colon);
        re::RE * const parsed = re::RE_Parser::parse(given.substr(colon + 1));
        PreparedRE prepared;
        PreparedSearchReplace sr;
        if (kind[0] == 'c') {
            const auto captures = re::capturesByName(parsed);
            std::string text, error;
            for (unsigned c = 1; c <= captures.size(); c++) text += "$" + std::to_string(c);
            re::RE * const replacement = re::parseReplacement(text, parsed, error);
            if (replacement == nullptr) {
                errs() << "multiplexed_engine: " << error << "\n";
                return 1;
            }
            sr = prepareSearchReplace(parsed, replacement);
            prepared = sr.search;
        } else {
            RE_ModeOptions opts;
            if (kind == "s") opts.goal = re::RE_Goal::MatchStarts;
            prepared = prepareRE(parsed, opts);
        }
        const cc::CC_Set ccs = re::collectCCs(prepared.re, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        cc::MultiplexedAlphabet * const mpx = cc::makeMultiplexedAlphabet("mpx", ccs);
        if (mpx->getMultiplexedCCs().empty()) {
            errs() << "multiplexed_engine: " << given << " has no CCs to multiplex\n";
            return 1;
        }
        StreamSet * const codes = P.CreateStreamSet(mpx->getMultiplexedCCs().size());
        P.CreateKernelFamilyCall<CharClassesKernel>(mpx->getMultiplexedCCs(), u21, codes);
        outs() << given << ": " << mpx->getSize() << " codes, " << mpx->getMultiplexedCCs().size() << " bits\n";
        for (const cc::Alphabet * encoding : {static_cast<const cc::Alphabet *>(&cc::Unicode),
                                              static_cast<const cc::Alphabet *>(mpx)}) {
            const bool multiplexed = (encoding == mpx);
            RE_PipelineBuilder engine(P, RE_context{encoding, multiplexed ? codes : u21, starts, follows});
            StreamSet * results = nullptr;
            if (kind == "e") {
                results = P.CreateStreamSet(1);
                engine.matchSearchPipeline(prepared, results);
            } else if (kind == "s") {
                results = P.CreateStreamSet(1);
                engine.matchStartsPipeline(prepared, results);
            } else {
                results = P.CreateStreamSet(3);
                engine.captureSpansPipeline(sr, {"\\" + kind.substr(1)}, results);
            }
            (multiplexed ? multiplexedResults : unicodeResults).push_back(results);
        }
    }
    StreamSet * const marks = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<CompareResults>(unicodeResults, multiplexedResults, marks);
    StreamSet * const out = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(marks, out);
    P.CreateKernelCall<StdOutKernel>(out);
    MultiplexedEngineFunctionType fn = P.compile();
    outs() << "--\n";
    outs().flush();
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (fd == -1) {
        errs() << "multiplexed_engine: cannot open " << inputFile << "\n";
        return 1;
    }
    fn(fd);
    close(fd);
    return 0;
}

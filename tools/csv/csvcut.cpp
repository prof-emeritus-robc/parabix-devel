/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <cstdio>
#include <vector>
#include <csv/csv_cmdline.h>
#include <csv/csv_parser.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/IR/Module.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/run_index.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/scan/scanmatchgen.h>
#include <re/adt/re_name.h>
#include <re/cc/cc_kernel.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <string>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>
using namespace kernel;
using namespace llvm;
using namespace pablo;

//  These declarations are for command line processing.
//  See the LLVM CommandLine Library Manual https://llvm.org/docs/CommandLine.html
static cl::opt<bool> FilterBasisBits("FilterBasisBits", cl::desc("Perform filtering on basis bits rather than on byte stream"), 
    cl::init(false), cl::cat(csv::CSV_Options), cl::Hidden);

typedef void (*CSVFunctionType)(uint32_t useMMap, uint32_t fd, uint32_t outputFD);

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

CSVFunctionType generatePipeline(CPUDriver & driver, const std::vector<unsigned> & colNos) {

    // A Parabix program is build as a set of kernel calls called a pipeline.
    // A pipeline is construction using a Parabix driver object.
    auto P = CreatePipeline(driver, Input<uint32_t>{"useMMap"}, Input<uint32_t>{"inputFileDecriptor"}, Input<uint32_t>{"outputFD"});
    //  The program will use a file descriptor as an input.
    Scalar * fileDescriptor = P.getInputScalar("inputFileDecriptor");
    // File data from mmap
    StreamSet * ByteStream = P.CreateStreamSet(1, 8);
    //  FDSourceKernel is a Parabix Kernel that produces a stream of bytes
    //  from a file descriptor, mapping the file into memory or reading it.
    P.CreateKernelCall<FDSourceKernel>(P.getInputScalar("useMMap"), fileDescriptor, ByteStream);

    //  The Parabix basis bits representation is created by the Parabix S2P kernel.
    //  S2P stands for serial-to-parallel.
    StreamSet * BasisBits = P.CreateStreamSet(8);
    Selected_S2P(P, ByteStream, BasisBits);
    SHOW_BYTES(ByteStream);
    SHOW_BIXNUM(BasisBits);

    csv::CSV_Parser parser(P, csv::QuoteChar, csv::FieldDelimiter);

    parser.setSource(BasisBits);

    StreamSet * fieldStarts = parser.getFieldStarts();
    StreamSet * fieldFollows = parser.getFieldFollows();
    StreamSet * recordSeparators = parser.getLineEnds();

    StreamSet * Selected = P.CreateStreamSet(1);
    csv::ColumnSelectionMask(P, recordSeparators, fieldStarts, fieldFollows, Selected, colNos, /*forCut = */ true);
    SHOW_STREAM(Selected);

    StreamSet * Filtered = P.CreateStreamSet(1, 8);
    if (FilterBasisBits) {
        StreamSet * filteredBasis = P.CreateStreamSet(8);
        FilterByMask(P, Selected, BasisBits, filteredBasis);
        P.CreateKernelCall<P2SKernel>(filteredBasis, Filtered);
        SHOW_BIXNUM(filteredBasis);
    } else {
        FilterByMask(P, Selected, ByteStream, Filtered);
    }
    SHOW_BYTES(Filtered);
    //  The StdOut kernel writes a byte stream to standard output.
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), Filtered);
    return P.compile();
}

int main(int argc, char *argv[]) {
    llvm_shutdown_obj shutdown;
    csv::InitializeCommandLineInterface(argc, argv);

    //  The input is read twice (its first line for the headers), so the
    //  standard input is copied to a temporary file if it is a pipe.
    toolio::Input in;
    if (!toolio::openSeekableInput(csv::inputFile, in) || !toolio::openOutput()) {
        return csv::InternalFailureCode;
    }
    std::vector<std::string> headers = csv::get_CSV_headers(in);

    std::vector<unsigned> colNos = csv::getColumnArgs(headers);

    CPUDriver driver("csv_function");
    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    CSVFunctionType fn = generatePipeline(driver, colNos);
    //  The compiled function "fn" can now be used.  It takes whether to map
    //  the input, and the input and output file descriptors.
    fn(in.useMMap, in.fd, toolio::outputFD());
    toolio::closeInput(in);
    return toolio::finishOutput() ? csv::SuccessExitCode : csv::InternalFailureCode;
}

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
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/scan/data_rearrangement.h>
#include <kernel/unicode/charclasses.h>
#include <re/adt/re_cc.h>
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

static cl::opt<bool> Standardize("standardize",
    cl::desc("Write standard CSV: ',' delimiters, '\"' quotes and LF line ends (the default); "
             "--standardize=0 keeps the delimiters, quotes and line ends of the input"),
    cl::init(true), cl::cat(csv::CSV_Options));

typedef void (*CSVFunctionType)(uint32_t useMMap, uint32_t fd, uint32_t outputFD);

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

//  Each record of numFields fields is rearranged to the fields of fieldOrder
//  (1-based field numbers), in any order.
CSVFunctionType generatePipeline(CPUDriver & driver, unsigned numFields, const std::vector<unsigned> & fieldOrder) {

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

    StreamSet * Data = nullptr;
    StreamSet * DataSeparators = nullptr;
    unsigned delimiterLength = 1;
    int recordSeparatorPrefix = DataRearrangementKernel::NoPrefix;
    if (Standardize && (csv::QuoteChar != U'"' || csv::FieldDelimiter != U',')) {
        //  Translation to standard CSV, then rearrangement.
        StreamSet * StandardBasis = P.CreateStreamSet(8);
        DataSeparators = P.CreateStreamSet(1);
        csv::StandardizeCSV(P, BasisBits, csv::QuoteChar, csv::FieldDelimiter, StandardBasis, DataSeparators);
        SHOW_BIXNUM(StandardBasis);
        Data = P.CreateStreamSet(1, 8);
        P.CreateKernelCall<P2SKernel>(StandardBasis, Data);
    } else {
        csv::CSV_Parser parser(P, csv::QuoteChar, csv::FieldDelimiter);
        parser.setSource(BasisBits);
        //  A delimiter of several UTF-8 code units is marked at its first code unit,
        //  so that it is kept whole: the field it ends stops before it, and the
        //  rearrangement copies all of its code units.
        delimiterLength = csv::UTF8_Length(csv::FieldDelimiter);
        StreamSet * DelimiterStarts = nullptr;
        if (delimiterLength > 1) {
            DelimiterStarts = P.CreateStreamSet(1);
            P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{re::makeCC(csv::FieldDelimiter)},
                                                  BasisBits, DelimiterStarts, pablo::BitMovementMode::LookAhead);
            SHOW_STREAM(DelimiterStarts);
        }
        //  Separators: unquoted delimiters and record-ending LFs.
        StreamSet * Separators = P.CreateStreamSet(1);
        if (Standardize) {
            //  Standard CSV but for CRLFs: the CR of each CRLF is deleted, so that
            //  each record ends with its LF.
            StreamSet * KeepMask = P.CreateStreamSet(1);
            csv::FieldSeparatorsAndCRMask(P, parser.getCsvCCs(), parser.getQuotedData(), Separators, KeepMask, DelimiterStarts);
            SHOW_STREAM(KeepMask);
            Data = P.CreateStreamSet(1, 8);
            FilterByMask(P, KeepMask, ByteStream, Data);
            DataSeparators = P.CreateStreamSet(1);
            FilterByMask(P, KeepMask, Separators, DataSeparators);
        } else {
            //  The input unchanged: the CR of a CRLF is kept with its LF.
            csv::FieldSeparatorsAndCRMask(P, parser.getCsvCCs(), parser.getQuotedData(), Separators, nullptr, DelimiterStarts);
            Data = ByteStream;
            DataSeparators = Separators;
            recordSeparatorPrefix = '\r';
        }
    }
    SHOW_STREAM(DataSeparators);

    StreamSet * Rearranged = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<DataRearrangementKernel>(numFields, fieldOrder, DataSeparators, Data, Rearranged,
                                                delimiterLength, recordSeparatorPrefix);
    SHOW_BYTES(Rearranged);
    //  The StdOut kernel writes a byte stream to standard output.
    P.CreateKernelCall<FDSinkKernel>(P.getInputScalar("outputFD"), Rearranged);
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

    //  Columns may be given in any order, each at most once.
    std::vector<unsigned> colNos = csv::getColumnArgs(headers, /* anyOrder = */ true);
    const unsigned numFields = headers.size();
    std::vector<bool> seen(numFields, false);
    for (const auto c : colNos) {
        if (seen[c]) {
            llvm::errs() << "csvcut: column " << (c + 1) << " is selected more than once\n";
            return csv::UsageErrorCode;
        }
        seen[c] = true;
    }
    std::vector<unsigned> fieldOrder;
    for (const auto c : colNos) {
        fieldOrder.push_back(c + 1);
    }

    CPUDriver driver("csv_function");
    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    CSVFunctionType fn = generatePipeline(driver, numFields, fieldOrder);
    //  The compiled function "fn" can now be used.  It takes whether to map
    //  the input, and the input and output file descriptors.
    fn(in.useMMap, in.fd, toolio::outputFD());
    toolio::closeInput(in);
    return toolio::finishOutput() ? csv::SuccessExitCode : csv::InternalFailureCode;
}

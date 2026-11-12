/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */


#include <cstdio>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <csv/csv_cmdline.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/raw_ostream.h>
#include <toolchain/toolchain.h>
#include <toolchain/toolio.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/locale/encoding_utf.hpp>

using namespace llvm;

//  These declarations are for command line processing.
//  See the LLVM CommandLine Library Manual https://llvm.org/docs/CommandLine.html

namespace csv {

cl::OptionCategory CSV_Options("CSV Processing Options", "Options to control parsing and field selection of input files");

std::string inputFile;
bool HeaderSpecNamesFile;
std::string HeaderSpec;
char32_t FieldDelimiter;
char32_t QuoteChar;
bool ZeroIndexing;

namespace {

// The CSV options, created by InitializeCommandLineInterface, so that only
// the CSV tools have them (see RegisterFileSelectOptions in fileselect).
struct CSVOptions {

cl::opt<std::string, true> inputFileOption{cl::location(inputFile), cl::Positional, cl::desc("<input file>"), cl::cat(csv::CSV_Options)};

// Although CSV files normally have column headers as the first line,
// we can provide them directly or from a file.
cl::opt<bool, true> HeaderSpecNamesFileOption{"headers-from-file", cl::location(HeaderSpecNamesFile), cl::desc("Interpret headers parameter as file name with header line"), cl::init(false), cl::cat(CSV_Options)};
cl::opt<std::string, true> HeaderSpecOption{"headers", cl::location(HeaderSpec), cl::desc("CSV column headers (explicit string or filename"), cl::init(""), cl::cat(CSV_Options)};

cl::opt<std::string> FieldDelimiterOption{"delimiter",
	cl::desc("Delimiter to separate fields (default comma)"), cl::init(","),
    cl::value_desc("Unicode character"), cl::cat(CSV_Options)};
cl::alias DelimterA{"d", cl::desc("Alias for --delimiter"), cl::aliasopt(FieldDelimiterOption), cl::NotHidden};
cl::opt<bool> TabDelimiter{"tabs",
	cl::desc("Use tabs as delimiter (overrides --delimiter)"), cl::init(false), cl::cat(CSV_Options)};
cl::alias TabsA{"t", cl::desc("Alias for --tabs"), cl::aliasopt(TabDelimiter), cl::NotHidden};

cl::opt<std::string> QuoteCharOption{"quotechar",
	cl::desc("Quote character (default double quote)"), cl::init("\""),
    cl::value_desc("Unicode character"), cl::cat(CSV_Options)};
cl::alias QuoteA{"q", cl::desc("Alias for --quotechar"), cl::aliasopt(QuoteCharOption), cl::NotHidden};

cl::list<std::string> Columns{"columns",
                              cl::desc("A comma-separated list of column names or indices"),
                              cl::CommaSeparated, cl::cat(csv::CSV_Options)};
//cl::alias ColumnsA{"c", cl::desc("Alias for --columns"), cl::aliasopt(Columns), cl::NotHidden};
cl::opt<bool, true> ZeroIndexingOption{"zero", cl::location(ZeroIndexing),
	cl::desc("Use 0-based rather than 1-based indices for column numbers"), cl::init(false), cl::cat(csv::CSV_Options)};

};

CSVOptions * Registered = nullptr;

CSVOptions & registeredOptions() {
    if (LLVM_UNLIKELY(Registered == nullptr)) {
        report_fatal_error("CSV options used before csv::InitializeCommandLineInterface");
    }
    return *Registered;
}

} // anonymous namespace

std::vector<std::string> parse_CSV_headers(std::string headerString) {
    std::vector<std::string> headers;
    boost::algorithm::split(headers, headerString, [] (char c) {return (c == ',');});
    for (unsigned i = 0; i < headers.size(); i++) {
        boost::algorithm::trim(headers[i]);
    }
    return headers;
}

std::vector<std::string> read_CSV_headers(std::string filename) {
    std::vector<std::string> headers;
    std::ifstream headerFile(filename.c_str());
    std::string line1;
    if (headerFile.is_open()) {
        std::getline(headerFile, line1);
        headerFile.close();
        headers = parse_CSV_headers(line1);
    } else {
        llvm::report_fatal_error(llvm::StringRef("Cannot open ") + filename);
    }
    return headers;
}

const unsigned MaxHeaderSize = 24;

// The first line of an input, read from in.start (without moving its position).
static std::vector<std::string> read_CSV_headers(const toolio::Input & in) {
    std::string line1;
    char buffer[4096];
    for (off_t offset = static_cast<off_t>(in.start); ; ) {
        const ssize_t n = pread(in.fd, buffer, sizeof(buffer), offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        const char * const end = static_cast<const char *>(std::memchr(buffer, '\n', static_cast<size_t>(n)));
        if (end) {
            line1.append(buffer, static_cast<size_t>(end - buffer));
            break;
        }
        line1.append(buffer, static_cast<size_t>(n));
        offset += n;
    }
    return parse_CSV_headers(line1);
}

std::vector<std::string> get_CSV_headers(const toolio::Input & in) {
	std::vector<std::string> headers;
    if (csv::HeaderSpec == "") {
        headers = csv::read_CSV_headers(in);
    } else if (csv::HeaderSpecNamesFile) {
        headers = csv::read_CSV_headers(csv::HeaderSpec);
    } else {
        headers = csv::parse_CSV_headers(csv::HeaderSpec);
    }
    for (auto & s : headers) {
        if (s.size() > MaxHeaderSize) {
            s = s.substr(0, MaxHeaderSize);
        }
    }
    return headers;
}

unsigned getColumn(std::string & columnSpec, std::map<std::string, unsigned> & headerMap) {
    auto f = headerMap.find(columnSpec);
    if (f != headerMap.end()) {
        return f->second;
    }
    // Not a column name - must be a column number.
    std::stringstream ss(columnSpec);
    unsigned colNo;
    ss >> colNo;
    if (ss.eof()) {
        if (ZeroIndexing) {
            return colNo;
        } else if (colNo > 0) {
            return colNo - 1;
        }
    }
    llvm::report_fatal_error(llvm::StringRef("Invalid column spec:" + columnSpec));
}

std::pair<unsigned, unsigned> getColumnRange(std::string & columnSpec, std::map<std::string, unsigned> & headerMap) {
    // We may have a column name.
    auto f = headerMap.find(columnSpec);
    if (f != headerMap.end()) {
        return std::pair<unsigned, unsigned>(f->second, f->second);
    }
    // We may have a hyphen-separated range.
    auto pos = columnSpec.find('-');
    if (pos != std::string::npos) {
        auto range_lo = columnSpec.substr(0, pos);
        auto range_hi = columnSpec.substr(pos+1);
        if (range_lo > range_hi) {
            llvm::report_fatal_error("Bad column range");
        }
        return std::pair<unsigned, unsigned>(getColumn(range_lo, headerMap), getColumn(range_hi, headerMap));
    } else {
        auto colNo = getColumn(columnSpec, headerMap);
        return std::pair<unsigned, unsigned>(colNo, colNo);
    }
}

std::vector<unsigned> getColumnArgs(std::vector<std::string> & headers) {
    std::map<std::string, unsigned> headerMap;
    for (unsigned i = 0; i < headers.size(); i++) {
        headerMap.emplace(headers[i], i);
        if (headers[i].size() > MaxHeaderSize) {
            headers[i] = headers[i].substr(0, MaxHeaderSize);
        }
    }
	std::vector<unsigned> colNos;
    if (registeredOptions().Columns.size() == 0) {
        for (unsigned i = 0; i < headers.size(); i++) {
            colNos.push_back(i);
        }
    } else {
        for (unsigned i = 0; i < registeredOptions().Columns.size(); i++) {
            //llvm::errs() << "csv::Columns[i] = " << csv::Columns[i] << "\n";
            auto rg = getColumnRange(registeredOptions().Columns[i], headerMap);
            if (rg.second >= headers.size()) {
                llvm::report_fatal_error("Column number too large");
            }
            if ((i > 0) && (rg.first < colNos.back())) {
                llvm::report_fatal_error("Column numbers must be specified in strictly increasing order");
            }
            for (auto colNo = rg.first; colNo <= rg.second; colNo++) {
                colNos.push_back(colNo);
            }
        }
    }
    return colNos;
}


//
// Handler for errors reported through llvm::report_fatal_error.  Report
// and signal error the InternalFailure exit code.
//
static void csv_error_handler(void *UserData,
							  const char * Message,
							  bool GenCrashDiag) {
    // Modified from LLVM's internal report_fatal_error logic.
    SmallVector<char, 64> Buffer;
    raw_svector_ostream OS(Buffer);
    OS << "Parabix csv error: " << Message << "\n";
    const auto MessageStr = OS.str();
    ssize_t written = ::write(2, MessageStr.data(), MessageStr.size());
    (void)written; // If something went wrong, we deliberately just give up.
    // Run the interrupt handlers to make sure any special cleanups get done, in
    // particular that we remove files registered with RemoveFileOnSignal.
    llvm::sys::RunInterruptHandlers();
    exit(InternalFailureCode);
}

const char32_t TAB = U'\t';

void InitializeCommandLineInterface(int argc, char *argv[]) {
    llvm::install_fatal_error_handler(&csv_error_handler);
    static CSVOptions opts;
    Registered = &opts;
    // (Registered here, not by static objects: the single library has this
    // code in every tool.)
    static toolio::RegisterInputOptions InputOptions;
    static toolio::RegisterOutputOptions OutputOptions;
    codegen::ParseCommandLineOptions(argc, argv, {&CSV_Options, &toolio::InputOutputOptions, &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});

    // Set the delimiter and quote characters.
    if (registeredOptions().TabDelimiter) {
        // --tabs or -t overrides field delimiter
        FieldDelimiter = TAB;
    } else {
        std::u32string delimArg = boost::locale::conv::utf_to_utf<char32_t>(registeredOptions().FieldDelimiterOption);
        if (delimArg.size() != 1) {
            report_fatal_error("Field Delimiter must be a single Unicode character");
        }
        FieldDelimiter = delimArg[0];
    }
    std::u32string quoteArg = boost::locale::conv::utf_to_utf<char32_t>(registeredOptions().QuoteCharOption);
    if (quoteArg.size() != 1) {
        report_fatal_error("Quote Character must be a single Unicode character");
    }
    QuoteChar = quoteArg[0];
}

}

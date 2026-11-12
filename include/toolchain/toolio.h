/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

// The input and output conventions of the Parabix tools:
//  - an input named "-", or none, is the standard input;
//  - the results go to the standard output, or to the file of --output
//    (--output=- is the standard output);
//  - all messages (errors, warnings, timing, traces) go to the standard error;
//  - output is colored only on a terminal (useColor).
//
// A tool registers the options it takes before parsing its command line, as
// with LLVM's codegen::RegisterCodeGenFlags, so that other tools have none of
// them:
//     static toolio::RegisterInputOptions InputOptions;     // --mmap
//     static toolio::RegisterOutputOptions OutputOptions;   // --output
// and then, e.g.,
//     toolio::Input in;
//     if (!toolio::openInput(inputFile, in) || !toolio::openOutput()) return 1;
//     fn(in.useMMap, in.fd, toolio::outputFD());     // FDSourceKernel, FDSinkKernel
//     toolio::closeInput(in);
//     return toolio::finishOutput() ? 0 : 1;

#include <cstddef>
#include <string>

namespace llvm { class raw_ostream; namespace cl { class OptionCategory; } }

namespace toolio {

// The options of this file, for ParseCommandLineOptions to show.
extern llvm::cl::OptionCategory InputOutputOptions;

struct RegisterInputOptions {
    RegisterInputOptions();
};

struct RegisterOutputOptions {
    RegisterOutputOptions();
};

// The name of the program, for messages.
const char * programName();

// An open input.
struct Input {
    std::string name;           // as shown in messages
    int fd = -1;
    bool isStdIn = false;
    bool isRegularFile = false;
    bool mappable = false;      // a regular file, read from its beginning
    bool useMMap = false;       // --mmap, and mappable
    long long start = 0;        // where reading starts (the standard input may
                                // have been read in part)
    bool spooled = false;       // a copy of the standard input (openSeekableInput)
};

// Opens the input file name ("" and "-" are the standard input).  On failure,
// reports it (tool: file: reason) and returns false.
bool openInput(const std::string & name, Input & in);
// As openInput, for a tool that reads its input more than once (from
// in.start, e.g., with rewindInput or pread): an input that is not a regular
// file (a pipe, a terminal) is first copied to a temporary file (removed
// when closed).
bool openSeekableInput(const std::string & name, Input & in);
// The input, positioned at its start again (an input of openSeekableInput);
// -1 (reported) on failure.
int rewindInput(const Input & in);
void closeInput(Input & in);

// Opens the output: the standard output, or the file of --output, written
// under a temporary name and renamed into place by finishOutput (a file that
// is not regular, such as /dev/null, is written in place).  On failure,
// reports it and returns false.  Without --output (or a tool not registering
// it), the standard output.
bool openOutput();
// Whether --output was given (for a tool whose output is optional).
bool outputRequested();
// The output file descriptor (opened by openOutput; exits on failure if it
// was not called).
int outputFD();
// Whether the output is a terminal.
bool outputIsTerminal();
// For results written by C++ code: buffered, so flushed (by finishOutput, or
// before a pipeline writes to outputFD() itself).
llvm::raw_ostream & out();
// Completes the output: flushes out(), reports any write error, and renames
// an output file into place.  Returns false on error, when the output file is
// removed.  Without it (e.g., on exit after an error), the output file is not
// created and an existing one is left as it was.
bool finishOutput();

// Colors: never, always, or (auto) when fd is a terminal and neither NO_COLOR
// is set nor TERM=dumb.
enum class ColorMode { Never, Auto, Always };
bool useColor(ColorMode mode, int fd);

}

// Writes the n bytes at buf to fd, as FDSinkKernel does: repeating short and
// interrupted writes; the first error is reported by finishOutput.
extern "C" void toolio_write(int fd, const void * buf, size_t n);

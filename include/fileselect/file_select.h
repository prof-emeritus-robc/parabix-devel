/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  This file defines commmand-line options and parameters for 
 *  file selection as used by various command line tools.
 *
 */
#pragma once
 
#include <string>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <boost/filesystem.hpp>
class CPUDriver;

namespace re { class RE; }

namespace argv {

// The file selection options, for ParseCommandLineOptions to show.
extern llvm::cl::OptionCategory File_Select_Options;

extern bool NoMessagesFlag;  // -s
    
enum DevDirAction {Read, Skip, Recurse};
extern DevDirAction DevicesFlag;
extern DevDirAction DirectoriesFlag;

// Use DirectoriesFlag==Recurse to test for recursive mode.
extern bool RecursiveFlag; 
extern bool DereferenceRecursiveFlag; // -R

extern bool MmapFlag; // -mmap

extern bool UseStdIn;
    
// The files to process.  Standard input ("-") sets UseStdIn; with
// stdinAsPath it is also selected as the path "-", as it is when there are
// no input files.
std::vector<boost::filesystem::path> getFullFileList(CPUDriver & driver, llvm::cl::list<std::string> & inputFiles,
                                                     bool stdinAsPath = false);

// Opens a selected file for reading ("-" is standard input), returning its
// file descriptor, or -1 after reporting the error (unless -s) when it cannot
// be opened or is a directory.
int openInputFile(const boost::filesystem::path & p);
}

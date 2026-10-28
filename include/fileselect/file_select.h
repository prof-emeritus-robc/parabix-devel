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

// The files selected for processing: those named by the input file
// arguments, and those found by recursing into directories with -r or -R,
// that the include and exclude options select.  Standard input is selected
// by "-" or by giving no input files; with stdinAsPath it is also selected
// in place, as the path "-".  Files that cannot be read are selected too,
// for openInputFile to report.
class FileSelection {
public:
    FileSelection(CPUDriver & driver, const llvm::cl::list<std::string> & inputFiles, bool stdinAsPath = false);

    const std::vector<boost::filesystem::path> & paths() const {return mPaths;}
    size_t size() const {return mPaths.size();}
    const boost::filesystem::path & operator[](size_t i) const {return mPaths[i];}

    // Whether standard input is selected.
    bool stdinSelected() const {return mStdIn;}

    // Whether to show file names (unless -h): as in GNU grep, when there
    // are several files, or files found by recursing into a directory.
    bool showFileNames() const;

    // Whether to show a total: when there are several files (including
    // those that cannot be read).
    bool showTotal() const {return mPaths.size() > 1;}

    // The name to show for a selected file: none for standard input
    // selected by giving no input files.
    std::string displayName(size_t i) const {return mDefaultStdIn ? std::string() : mPaths[i].string();}

private:
    std::vector<boost::filesystem::path> selectFiles(CPUDriver & driver, const llvm::cl::list<std::string> & inputFiles);

    const bool mStdinAsPath;
    bool mStdIn = false;
    bool mDefaultStdIn = false;
    bool mRecursedIntoDirectory = false;
    std::vector<boost::filesystem::path> mPaths;
};

// Opens a selected file for reading ("-" is standard input), returning its
// file descriptor, or -1 after reporting the error (unless -s) when it cannot
// be opened or is a directory.
int openInputFile(const boost::filesystem::path & p);
}

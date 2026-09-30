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

extern bool NoMessagesFlag;  // -s
    
enum DevDirAction {Read, Skip, Recurse};
extern DevDirAction DevicesFlag;
extern DevDirAction DirectoriesFlag;

// Use DirectoriesFlag==Recurse to test for recursive mode.
extern bool RecursiveFlag; 
extern bool DereferenceRecursiveFlag; // -R

extern bool MmapFlag; // -mmap

extern bool UseStdIn;
    
std::vector<boost::filesystem::path> getFullFileList(CPUDriver & driver, llvm::cl::list<std::string> & inputFiles);
}

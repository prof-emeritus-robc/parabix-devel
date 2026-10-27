/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <fileselect/file_select.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <boost/filesystem.hpp>
#include <chrono>
#include <grep/nested_grep_engine.h>
#include <grep/searchable_buffer.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/raw_ostream.h>
#include <re/adt/re_re.h>
#include <re/adt/re_alt.h>
#include <re/adt/re_seq.h>
#include <re/adt/re_rep.h>
#include <re/adt/re_start.h>
#include <re/adt/re_end.h>
#include <re/adt/re_cc.h>
#include <re/adt/re_utility.h>
#include <re/adt/re_assertion.h>
#include <re/printer/re_printer.h>
#include <re/parse/parser.h>
#include <re/parse/GLOB_parser.h>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>

using namespace llvm;
using error_code = boost::system::error_code;

namespace argv {

static cl::OptionCategory Input_Options("File Selection Options", "These options control the input sources.");

bool NoMessagesFlag;
static cl::opt<bool, true> NoMessagesOption("s", cl::location(NoMessagesFlag), cl::desc("Suppress messages for file errors."), cl::cat(Input_Options), cl::Grouping);
static cl::alias NoMessagesAlias("no-messages", cl::desc("Alias for -s"), cl::aliasopt(NoMessagesOption));

bool RecursiveFlag;
static cl::opt<bool, true> RecursiveOption("r", cl::location(RecursiveFlag), cl::desc("Recursively process files within directories, (but follow only top-level symlinks unless -R)."), cl::cat(Input_Options), cl::Grouping);
static cl::alias RecursiveAlias("recursive", cl::desc("Alias for -r"), cl::aliasopt(RecursiveOption));

bool DereferenceRecursiveFlag;
static cl::opt<bool, true> DereferenceRecursiveOption("R", cl::location(DereferenceRecursiveFlag), cl::desc("Recursively process files within directories, following symlinks at all levels."), cl::cat(Input_Options), cl::Grouping);
static cl::alias DereferenceRecursiveAlias("dereference-recursive", cl::desc("Alias for -R"), cl::aliasopt(DereferenceRecursiveOption));

bool MmapFlag;
static cl::opt<bool, true> MmapOption("mmap", cl::location(MmapFlag),  cl::init(false), cl::desc("Use mmap for file input."), cl::cat(Input_Options));

static cl::list<std::string> ExcludeFiles("exclude", cl::ZeroOrMore,
                                          cl::desc("Exclude files matching the given filename GLOB pattern."), cl::cat(Input_Options));

static cl::opt<std::string> ExcludeFromFlag("exclude-from", cl::desc("Exclude files matching filename GLOB patterns from the given file."), cl::cat(Input_Options));

static cl::list<std::string> ExcludeDirectories("exclude-dir", cl::desc("Exclude directories matching the given pattern."), cl::ZeroOrMore, cl::cat(Input_Options));

static cl::opt<std::string> ExcludePerDirectory("exclude-per-directory",
                                                cl::desc(".gitignore (default) or other file specifying files to exclude."),
                                                cl::init(".gitignore"), cl::cat(Input_Options));

static cl::list<std::string> IncludeDirectories("include-dir", cl::desc("Include directories matching the given pattern."), cl::ZeroOrMore, cl::cat(Input_Options));

static cl::list<std::string> IncludeFiles("include", cl::ZeroOrMore, cl::desc("Include only files matching the given filename GLOB pattern(s)."), cl::cat(Input_Options));

DevDirAction DevicesFlag;
static cl::opt<DevDirAction, true> DevicesOption("D", cl::desc("Processing mode for devices:"),
                                                 cl::values(clEnumValN(Read, "read", "Treat devices as files to be searched."),
                                                            clEnumValN(Skip, "skip", "Silently skip devices.")), cl::cat(Input_Options), cl::location(DevicesFlag), cl::init(Read));
static cl::alias DevicesAlias("devices", cl::desc("Alias for -D"), cl::aliasopt(DevicesOption));

DevDirAction DirectoriesFlag;
static cl::opt<DevDirAction, true> DirectoriesOption("d", cl::desc("Processing mode for directories:"),
                                                     cl::values(clEnumValN(Read, "read", "Print an error message for any listed directories."),
                                                                clEnumValN(Skip, "skip", "Silently skip directories."),
                                                                clEnumValN(Recurse, "recurse", "Recursive process directories, equivalent to -r.")), cl::cat(Input_Options), cl::location(DirectoriesFlag), cl::init(Read));
static cl::alias DirectoriesAlias("directories", cl::desc("Alias for -d"), cl::aliasopt(DirectoriesOption));

static cl::opt<bool> TraceFileSelect("TraceFileSelect", cl::desc("Trace file selection"), cl::cat(Input_Options));
static cl::opt<bool> TimeFileSelect("TimeFileSelect", cl::desc("Report the time required for file selection."), cl::cat(Input_Options));

static cl::opt<unsigned> GitREcoalescing("git-RE-coalescing", cl::desc("gitignore RE coalescing factor"), cl::init(10), cl::cat(Input_Options));



//
// Include and exclude options (--include, --exclude, --exclude-from,
// --include-dir, --exclude-dir) follow GNU grep: GLOB patterns, applied in
// the order given on the command line, so that the last option matching a
// name wins.  A name matching no option is selected unless the first such
// option (for files, or for directories) is an include.  For the files and
// directories named on the command line, a pattern matches any suffix of the
// name that begins after a "/" (or the whole name); for those found while
// recursing, it matches the base name.  (BSD grep instead matches the full
// path, and selects no unmatched files once an --include is given.)
//
struct SelectionOption {
    unsigned position;      // on the command line
    re::PatternKind kind;
    std::string glob;
};

// The options for files or for directories, in command line order.
static std::vector<SelectionOption> selectionOptions(bool directories) {
    std::vector<SelectionOption> options;
    auto add = [&](const cl::list<std::string> & list, re::PatternKind kind) {
        for (unsigned i = 0; i < list.size(); i++) {
            options.push_back(SelectionOption{list.getPosition(i), kind, list[i]});
        }
    };
    if (directories) {
        add(IncludeDirectories, re::PatternKind::Include);
        add(ExcludeDirectories, re::PatternKind::Exclude);
    } else {
        add(IncludeFiles, re::PatternKind::Include);
        add(ExcludeFiles, re::PatternKind::Exclude);
        if (ExcludeFromFlag.getNumOccurrences()) {
            std::ifstream globFile(ExcludeFromFlag.c_str());
            std::string r;
            if (globFile.is_open()) {
                while (std::getline(globFile, r)) {
                    options.push_back(SelectionOption{ExcludeFromFlag.getPosition(), re::PatternKind::Exclude, r});
                }
                globFile.close();
            }
        }
    }
    std::stable_sort(options.begin(), options.end(),
                     [](const SelectionOption & a, const SelectionOption & b) {return a.position < b.position;});
    return options;
}

// The pattern of a GLOB for files or directories (whose candidate names end
// with "/"): matching a name suffix after a "/" for command line names, or
// the base name for names found while recursing (never, for a GLOB with "/").
static re::RE * selectionPattern(const std::string & glob, bool directory, bool commandLine) {
    if (!commandLine && (glob.find('/') != std::string::npos)) return nullptr;
    re::RE * globRE = re::RE_Parser::parse(directory ? glob + "/" : glob, re::DEFAULT_MODE, re::RE_Syntax::GrepGLOB);
    return re::makeSeq({re::makeAlt({re::makeStart(), re::makeCC('/')}), globRE, re::makeEnd()});
}

bool UseStdIn;

re::PatternVector getIncludeExcludePatterns(bool commandLine) {
    using Pattern = re::PatternKind;
    re::PatternVector signedPatterns;
    const std::vector<SelectionOption> fileOptions = selectionOptions(false);
    const std::vector<SelectionOption> dirOptions = selectionOptions(true);
    // The names selected initially: files (not ending in "/") unless the
    // first file option is an include, and directories (ending in "/")
    // unless the first directory option is an include.
    const bool allFiles = fileOptions.empty() || (fileOptions.front().kind == Pattern::Exclude);
    const bool allDirs = dirOptions.empty() || (dirOptions.front().kind == Pattern::Exclude);
    if (allFiles && allDirs) {
        signedPatterns.push_back(std::make_pair(Pattern::Include, re::makeEnd()));
    } else if (allDirs) {
        signedPatterns.push_back(std::make_pair(Pattern::Include, re::makeSeq({re::makeCC('/'), re::makeEnd()})));
    } else if (allFiles) {
        signedPatterns.push_back(std::make_pair(Pattern::Include,
                                                re::makeSeq({re::makeComplement(re::makeCC('/')), re::makeEnd()})));
    }
    // The options, each applied in turn (file and directory patterns match
    // different names).
    for (const auto & opts : {std::make_pair(&fileOptions, false), std::make_pair(&dirOptions, true)}) {
        for (const SelectionOption & o : *opts.first) {
            if (re::RE * pattern = selectionPattern(o.glob, opts.second, commandLine)) {
                signedPatterns.push_back(std::make_pair(o.kind, pattern));
            }
        }
    }
    if (signedPatterns.empty()) {
        // Nothing is selected.
        signedPatterns.push_back(std::make_pair(Pattern::Exclude, re::makeEnd()));
    }
    return signedPatterns;
}

namespace fs = boost::filesystem;
//
void selectPath(std::vector<fs::path> & collectedPaths, fs::path && p) {
    collectedPaths.emplace_back(std::move(p));
    if (TraceFileSelect) {
        llvm::errs() << "Selecting: " << p.string() << "\n";
    }
}

//
//  Directory List: a set of directory paths that have been
//  examined to identify candidate files for searching, together
//  with a count of the number of candidate files in each directory.
//
//  FileName Buffer: an ordered sequence of NUL terminated filenames
//  for each candidate produced in the directory traversal.
//  The first mFullPathEntries entries are CWD paths.  Subsequent entries
//  are base file names relative to a directory.   The set
//  of all entries for a given directory are consecutive in the
//  buffer, and the sets are ordered consecutively by directory
//  index in the Directory List.
//
//  CollectedPaths: a vector of file paths to which the
//  selected files are added.

class FileSelectAccumulator : public grep::MatchAccumulator {
public:
    FileSelectAccumulator(std::vector<fs::path> & collectedPaths) :
        mCollectedPaths(collectedPaths),
        mFullPathEntries(0)
    {}
    void setFullPathEntries(unsigned entries) {mFullPathEntries = entries; mDirectoryIndex = 0;}
    void reset();
    void addDirectory(fs::path dirPath, unsigned cumulativeEntryCount);
    void accumulate_match(const size_t lineNum, char * line_start, char * line_end) override;
protected:
    std::vector<fs::path> & mCollectedPaths;
    unsigned mFullPathEntries;
    unsigned mDirectoryIndex;
    std::vector<fs::path> mDirectoryList;
    std::vector<unsigned> mCumulativeEntryCount;
};

void FileSelectAccumulator::reset() {
    mCollectedPaths.clear();
    mFullPathEntries = 0;
    mDirectoryIndex = 0;
    mDirectoryList.clear();
    mCumulativeEntryCount.clear();
}

void FileSelectAccumulator::addDirectory(fs::path dirPath, unsigned cumulativeEntryCount) {
    mDirectoryList.push_back(dirPath);
    mCumulativeEntryCount.push_back(cumulativeEntryCount);
}

void FileSelectAccumulator::accumulate_match(const size_t fileIdx, char * name_start, char * name_end) {
    assert(name_end > name_start);
    assert((name_end - name_start) <= 4096);
    fs::path p(std::string(name_start, name_end - name_start));
    if (fileIdx < mFullPathEntries) {
        selectPath(mCollectedPaths, std::move(p));
   } else {
        assert(mDirectoryIndex < mDirectoryList.size());
        while (fileIdx >= mCumulativeEntryCount[mDirectoryIndex]) {
            mDirectoryIndex++;
        }
        selectPath(mCollectedPaths, mDirectoryList[mDirectoryIndex]/p);
    }
}

re::PatternVector coalesceREs(const re::PatternVector sourceREs, unsigned groupingFactor) {
    re::PatternVector coalesced;
    unsigned i = 0;
    while (i < sourceREs.size()) {
        std::vector<re::RE *> current = {sourceREs[i].second};
        const auto currentKind = sourceREs[i].first;
        unsigned j = 1;
        while ((j < groupingFactor) && (i + j < sourceREs.size()) && (sourceREs[i + j].first == currentKind)) {
            current.push_back(sourceREs[i + j].second);
            j++;
        }
        coalesced.push_back(std::make_pair(currentKind, re::makeAlt(current.begin(), current.end())));
        i = i + j;
    }
    return coalesced;
}

void recursiveFileSelect(CPUDriver & driver,
                         const fs::path & dirpath,
                         grep::NestedInternalSearchEngine & pathSelectEngine,
                         std::vector<fs::path> & collectedPaths) {

    // First update the search REs with any local .gitignore or other exclude file.
    error_code ec;
    const auto hasLocalIgnoreFile = !ExcludePerDirectory.empty() && fs::exists(dirpath/ExcludePerDirectory, ec) && !ec;

    if (hasLocalIgnoreFile) {
        pathSelectEngine.push(coalesceREs(re::parseGitIgnoreFile(dirpath, ExcludePerDirectory), GitREcoalescing));
    }
    // Gather files and subdirectories.
    // TODO: verify whether these are discarded before recursion
    grep::SearchableBuffer fileCandidates;
    grep::SearchableBuffer subdirCandidates;

    error_code errc;
    fs::directory_iterator di(dirpath, errc);
    if (errc) {
        // If we cannot enter the directory, keep it in the list of files,
        // for possible error reporting.
        if (!NoMessagesFlag) {
            collectedPaths.push_back(dirpath);
        }
        return;
    }
    const auto di_end = fs::directory_iterator();
    while (di != di_end) {
        const auto & e = di->path();
        error_code errc;
        const auto s = fs::status(e, errc);
        if (errc) {
            // If there was an error, we leave the file in the fileCandidates
            // list for later error processing.
            if (!NoMessagesFlag) {
                fileCandidates.append(e.string());
            }
        } else if (fs::is_directory(s)) {
            if (fs::is_symlink(e) && !DereferenceRecursiveFlag) {
                di.increment(errc);
                continue;
            }
            subdirCandidates.append(e.string() + "/");
        } else if (fs::is_regular_file(s) || DevicesFlag == Read) {
            fileCandidates.append(e.string());
        }
        error_code errc2;
        di.increment(errc2);
        if (errc2) break;
    }

    // For each directory, update counts for candidates generated at this level.
    //
    const auto fileCount = fileCandidates.getCandidateCount();
    if (fileCount > 0) {
        FileSelectAccumulator fileAccum(collectedPaths);
        fileAccum.setFullPathEntries(fileCount);
        pathSelectEngine.doGrep(fileCandidates.data(), fileCandidates.size(), fileAccum);
    }

    const auto subdirCount = subdirCandidates.getCandidateCount();
    if (subdirCount > 0) {
        std::vector<fs::path> selectedDirectories;
        FileSelectAccumulator directoryAccum(selectedDirectories);
        directoryAccum.setFullPathEntries(subdirCount);
        pathSelectEngine.doGrep(subdirCandidates.data(), subdirCandidates.size(), directoryAccum);
        for (const auto & subdir : selectedDirectories) {
            recursiveFileSelect(driver, subdir, pathSelectEngine, collectedPaths);
        }
    }

    if (hasLocalIgnoreFile) {
        pathSelectEngine.pop();
    }
}

std::vector<fs::path> getFullFileList(CPUDriver & driver, cl::list<std::string> & inputFiles) {
    auto file_select_start = std::chrono::high_resolution_clock::now();
    // The vector to accumulate the full list of collected files to be searched.
    std::vector<fs::path> collectedPaths;

    // In this pass through command line arguments and the file hierarchy,
    // we are just gathering file and subdirectory entries, so we silently
    // ignore errors.  We use the boost::filesystem operations that set
    // error codes rather than raise exceptions.

    // In non-recursive greps with no include/exclude processing, we simply assemble the
    // paths.
    if ((DirectoriesFlag != Recurse) && (ExcludeFiles.empty()) && (IncludeFiles.empty()) && (ExcludeFromFlag.empty())) {
        for (const auto & f : inputFiles) {
            if (f == "-") {  // stdin, will always be searched.
                argv::UseStdIn = true;
                continue;
            }
            fs::path p(f);
            error_code errc;
            fs::file_status s = fs::status(p, errc);
            if (errc) {
                // If there was an error, we leave the file in the fileCandidates
                // list for later error processing.
                if (!NoMessagesFlag) {
                    selectPath(collectedPaths, std::move(p));
                }
            } else if (fs::is_directory(s)) {
                if (DirectoriesFlag == Read) {
                    selectPath(collectedPaths, std::move(p));
                }
            } else if (fs::is_regular_file(s) || DevicesFlag == Read) {
                // Regular files, Devices and unknown file types
                selectPath(collectedPaths, std::move(p));
            }
        }
        return collectedPaths;
    }

    // Otherwise we may need to filter paths according to some include/exclude rules.
    grep::SearchableBuffer dirCandidates;
    grep::SearchableBuffer fileCandidates;

    // First level of processing:  command line files and directories.
    for (const auto & f : inputFiles) {
        if (f == "-") {  // stdin, will always be searched.
            argv::UseStdIn = true;
            continue;
        }
        fs::path p(f);
        error_code errc;
        fs::file_status s = fs::status(p, errc);
        if (errc) {
            // If there was an error, we leave the file in the fileCandidates
            // list for later error processing.
            if (!NoMessagesFlag) fileCandidates.append(p.string());
        } else if (fs::is_directory(s)) {
            if (DirectoriesFlag == Recurse) {
                dirCandidates.append(p.string() + "/");
            } else if (DirectoriesFlag == Read) {
                fileCandidates.append(p.string());
            }
        } else if (fs::is_regular_file(s) || DevicesFlag == Read) {
            // Regular files, Devices and unknown file types
            fileCandidates.append(p.string());
        }
    }

    //
    // Apply the file selection REs to choose files for processing, adding
    // them to the global list of selected files.

    grep::NestedInternalSearchEngine pathSelectEngine(driver);
    pathSelectEngine.setRecordBreak(grep::GrepRecordBreakKind::Null);
    // The command line names are selected first, then the names found while
    // recursing, with their own patterns (see getIncludeExcludePatterns).
    // These levels are filters: .gitignore patterns (pushed for each
    // directory with such a file) cannot override them.
    pathSelectEngine.push(coalesceREs(getIncludeExcludePatterns(true), GitREcoalescing), true);

    const auto commandLineFileCandidates = fileCandidates.getCandidateCount();
    if (commandLineFileCandidates > 0) {
        FileSelectAccumulator fileAccum(collectedPaths);
        fileAccum.setFullPathEntries(commandLineFileCandidates);
        pathSelectEngine.doGrep(fileCandidates.data(), fileCandidates.size(), fileAccum);
    }

    const auto commandLineDirCandidates = dirCandidates.getCandidateCount();
    if (commandLineDirCandidates > 0) {
        // Recursive processing of directories has been requested and we have
        // candidate directories from the command line.

        // selectedDirectories will accumulate hold the results of directory
        // include/exclude filtering at each level of processing.
        std::vector<fs::path> selectedDirectories;
        FileSelectAccumulator directoryAccum(selectedDirectories);

        // The initial grep search determines which of the command line directories to process.
        // Each of these candidates is a full path return from command line argument processing.
        directoryAccum.setFullPathEntries(commandLineDirCandidates);
        pathSelectEngine.doGrep(dirCandidates.data(), dirCandidates.size(), directoryAccum);
        pathSelectEngine.pop();
        pathSelectEngine.push(coalesceREs(getIncludeExcludePatterns(false), GitREcoalescing), true);
        // Select files from subdirectories using the recursive process.
        for (const auto & dirpath : selectedDirectories) {
            recursiveFileSelect(driver, dirpath, pathSelectEngine, collectedPaths);
        }
    }
    if (TimeFileSelect) {
        auto file_select_done = std::chrono::high_resolution_clock::now();
        auto file_select_time = file_select_done - file_select_start;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(file_select_time).count();
        llvm::errs() << "File select time: "<< duration << " msec.\n";
    }
    return collectedPaths;
}

}

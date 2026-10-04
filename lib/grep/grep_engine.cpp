/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <grep/grep_engine.h>

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <sched.h>
#include <boost/filesystem.hpp>
#include <toolchain/toolchain.h>
#include <toolchain/fileutil.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Casting.h>
#include <re/unicode/regex_passes.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/bitwise/bixlogic.h>
#include <kernel/core/idisa_target.h>
#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/core/callback.h>
#include <kernel/re/regexp_engine.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/boundary_kernels.h>
#include <kernel/unicode/utf8_support.h>
#include <re/unicode/resolve_properties.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/util/linebreak_kernel.h>
#include <kernel/streamutils/streams_merge.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/scan/scanmatchgen.h>
#include <kernel/streamutils/until_n.h>
#include <kernel/streamutils/sentinel.h>
#include <kernel/streamutils/run_index.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <pablo/pablo.h>
#include <re/adt/adt.h>
#include <re/adt/re_utility.h>
#include <re/adt/re_empty_set.h>
#include <re/printer/re_printer.h>
#include <re/alphabet/alphabet.h>
#include <re/analysis/re_analysis.h>
#include <re/analysis/re_name_gather.h>
#include <re/analysis/capture-ref.h>
#include <re/analysis/collect_ccs.h>
#include <re/cc/cc_kernel.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/remove_nullable.h>
#include <re/transforms/re_multiplex.h>
#include <re/transforms/expand_permutes.h>
#include <re/transforms/name_intro.h>
#include <re/transforms/name_lookaheads.h>
#include <re/transforms/reference_transform.h>
#include <re/transforms/variable_alt_promotion.h>
#include <re/unicode/casing.h>
#include <re/unicode/boundaries.h>
#include <re/unicode/re_name_resolve.h>
#include <ucd/data/PropertyObjectTable.h>
#include <sys/stat.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <grep/grep_toolchain.h>
#include <toolchain/toolchain.h>
#include <sys/mman.h>
#include <util/aligned_allocator.h>

using namespace llvm;
using namespace cc;
using namespace kernel;

namespace grep {

using UntilNMode = UntilNkernel::Mode;

static cl::opt<UntilNMode>
MaxLimitTerminationMode("maxlimit-termination-mode",
                  cl::init(UntilNMode::ZeroAfterN),
                  cl::desc("method of pipeline termination when -m=maxlimit is reached."),
                  cl::values(clEnumValN(UntilNMode::ReportAcceptedLengthAtAndBeforeN, "report", "halt pipeline after maxlimit using truncated streamset"),
                             clEnumValN(UntilNMode::TerminateAtN, "terminate", "halt pipeline after maxlimit using streamset copy"),
                             clEnumValN(UntilNMode::ZeroAfterN, "zero", "fully process the file")));


static cl::opt<bool> UseLayers("UseLayers", cl::desc("Use pipeline layers"), cl::init(false));

const auto ENCODING_BITS = 8;

void GrepCallBackObject::handle_signal(unsigned s) {
    if (static_cast<GrepSignal>(s) == GrepSignal::BinaryFile) {
        mBinaryFile = true;
    } else {
        llvm::report_fatal_error("Unknown GrepSignal");
    }
}

extern "C" void accumulate_match_wrapper(MatchAccumulator * accum_addr, const size_t lineNum, char * line_start, char * line_end) {
    assert ("passed a null accumulator" && accum_addr);
    accum_addr->accumulate_match(lineNum, line_start, line_end);
}

extern "C" void finalize_match_wrapper(MatchAccumulator * accum_addr, char * buffer_end) {
    assert ("passed a null accumulator" && accum_addr);
    accum_addr->finalize_match(buffer_end);
}

extern "C" size_t get_file_count_wrapper(MatchAccumulator * accum_addr) {
    assert ("passed a null accumulator" && accum_addr);
    return accum_addr->getFileCount();
}

extern "C" size_t get_file_start_pos_wrapper(MatchAccumulator *accum_addr, size_t fileNo) {
    assert ("passed a null accumulator" && accum_addr);
    return accum_addr->getFileStartPos(fileNo);
}

extern "C" void set_batch_line_number_wrapper(MatchAccumulator *accum_addr, size_t fileNo, size_t batchLine) {
    assert ("passed a null accumulator" && accum_addr);
    accum_addr->setBatchLineNumber(fileNo, batchLine);
}

// Grep Engine construction and initialization.

GrepEngine::GrepEngine(BaseDriver &driver) :
    mSuppressFileMessages(false),
    mBinaryFilesMode(argv::Text),
    mPreferMMap(false),
    mColoring(false),
    mShowFileNames(false),
    mStdinLabel("(stdin)"),
    mShowLineNumbers(false),
    mBeforeContext(0),
    mAfterContext(0),
    mInitialTab(false),
    mInvertMatches(false),
    mMaxCount(0),
    mGrepStdIn(false),
    mNullMode(NullCharMode::Data),
    mGrepDriver(driver),
    mMainMethod(nullptr),
    mBatchSize(FileBatchSegments * codegen::SegmentSize),
    mBatchMethod(nullptr),
    mNextFileToGrep(0),
    mNextFileToPrint(0),
    grepMatchFound(false),
    mGrepRecordBreak(GrepRecordBreakKind::LF),
    mSource(nullptr),
    mMatchStarts(nullptr),
    mLineBreakStream(nullptr),
    mU8index(nullptr),
    mEngineThread(std::this_thread::get_id()) {

    }

GrepEngine::~GrepEngine() { }

QuietModeEngine::QuietModeEngine(BaseDriver &driver) : GrepEngine(driver) {
    mEngineKind = EngineKind::QuietMode;
    mMaxCount = 1;
    mColoring = false;
}

MatchOnlyEngine::MatchOnlyEngine(BaseDriver & driver, bool showFilesWithMatch, bool useNullSeparators) :
    GrepEngine(driver), mRequiredCount(showFilesWithMatch) {
    mEngineKind = EngineKind::MatchOnly;
    mFileSuffix = useNullSeparators ? std::string("\0", 1) : "\n";
    mMaxCount = 1;
    mShowFileNames = true;
    mColoring = false;
}

CountOnlyEngine::CountOnlyEngine(BaseDriver &driver, bool countall) : GrepEngine(driver) {
    mEngineKind = countall ? EngineKind::CountAll : EngineKind::CountOnly;
    mFileSuffix = ":";
    mColoring = false;
}

EmitMatchesEngine::EmitMatchesEngine(BaseDriver &driver)
: GrepEngine(driver) {
    mEngineKind = EngineKind::EmitMatches;
    mFileSuffix = mInitialTab ? "\t:" : ":";
    if (mInvertMatches) {
        mColoring = false;
    }
}

void GrepEngine::setRecordBreak(GrepRecordBreakKind b) {
    mGrepRecordBreak = b;
}

namespace fs = boost::filesystem;

std::vector<std::vector<std::string>> formFileGroups(std::vector<fs::path> paths, size_t max_batch_size) {
    const unsigned maxFilesPerGroup = 32;
    std::vector<std::vector<std::string>> groups;
    // The total size of files in the current group, or 0 if the
    // the next file should start a new group.
    uintmax_t groupTotalSize = 0;
    for (auto p : paths) {
        boost::system::error_code errc;
        auto s = fs::file_size(p, errc);
        if ((s > 0) && (s + 1 + groupTotalSize < max_batch_size)) {
            s += 1;  // +1 for an extra "\n"
            if (groupTotalSize == 0) {
                groups.push_back({p.string()});
                groupTotalSize = s;
            } else {
                groups.back().push_back(p.string());
                groupTotalSize += s;
                if (groups.back().size() == maxFilesPerGroup) {
                    // Signal to start a new group
                    groupTotalSize = 0;
                }
            }
        } else {
            // For large files, or in the case of non-regular file or other error,
            // the path is saved in its own group.
            groups.push_back({p.string()});
            // This group is done, signal to start a new group.
            groupTotalSize = 0;
        }
    }
    return groups;
}

void GrepEngine::initFileResult(const std::vector<boost::filesystem::path> & paths) {
    const unsigned n = paths.size();
    mResultStrs.resize(n);
    mFileStatus.resize(n, FileStatus::Pending);
    mInputPaths = paths;
    mFileGroups = formFileGroups(paths, mBatchSize);
    const unsigned numOfThreads = std::min(static_cast<unsigned>(codegen::TaskThreads),
                                           std::max(static_cast<unsigned>(mFileGroups.size()), 1u));
    codegen::setTaskThreads(numOfThreads);
}

//
// Moving matches to EOL.   Mathches need to be aligned at EOL if for
// scanning or counting processes (with a max count != 1).   If the REs
// are not all anchored, then we need to move the matches to EOL.
bool GrepEngine::matchesToEOLrequired () {
    if (mEngineKind == EngineKind::CountAll) return false;
    // Moving matches is required for UnicodeLines mode, because matches
    // may be on the CR of a CRLF.
    if (mGrepRecordBreak == GrepRecordBreakKind::Unicode) return true;
    // If all REs are anchored to EOL already, then we can avoid moving them.
    if (hasEndAnchor(mRE) && (grepOffset(mRE) > 0)) return false;
    //
    // Not all REs are anchored.   We can avoid moving matches, if we are
    // in MatchOnly mode (or CountOnly with MaxCount = 1) and no invert match inversion.
    return (mEngineKind == EngineKind::EmitMatches) || (mMaxCount != 1) || mInvertMatches;
}

void GrepEngine::initRE(re::RE * re) {
    // Ensure that all modes and Unicode properties are resolved, and
    // the RE is fully simplified before proceeding with RE analysis.
    mRE = prepareInputRE(re, grep::lineNumGrep);

    // Mode determination (byte / UTF8-indexed / full Unicode) is the regex
    // engine's job; grep_engine just supplies the reasons specific to it.
    // Unicode record-break mode always forces full Unicode indexing, just as
    // before. Coloring doesn't force full indexing outright (a fixed-UTF8 RE
    // still gets plain byte mode); it only rules out the cheaper UTF8-indexed
    // optimization once we're past that -- the same role UnicodeBasisMode
    // plays -- so it's folded in there rather than given its own concept in
    // the engine's API.
    RE_ModeOptions opts;
    opts.forceUnicodeIndexing = (mGrepRecordBreak == GrepRecordBreakKind::Unicode);
    opts.unicodeIndexingOverride = UnicodeIndexing;
    opts.unicodeBasisOverride = UnicodeBasisMode || mColoring;
    opts.byteCClimit = ByteCClimit;
    mMode = determineREMode(mRE, opts);

    // Don't attempt colorization with a zero-width RE.
    if (getLengthRange(mRE, mMode.lengthAlphabet).second == 0) {
        mColoring = false;
    }
}

void GrepEngine::grepPrologue(kernel::PipelineBuilder & P, StreamSet * ByteStream) {
    StreamSet * Source = ByteStream;
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        P.captureByteData("Source", ByteStream);
    }

    mLineBreakStream = nullptr;
    mU8index = nullptr;

    Scalar * const callbackObject = P.getInputScalar("callbackObject");
    if (mBinaryFilesMode == argv::Text) {
        mNullMode = NullCharMode::Data;
    } else if (mBinaryFilesMode == argv::WithoutMatch) {
        mNullMode = NullCharMode::Abort;
    } else {
        mNullMode = NullCharMode::Break;
    }
    mLineBreakStream = P.CreateStreamSet(1, 1);
    if (mGrepRecordBreak == GrepRecordBreakKind::Unicode) {
        // Unicode line-break detection needs bit-parallel basis streams; the
        // regex engine doesn't get a say here since this is purely about
        // finding line breaks, not about how the RE itself will be compiled.
        StreamSet * BasisBits = P.CreateStreamSet(ENCODING_BITS, 1);
        Selected_S2P(P, ByteStream, BasisBits);
        Source = BasisBits;
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBixNum("basis", BasisBits);
        }
        mU8index = P.CreateStreamSet(1, 1);
        UnicodeLinesLogic(P, Source, mLineBreakStream, mU8index, UnterminatedLineAtEOF::Add1, mNullMode, callbackObject);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("mU8index", mU8index);
        }
    }
    else {
        if (mGrepRecordBreak == GrepRecordBreakKind::LF) {
            Kernel * k = P.CreateKernelCall<UnixLinesKernelBuilder>(Source, mLineBreakStream, UnterminatedLineAtEOF::Add1, mNullMode, callbackObject);
            if (mNullMode == NullCharMode::Abort) {
                P.LinkFunction(k, "signal_dispatcher", signal_dispatcher);
            }
        } else { // if (mGrepRecordBreak == GrepRecordBreakKind::Null) {
            P.CreateKernelCall<NullDelimiterKernel>(Source, mLineBreakStream, UnterminatedLineAtEOF::Add1);
        }
    }
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        P.captureBitstream("mLineBreakStream", mLineBreakStream);
    }

    mSource = Source;
    mMatchStarts = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<LineStartsKernel>(mLineBreakStream, mMatchStarts);
}

StreamSet * GrepEngine::initialMatches(RE_PipelineBuilder & RE_PB, StreamSet * InputStream) {
    kernel::PipelineBuilder & P = RE_PB.getPipelineBuilder();
    StreamSet * Matches = P.CreateStreamSet();
    RE_PB.matchSearchPipeline(mRE, Matches);
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        P.captureBitstream("initial matches", Matches);
    }
    return Matches;
}

StreamSet * GrepEngine::matchedLines(kernel::PipelineBuilder & P, StreamSet * initialMatches, StreamSet * lineBreaks) {
    StreamSet * MatchedLineEnds = nullptr;
    if (matchesToEOLrequired() || mColoring) {
        StreamSet * const MovedMatches = P.CreateStreamSet();
        P.CreateKernelCall<MatchedLinesKernel>(initialMatches, lineBreaks, MovedMatches);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("MovedMatches", MovedMatches);
        }
        MatchedLineEnds = MovedMatches;
    } else {
        MatchedLineEnds = initialMatches;
    }
    if (mInvertMatches) {
        StreamSet * const InvertedMatches = P.CreateStreamSet();
        P.CreateKernelCall<InvertMatchesKernel>(MatchedLineEnds, lineBreaks, InvertedMatches);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("InvertedMatches", InvertedMatches);
        }
        MatchedLineEnds = InvertedMatches;
    }
    if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
        P.captureBitstream("MatchedLineEnds", MatchedLineEnds);
    }
    return MatchedLineEnds;
}

StreamSet * GrepEngine::applyMatchLimit(kernel::PipelineBuilder & P, StreamSet * MatchedLineEnds) {
    if (mMaxCount > 0) {
        StreamSet * MaxCountLines = nullptr;
        Scalar * const maxCount = P.getInputScalar("maxCount");
        if (MaxLimitTerminationMode == UntilNMode::ReportAcceptedLengthAtAndBeforeN) {
            MaxCountLines = P.CreateTruncatedStreamSet(MatchedLineEnds);
        } else {
            MaxCountLines = P.CreateStreamSet();
        }
        P.CreateKernelCall<UntilNkernel>(maxCount, MatchedLineEnds, MaxCountLines, MaxLimitTerminationMode);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("MaxCountLines", MaxCountLines);
        }
        MatchedLineEnds = MaxCountLines;
        if (MaxLimitTerminationMode != UntilNMode::ZeroAfterN) {
            StreamSet * TruncatedLines = streamutils::Merge(P, {{MaxCountLines, {0}}, {mLineBreakStream, {0}}});
            if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
                P.captureBitstream("TruncatedLines", TruncatedLines);
            }
            mLineBreakStream = TruncatedLines;
        }
    }
    return MatchedLineEnds;

}

StreamSet * GrepEngine::grepPipeline(kernel::PipelineBuilder & P, StreamSet * InputStream) {
    grepPrologue(P, InputStream);
    RE_PipelineBuilder RE_PB(P, RE_context{&cc::UTF8, mSource, mMatchStarts, mLineBreakStream});
    RE_ModeOptions modeOpts;
    modeOpts.byteCClimit = ByteCClimit;
    RE_PB.setModeOptions(modeOpts);
    RE_PB.setMode(mMode);
    if (mU8index) {
        RE_PB.setU8IndexHint(mU8index);
    } else {
        RE_PB.setLineBreakStream(mLineBreakStream);
    }
    StreamSet * Matches = initialMatches(RE_PB, InputStream);
    mU8index = RE_PB.getU8Index();
    StreamSet * lbs = RE_PB.getMatchFollows();
    StreamSet * matches = matchedLines(P, Matches, lbs);
    return applyMatchLimit(P, matches);
}


// The QuietMode, MatchOnly and CountOnly engines share a common code generation main function,
// which returns a count of the matches found (possibly subject to a MaxCount).
//

void GrepEngine::grepCodeGen() {

    auto P = CreatePipeline(mGrepDriver,
                            Input<uint32_t>{"useMMap"}, Input<uint32_t>{"fileDescriptor"},
                            Input<GrepCallBackObject &>{"callbackObject"}, Input<size_t>{"maxCount"},
                            Output<uint64_t>{"countResult"});

    Scalar * const useMMap = P.getInputScalar("useMMap");
    Scalar * const fileDescriptor = P.getInputScalar("fileDescriptor");
    StreamSet * const ByteStream = P.CreateStreamSet(1, ENCODING_BITS);
    P.CreateKernelCall<FDSourceKernel>(useMMap, fileDescriptor, ByteStream);
    StreamSet * const Matches = grepPipeline(P, ByteStream);
    P.CreateKernelCall<PopcountKernel>(Matches, P.getOutputScalar("countResult"));
    mMainMethod = P.compile();
}

//
//  Default Report Match:  lines are emitted with whatever line terminators are found in the
//  input.  However, if the final line is not terminated, a new line is appended.
//
void EmitMatch::setFileLabel(std::string fileLabel) {
    if (mShowFileNames) {
        mLinePrefix = fileLabel + (mInitialTab ? "\t:" : ":");
    } else mLinePrefix = "";
}

void EmitMatch::setStringStream(std::ostringstream * s) {
    mResultStr = s;
}

size_t EmitMatch::getFileCount() {
    mCurrentFile = 0;
    if (mFileNames.size() == 0) return 1;
    return mFileNames.size();
}

size_t EmitMatch::getFileStartPos(size_t fileNo) {
    if (mFileStartPositions.size() == 0) return 0;
    assert(fileNo < mFileStartPositions.size());
    //llvm::errs() << "getFileStartPos(" << fileNo << ") = ";
    //llvm::errs().write_hex(mFileStartPositions[fileNo]);
    //llvm::errs() << "  file = " << mFileNames[fileNo] << "\n";
    return mFileStartPositions[fileNo];
}

void EmitMatch::setBatchLineNumber(size_t fileNo, size_t batchLine) {
    //llvm::errs() << "setBatchLineNumber(" << fileNo << ", " << batchLine << ")  file = " << mFileNames[fileNo] << "\n";
    mFileStartLineNumbers[fileNo+1] = batchLine;
    terminateRecord();
}

// Whether the matched record [line_start, line_end] includes its own terminator.
bool EmitMatch::isTerminated(const char * line_start, const char * line_end) const {
    const unsigned last_byte = static_cast<unsigned char>(*line_end);
    switch (mRecordBreak) {
        case GrepRecordBreakKind::Null:
            return last_byte == 0;
        case GrepRecordBreakKind::LF:
            return last_byte == 0x0A;
        case GrepRecordBreakKind::Unicode:
            break;
    }
    if ((last_byte >= 0x0A) && (last_byte <= 0x0D)) {
        return true;
    }
    const auto bytes = line_end - line_start + 1;
    if (last_byte == 0x85) {  //  Possible NEL terminator.
        return (bytes >= 2) && (static_cast<unsigned char>(line_end[-1]) == 0xC2);
    }
    // Possible LS or PS terminators.
    return (bytes >= 3) && (static_cast<unsigned char>(line_end[-2]) == 0xE2)
                        && (static_cast<unsigned char>(line_end[-1]) == 0x80)
                        && ((last_byte == 0xA8) || (last_byte == 0xA9));
}

// Emit the record terminator after a final record that lacked one.
void EmitMatch::terminateRecord() {
    if (!mTerminated) {
        if (mRecordBreak == GrepRecordBreakKind::Null) {
            mResultStr->put('\0');
        } else {
            *mResultStr << "\n";
        }
    }
    mTerminated = true;
}

void EmitMatch::accumulate_match (const size_t lineNum, char * line_start, char * line_end) {
    //llvm::errs() << "lineNum = " << lineNum << "\n";
    while ((mCurrentFile + 1 < mFileStartPositions.size()) && (mFileStartLineNumbers[mCurrentFile + 1] <= lineNum)) {
        mCurrentFile++;
        //llvm::errs() << "mCurrentFile = " << mCurrentFile << "\n";
        setFileLabel(mFileNames[mCurrentFile]);
    }
    size_t relLineNum = mCurrentFile > 0 ? lineNum - mFileStartLineNumbers[mCurrentFile] : lineNum;
    if (mContextGroups && (lineNum > mLineNum + 1) && (relLineNum > 0)) {
        *mResultStr << "--\n";
    }
    *mResultStr << mLinePrefix;
    if (mShowLineNumbers) {
        // Internally line numbers are counted from 0.  For display, adjust
        // the line number so that lines are numbered from 1.
        if (mInitialTab) {
            *mResultStr << relLineNum+1 << "\t:";
        }
        else {
            *mResultStr << relLineNum+1 << ":";
        }
    }

    const auto bytes = line_end - line_start + 1;
    mResultStr->write(line_start, bytes);
    mLineCount++;
    mLineNum = lineNum;
    mTerminated = isTerminated(line_start, line_end);
}

void EmitMatch::finalize_match(char * buffer_end) {
    terminateRecord();
}

void GrepEngine::applyColorization(PipelineBuilder & P,
                                   StreamSet * SourceCoords,
                                   StreamSet * MatchSpans,
                                   StreamSet * Basis) {

    if (UsePhaseForColourization) {
        P.InsertPhaseBoundary();
    }

    Scalar * const callbackObject = P.getInputScalar("callbackObject");

    auto makeNestedColourizationPipeline = [&](PipelineBuilder & E) {

        std::string ESC = "\x1B";
        std::vector<std::string> colorEscapes = {ESC + "[01;31m" + ESC + "[K", ESC + "[m"};
        unsigned insertLengthBits = 4;
        std::vector<unsigned> insertAmts;
        for (auto & s : colorEscapes) {insertAmts.push_back(s.size());}

        StreamSet * const SpanMarks = E.CreateStreamSet(2, 1);
        E.CreateKernelCall<SpansToMarksKernel>(MatchSpans, SpanMarks);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            E.captureBixNum("SpanMarks", SpanMarks);
        }

        StreamSet * const InsertBixNum = E.CreateStreamSet(insertLengthBits, 1);
        E.CreateKernelCall<ZeroInsertBixNum>(insertAmts, SpanMarks, InsertBixNum);
        StreamSet * const SpreadMask = E.CreateStreamSet(1, 1);
        InsertionSpreadMask(E, InsertBixNum, SpreadMask, kernel::InsertPosition::Before);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            E.captureBitstream("SpreadMask", SpreadMask);
        }

        // For each run of 0s marking insert positions, create a parallel
        // bixnum sequentially numbering the string insert positions.
        StreamSet * const InsertIndex = E.CreateStreamSet(insertLengthBits);
        E.CreateKernelCall<RunIndex>(SpreadMask, InsertIndex, nullptr, RunIndex::Kind::RunOf0);
        // Basis bit streams expanded with 0 bits for each string to be inserted.
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            E.captureBixNum("InsertIndex", InsertIndex);
        }

        StreamSet * ExpandedBasis = E.CreateStreamSet(8);
        SpreadByMask(E, SpreadMask, Basis, ExpandedBasis);

        // Map the match start/end marks to their positions in the expanded basis.
        StreamSet * ExpandedMarks = E.CreateStreamSet(2);
        SpreadByMask(E, SpreadMask, SpanMarks, ExpandedMarks);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            E.captureBixNum("ExpandedMarks", ExpandedMarks);
        }

        StreamSet * ColorizedBasis = E.CreateStreamSet(8);
        E.CreateKernelCall<StringReplaceKernel>(colorEscapes, ExpandedBasis, SpreadMask, ExpandedMarks, InsertIndex, ColorizedBasis, -1);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            E.captureBixNum("ColorizedBasis", ColorizedBasis);
        }
        StreamSet * const ColorizedBytes  = E.CreateStreamSet(1, 8);
        E.CreateKernelCall<P2SKernel>(ColorizedBasis, ColorizedBytes);

        StreamSet * ColorizedBreaks = E.CreateStreamSet(1);
        E.CreateKernelCall<UnixLinesKernelBuilder>(ColorizedBasis, ColorizedBreaks, UnterminatedLineAtEOF::Add1);

        StreamSet * const ColorizedCoords = E.CreateStreamSet(3, sizeof(size_t) * 8);
        E.CreateKernelCall<MatchCoordinatesKernel>(ColorizedBreaks, ColorizedBreaks, ColorizedCoords, 1);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            E.captureBitstream("ColorizedBreaks", ColorizedBreaks);
        }

        // TODO: source coords >= colorized coords until the final stride?
        // E.AssertEqualLength(SourceCoords, ColorizedCoords);

        Kernel * const matchK = E.CreateKernelCall<ColorizedReporter>(ColorizedBytes, SourceCoords, ColorizedCoords, callbackObject);
        P.LinkFunction(matchK, "accumulate_match_wrapper", accumulate_match_wrapper);
        P.LinkFunction(matchK, "finalize_match_wrapper", finalize_match_wrapper);
    };


    if (UseNestedColourizationPipeline) {

        auto E = CreatePipeline(mGrepDriver,
                                Input<streamset_t>("SourceCoords", SourceCoords, GreedyRate(1), Deferred()),
                                Input<streamset_t>("MatchSpans", MatchSpans, GreedyRate(), Deferred()),
                                Input<streamset_t>("Basis", Basis, GreedyRate(), Deferred()),
                                Input<grep::GrepCallBackObject &>("callbackObject", callbackObject),
                                InternallySynchronized(),
                                MustExplicitlyTerminate(),
                                SideEffecting()
                                );

        makeNestedColourizationPipeline(E);

        P.AddKernelCall(E.makeKernel());

    } else {

        makeNestedColourizationPipeline(P);

    }

}

void EmitMatchesEngine::grepPipeline(kernel::PipelineBuilder & P, StreamSet * ByteStream) {
    grepPrologue(P, ByteStream);
    RE_PipelineBuilder RE_PB(P, RE_context{&cc::UTF8, mSource, mMatchStarts, mLineBreakStream});
    RE_ModeOptions modeOpts;
    modeOpts.byteCClimit = ByteCClimit;
    RE_PB.setModeOptions(modeOpts);
    RE_PB.setMode(mMode);
    if (mU8index) {
        RE_PB.setU8IndexHint(mU8index);
    } else {
        RE_PB.setLineBreakStream(mLineBreakStream);
    }

    StreamSet * Matches = P.CreateStreamSet(1);
    StreamSet * MatchSpans = nullptr;
    if (mColoring) {
        MatchSpans = P.CreateStreamSet(1);
        RE_PB.matchSpanPipeline(mRE, Matches, MatchSpans);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("MatchSpans", MatchSpans);
            P.captureBitstream("Matches", Matches);
        }
    } else {
        RE_PB.matchSearchPipeline(mRE, Matches);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("Matches", Matches);
        }
    }
    mU8index = RE_PB.getU8Index();
    const bool usesUnicodeIndexing = RE_PB.usesUnicodeIndexing();

    StreamSet * lbs = RE_PB.getMatchFollows();
    StreamSet * MatchedLineEnds = matchedLines(P, Matches, lbs);

    bool hasContext = (mAfterContext != 0) || (mBeforeContext != 0);
    StreamSet * MatchesByLine = nullptr;
    if (mColoring | hasContext) {
        if (UseLayers) {
            P.InsertPhaseBoundary();
        }
        MatchesByLine = P.CreateStreamSet(1, 1);
        FilterByMask(P, lbs, MatchedLineEnds, MatchesByLine);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("MatchesByLine", MatchesByLine);
        }
    }

    if (hasContext) {
        StreamSet * ContextByLine = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<ContextSpan>(MatchesByLine, ContextByLine, mBeforeContext, mAfterContext);
        StreamSet * SelectedLines = P.CreateStreamSet(1, 1);
        // Note that the following spread will produce SelectedLines in u8 space.
        SpreadByMask(P, mLineBreakStream, ContextByLine, SelectedLines);
        MatchedLineEnds = SelectedLines;
        MatchesByLine = ContextByLine;
    } else if (usesUnicodeIndexing) {
        StreamSet * u8index1 = mU8index;
        if (grepOffset(mRE) > 0) {
            u8index1 = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<AddSentinel>(mU8index, u8index1);
        }
        StreamSet * Results = P.CreateStreamSet(1, 1);
        SpreadByMask(P, u8index1, MatchedLineEnds, Results);
        MatchedLineEnds = Results;
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("u8 matches", MatchedLineEnds);
        }
    }

    MatchedLineEnds = applyMatchLimit(P, MatchedLineEnds);

    if (mColoring && usesUnicodeIndexing) {
        StreamSet * spreadSpans = P.CreateStreamSet(1, 1);
        SpreadByMask(P, mU8index, MatchSpans, spreadSpans);
        StreamSet * ResultSpans = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<U8Spans>(spreadSpans, mU8index, ResultSpans);
        MatchSpans = ResultSpans;
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("u8 spreadSpans", ResultSpans);
        }
    }

    if (mColoring) {
        StreamSet * SourceCoords = P.CreateStreamSet(1, sizeof(size_t) * 8);
        Scalar * const callbackObject = P.getInputScalar("callbackObject");
        Kernel * const batchK = P.CreateKernelCall<BatchCoordinatesKernel>(MatchedLineEnds, mLineBreakStream, SourceCoords, callbackObject);
        P.LinkFunction(batchK, "get_file_count_wrapper", get_file_count_wrapper);
        P.LinkFunction(batchK, "get_file_start_pos_wrapper", get_file_start_pos_wrapper);
        P.LinkFunction(batchK, "set_batch_line_number_wrapper", set_batch_line_number_wrapper);

        StreamSet * MatchedLineStarts = P.CreateStreamSet(1, 1);
        StreamSet * lineStarts = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<LineStartsKernel>(mLineBreakStream, lineStarts);
        SpreadByMask(P, lineStarts, MatchesByLine, MatchedLineStarts);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("MatchedLineStarts", MatchedLineStarts);
        }
        StreamSet * MatchedLineSpans = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<LineSpansKernel>(MatchedLineStarts, MatchedLineEnds, MatchedLineSpans);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("MatchedLineSpans", MatchedLineSpans);
        }

        StreamSet * Filtered = P.CreateStreamSet(1, 8);
        if (UseByteFilterByMask) {
            // Warning: a phantom null byte may be produced in the event
            // of input files with no final line break.
            FilterByMask(P, MatchedLineSpans, ByteStream, Filtered, 0, 64);
        } else {
            P.CreateKernelCall<MatchFilterKernel>(MatchedLineStarts, mLineBreakStream, ByteStream, Filtered);
        }
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureByteData("Filtered", Filtered);
        }

        StreamSet * FilteredMatchSpans = P.CreateStreamSet(1, 1);
        FilterByMask(P, MatchedLineSpans, MatchSpans, FilteredMatchSpans);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBitstream("FilteredMatchSpans", FilteredMatchSpans);
        }
        StreamSet * FilteredBasis = P.CreateStreamSet(8, 1);
        Selected_S2P(P, Filtered, FilteredBasis);
        if (LLVM_UNLIKELY(codegen::EnableIllustrator)) {
            P.captureBixNum("FilteredBasis", FilteredBasis);
        }

        applyColorization(P, SourceCoords, FilteredMatchSpans, FilteredBasis);

    } else { // Non colorized output
        if (MatchCoordinateBlocks > 0) {
            StreamSet * MatchCoords = P.CreateStreamSet(3, sizeof(size_t) * 8);
            P.CreateKernelCall<MatchCoordinatesKernel>(MatchedLineEnds, mLineBreakStream, MatchCoords, MatchCoordinateBlocks);
            Scalar * const callbackObject = P.getInputScalar("callbackObject");
            Kernel * const matchK = P.CreateKernelCall<MatchReporter>(ByteStream, MatchCoords, callbackObject);
            P.LinkFunction(matchK, "accumulate_match_wrapper", accumulate_match_wrapper);
            P.LinkFunction(matchK, "finalize_match_wrapper", finalize_match_wrapper);
        } else {
            Scalar * const callbackObject = P.getInputScalar("callbackObject");
            Kernel * const scanBatchK = P.CreateKernelCall<ScanBatchKernel>(MatchedLineEnds, mLineBreakStream, ByteStream, callbackObject, ScanMatchBlocks);
            P.LinkFunction(scanBatchK, "get_file_count_wrapper", get_file_count_wrapper);
            P.LinkFunction(scanBatchK, "get_file_start_pos_wrapper", get_file_start_pos_wrapper);
            P.LinkFunction(scanBatchK, "set_batch_line_number_wrapper", set_batch_line_number_wrapper);
            P.LinkFunction(scanBatchK, "accumulate_match_wrapper", accumulate_match_wrapper);
            P.LinkFunction(scanBatchK, "finalize_match_wrapper", finalize_match_wrapper);
        }
    }
}


void EmitMatchesEngine::grepCodeGen() {

    auto P = CreatePipeline(mGrepDriver,
                            Input<const char*>{"buffer"}, Input<size_t>{"length"},
                            Input<EmitMatch &>{"callbackObject"}, Input<size_t>{"maxCount"},
                            Output<uint64_t>{"countResult"});

    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");
    StreamSet * const InternalBytes = P.CreateStreamSet(1, 8);

    P.CreateKernelCall<MemorySourceKernel>(buffer, length, InternalBytes);
    grepPipeline(P, InternalBytes);
    P.setOutputScalar("countResult", P.CreateConstant(P.getInt64(0)));
    mBatchMethod = P.compile();
}

uint64_t GrepEngine::doGrep(const std::vector<std::string> & fileNames, std::ostringstream & strm) {
    auto f = mMainMethod;
    uint64_t resultTotal = 0;

    for (auto fileName : fileNames) {
        GrepCallBackObject handler;
        bool useMMap = mPreferMMap && canMMap(fileName) && (fileNames.size() == 1);
        int32_t fileDescriptor = openFile(fileName, strm);
        if (fileDescriptor == -1) return 0;
        uint64_t grepResult = f(useMMap, fileDescriptor, handler, mMaxCount);
        close(fileDescriptor);
        if (handler.binaryFileSignalled()) {
            llvm::errs() << "Binary file " << fileName << "\n";
        }
        else {
            showResult(grepResult, fileName, strm);
            resultTotal += grepResult;
        }
    }
    return resultTotal;
}

std::string GrepEngine::linePrefix(std::string fileName) {
    if (!mShowFileNames) return "";
    if (fileName == "-") {
        return mStdinLabel + mFileSuffix;
    }
    else {
        return fileName + mFileSuffix;
    }
}

// Default: do not show anything
void GrepEngine::showResult(uint64_t grepResult, const std::string & fileName, std::ostringstream & strm) {

}

void CountOnlyEngine::showResult(uint64_t grepResult, const std::string & fileName, std::ostringstream & strm) {
    if (mShowFileNames) strm << linePrefix(fileName);
    strm << grepResult << "\n";
}

void MatchOnlyEngine::showResult(uint64_t grepResult, const std::string & fileName, std::ostringstream & strm) {
    if (grepResult == mRequiredCount) {
       strm << linePrefix(fileName);
    }
}

constexpr size_t batch_alignment = 64;

// A NUL byte marks a binary file, except with NUL record breaks (-z), where it is
// the record terminator.
bool EmitMatchesEngine::detectsBinaryByNull() const {
    return ((mBinaryFilesMode == argv::WithoutMatch) || (mBinaryFilesMode == argv::Binary))
        && (mGrepRecordBreak != GrepRecordBreakKind::Null);
}

uint64_t EmitMatchesEngine::doGrep(const std::vector<std::string> & fileNames, std::ostringstream & strm) {
    auto f = mBatchMethod;
    EmitMatch accum(mShowFileNames, mShowLineNumbers, ((mBeforeContext > 0) || (mAfterContext > 0)), mInitialTab, mGrepRecordBreak);
    accum.setStringStream(&strm);
    AlignedAllocator<char, batch_alignment> alloc;
    if (fileNames.size() == 1) {
        int32_t fd = openFile(fileNames[0], strm);
        if (fd == -1) return 0;   // File error; skip.
        struct stat st;
        if (fstat(fd, &st) != 0)  return 0;
        if (st.st_size == 0) return 0;
        accum.mFileNames.push_back(fileNames[0]);
        accum.mFileStartPositions.push_back(static_cast<size_t>(0));
        accum.setFileLabel(accum.mFileNames[0]);
        accum.mFileStartLineNumbers.push_back(~static_cast<size_t>(0));
        // Only try mmap for file groups consisting of a single file.
        bool useMMap = mPreferMMap && canMMap(fileNames[0]);
        AlignedFileBuffer buf;
        buf.load(fileNames[0], useMMap);
        size_t bytes_read = buf.getBufSize();
        if (bytes_read <= 0) return 0;
        accum.mBatchBuffer = buf.getBuf();
        bool skip_binary_file = false;
        if (detectsBinaryByNull()) {
            auto null_byte_ptr = memchr(accum.mBatchBuffer, char (0), bytes_read);
            if (null_byte_ptr != nullptr) { // Binary file;
                skip_binary_file = true;
                if (mBinaryFilesMode != argv::WithoutMatch) {
                    strm << "Binary file: " << fileNames[0] << " skipped.\n";
                }
            }
        }
        if (!skip_binary_file) {
            f(accum.mBatchBuffer, bytes_read, accum, mMaxCount);
        }
        buf.release();
        if (accum.mLineCount > 0) grepMatchFound = true;
        return accum.mLineCount;
    }
    std::vector<size_t> fileSize(fileNames.size(), 0);
    size_t cumulativeSize = 0;
    unsigned filesExamined = 0;
    accum.mFileNames.reserve(fileNames.size());
    accum.mFileStartPositions.reserve(fileNames.size());
    accum.mBatchBuffer = alloc.allocate(mBatchSize, 0);
    size_t current_start_position = 0;
    if (accum.mBatchBuffer == nullptr) {
        llvm::report_fatal_error(llvm::StringRef("Unable to allocate batch buffer of size: ") + std::to_string(mBatchSize));
    }
    char * current_base = accum.mBatchBuffer;
    for (unsigned i = 0; i < fileNames.size(); i++) {
        int32_t fd = openFile(fileNames[i], strm);
        filesExamined++;
        if (fd == -1) continue;  // File error; skip.
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            continue;
        }
        fileSize[i] = st.st_size;
        if (cumulativeSize + fileSize[i] > mBatchSize) {
            close(fd);
            //llvm::errs() << "Exceeding mBatchSize: " << cumulativeSize << " + " << fileSize[i] << "\n";
            break;
        }
        ssize_t bytes_read = read(fd, current_base, fileSize[i]);
        close(fd);
        if (bytes_read <= 0) continue; // No data or error reading the file; skip.
        if (detectsBinaryByNull()) {
            auto null_byte_ptr = memchr(current_base, char (0), bytes_read);
            if (null_byte_ptr != nullptr) { // Binary file;
                // Silently skip in the WithoutMatch mode
                if (mBinaryFilesMode != argv::WithoutMatch) {
                    strm << "Binary file: " << fileNames[i] << " skipped.\n";
                }
                continue;
            }
        }
        accum.mFileNames.push_back(fileNames[i]);
        accum.mFileStartPositions.push_back(current_start_position);
        current_base += bytes_read;
        current_start_position += bytes_read;
        // Terminate each file's final record so that records never span files.
        const char terminator = (mGrepRecordBreak == GrepRecordBreakKind::Null) ? '\0' : '\n';
        if (*(current_base - 1) != terminator) {
            *current_base = terminator;
            current_base++;
            current_start_position++;
        }
        cumulativeSize = current_start_position;
    }
    if (accum.mFileNames.size() > 0) {
        accum.setFileLabel(accum.mFileNames[0]);
        accum.mFileStartLineNumbers.resize(accum.mFileNames.size());
        // Initialize to the maximum integer value so that tests
        // will not rule that we are past a given file until the
        // actual limit is computed.
        for (unsigned i = 0; i < accum.mFileStartLineNumbers.size(); i++) {
            accum.mFileStartLineNumbers[i] = ~static_cast<size_t>(0);
        }
        f(accum.mBatchBuffer, cumulativeSize, accum, mMaxCount);
    }
    alloc.deallocate(accum.mBatchBuffer, 0);
    if (filesExamined < fileNames.size()) {
        //llvm::errs() << "filesExamined " << filesExamined << "\n";
        std::vector<std::string> remainingFileNames(fileNames.size() - filesExamined);
        for (unsigned i = 0; i < remainingFileNames.size(); i++) {
            remainingFileNames[i] = fileNames[i + filesExamined];
            //llvm::errs() << "remainingFileNames[i]: " << remainingFileNames[i] << "\n";
        }
        accum.mLineCount += doGrep(remainingFileNames, strm);
    }
    if (accum.mLineCount > 0) grepMatchFound = true;
    return accum.mLineCount;
}

// Open a file and return its file desciptor.
int32_t GrepEngine::openFile(const std::string & fileName, std::ostringstream & msgstrm) {
    if (fileName == "-") {
        return STDIN_FILENO;
    }
    else {
        struct stat sb;
        int flags = O_RDONLY;
        #ifdef __linux__
        if (NoOSFileCaching) {
            flags |= O_DIRECT;
        }
        #endif
        int32_t fileDescriptor = open(fileName.c_str(), flags);
        if (LLVM_UNLIKELY(fileDescriptor == -1)) {
            if (!mSuppressFileMessages) {
                msgstrm << "icgrep: \"" << fileName << "\": ";
                if (errno == EACCES) {
                    msgstrm << "Permission denied.\n";
                }
                else if (errno == ENOENT) {
                    msgstrm << "No such file.\n";
                }
                else {
                    msgstrm << "Failed; errno = " << errno << ".\n";
                }
            }
            return fileDescriptor;
        }
        if (stat(fileName.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
            if (!mSuppressFileMessages) {
                msgstrm << "icgrep: " << fileName << ": Is a directory.\n";
            }
            close(fileDescriptor);
            return -1;
        }
        #ifdef __APPLE__
        if (NoOSFileCaching) {
            fcntl(fileDescriptor, F_NOCACHE, 1);
            fcntl(fileDescriptor, F_RDAHEAD, 0);
        }
        #endif
        if (TraceFiles) {
            llvm::errs() << "Opened " << fileName << ".\n";
        }
        return fileDescriptor;
    }
}

// The process of searching a group of files may use a sequential or a task
// parallel approach.

bool GrepEngine::searchAllFiles() {

    std::vector<std::thread> threads;
    threads.reserve(codegen::TaskThreads - 1);

    for(unsigned long i = 1; i < codegen::TaskThreads; ++i) {
        threads.emplace_back([this]() { this->DoGrepThreadMethod(); });
    }
    // Main thread also does the work;
    DoGrepThreadMethod();
    for (auto & t : threads) {
        t.join();
    }
    return grepMatchFound;
}

// DoGrep thread function.
void GrepEngine::DoGrepThreadMethod() {

    unsigned fileIdx = mNextFileToGrep++;
    while (fileIdx < mFileGroups.size()) {

        const auto grepResult = doGrep(mFileGroups[fileIdx], mResultStrs[fileIdx]);
        mFileStatus[fileIdx] = FileStatus::GrepComplete;
        if (grepResult > 0) {
            grepMatchFound = true;
        }
        if ((mEngineKind == EngineKind::QuietMode) && grepMatchFound) {
            return;
        }
        fileIdx = mNextFileToGrep++;
        if (std::this_thread::get_id() == mEngineThread) {
            while ((mNextFileToPrint < mFileGroups.size()) && (mFileStatus[mNextFileToPrint] == FileStatus::GrepComplete)) {
                const auto output = mResultStrs[mNextFileToPrint].str();
                if (!output.empty()) {
                    llvm::outs() << output;
                }
                mFileStatus[mNextFileToPrint] = FileStatus::PrintComplete;
                mNextFileToPrint++;
            }
        }
    }
    if (std::this_thread::get_id() != mEngineThread) {
        return;
    }
    while (mNextFileToPrint < mFileGroups.size()) {
        const bool readyToPrint = (mFileStatus[mNextFileToPrint] == FileStatus::GrepComplete);
        if (readyToPrint) {
            const auto output = mResultStrs[mNextFileToPrint].str();
            if (!output.empty()) {
                llvm::outs() << output;
            }
            mFileStatus[mNextFileToPrint] = FileStatus::PrintComplete;
            mNextFileToPrint++;
        } else {
            sched_yield();
        }
    }
    if (mGrepStdIn) {
        std::ostringstream s;
        const auto grepResult = doGrep({"-"}, s);
        llvm::outs() << s.str();
        if (grepResult) grepMatchFound = true;
    }
}

void matchingRecords(PipelineBuilder & P, re::RE * re, StreamSet * basis, StreamSet * u8index,
                     StreamSet * breaks, StreamSet * matchStarts, StreamSet * records) {
    // Link and resolve properties and boundaries as for the main engine; a search
    // of property value names may itself contain property value patterns.
    re = prepareInputRE(re, lineNumGrep);
    RE_PipelineBuilder RE_PB(P, RE_context{&cc::UTF8, basis, matchStarts, breaks});
    RE_PB.setU8IndexHint(u8index);
    StreamSet * const matches = P.CreateStreamSet();
    RE_PB.matchSearchPipeline(re, matches);
    // A match marks the end of the matched text; move each to its record break
    // (in the index space of the mode).
    if (!RE_PB.usesUnicodeIndexing()) {
        P.CreateKernelCall<MatchedLinesKernel>(matches, breaks, records);
        return;
    }
    StreamSet * const matchedRecords = P.CreateStreamSet();
    P.CreateKernelCall<MatchedLinesKernel>(matches, RE_PB.getMatchFollows(), matchedRecords);
    StreamSet * index = u8index;
    if (grepOffset(re) > 0) {
        index = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<AddSentinel>(u8index, index);
    }
    SpreadByMask(P, index, matchedRecords, records);
}

InternalSearchEngine::InternalSearchEngine(BaseDriver &driver) :
mGrepRecordBreak(GrepRecordBreakKind::LF),
mGrepDriver(driver),
mMainMethod(nullptr) {
}

void InternalSearchEngine::grepCodeGen(re::RE * matchingRE) {

    re::CC * breakCC = nullptr;
    if (mGrepRecordBreak == GrepRecordBreakKind::Null) {
        breakCC = re::makeCC(0x0, &cc::UTF8);
    } else {// if (mGrepRecordBreak == GrepRecordBreakKind::LF)
        breakCC = re::makeCC(0x0A, &cc::UTF8);
    }

    auto E = CreatePipeline(mGrepDriver,
                            Input<const char*>{"buffer"}, Input<size_t>{"length"},
                            Input<MatchAccumulator *>{"accumulator"});

    Scalar * const buffer = E.getInputScalar(0);
    Scalar * const length = E.getInputScalar(1);
    Scalar * const callbackObject = E.getInputScalar(2);
    StreamSet * ByteStream = E.CreateStreamSet(1, 8);
    E.CreateKernelCall<MemorySourceKernel>(buffer, length, ByteStream);

    StreamSet * RecordBreakStream = E.CreateStreamSet();
    StreamSet * BasisBits = E.CreateStreamSet(8);
    E.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    E.CreateKernelCall<CharacterClassKernelBuilder>(std::vector<re::CC *>{breakCC}, BasisBits, RecordBreakStream);
    StreamSet * matchStarts = E.CreateStreamSet(1, 1);
    E.CreateKernelCall<LineStartsKernel>(RecordBreakStream, matchStarts);

    StreamSet * u8index = E.CreateStreamSet();
    E.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    StreamSet * MatchingRecords = E.CreateStreamSet();
    matchingRecords(E, matchingRE, BasisBits, u8index, RecordBreakStream, matchStarts, MatchingRecords);

    if (MatchCoordinateBlocks > 0) {
        StreamSet * MatchCoords = E.CreateStreamSet(3, sizeof(size_t) * 8);
        E.CreateKernelCall<MatchCoordinatesKernel>(MatchingRecords, RecordBreakStream, MatchCoords, MatchCoordinateBlocks);
        Kernel * const matchK = E.CreateKernelCall<MatchReporter>(ByteStream, MatchCoords, callbackObject);
        E.LinkFunction(matchK, "accumulate_match_wrapper", accumulate_match_wrapper);
        E.LinkFunction(matchK, "finalize_match_wrapper", finalize_match_wrapper);
    } else {
        Kernel * const scanMatchK = E.CreateKernelCall<ScanMatchKernel>(MatchingRecords, RecordBreakStream, ByteStream, callbackObject, ScanMatchBlocks);
        E.LinkFunction(scanMatchK, "accumulate_match_wrapper", accumulate_match_wrapper);
        E.LinkFunction(scanMatchK, "finalize_match_wrapper", finalize_match_wrapper);
    }

    mMainMethod = E.compile();
}

InternalSearchEngine::InternalSearchEngine(const std::unique_ptr<grep::GrepEngine> & engine)
    : InternalSearchEngine(engine->mGrepDriver) {}

InternalSearchEngine::~InternalSearchEngine() { }


void InternalSearchEngine::doGrep(const char * search_buffer, size_t bufferLength, MatchAccumulator & accum) {
    assert ((((uintptr_t)search_buffer) % (512 / 8)) == 0);
    mMainMethod(search_buffer, bufferLength, &accum);
}

class LineNumberAccumulator : public grep::MatchAccumulator {
public:
    LineNumberAccumulator() {}
    void accumulate_match(const size_t lineNum, char * line_start, char * line_end) override;
    std::vector<uint64_t> && getAccumulatedLines() { return std::move(mLineNums); }
private:
    std::vector<uint64_t> mLineNums;
};

void LineNumberAccumulator::accumulate_match(const size_t lineNum, char * /* line_start */, char * /* line_end */) {
    mLineNums.push_back(lineNum);
}

std::vector<uint64_t> lineNumGrep(re::RE * pattern, const char * buffer, size_t bufSize) {
    LineNumberAccumulator accum;
    CPUDriver driver("driver");
    grep::InternalSearchEngine engine(driver);
    engine.setRecordBreak(grep::GrepRecordBreakKind::LF);
    engine.grepCodeGen(pattern);
    assert ((((uintptr_t)buffer) % (512 / 8)) == 0);
    engine.doGrep(buffer, bufSize, accum);
    return accum.getAccumulatedLines();
}

class MatchOnlyAccumulator : public grep::MatchAccumulator {
public:
    MatchOnlyAccumulator() : mFoundMatch(false) {}
    void accumulate_match(const size_t lineNum, char * line_start, char * line_end) override;
    bool foundAnyMatches() { return mFoundMatch; }
private:
    bool mFoundMatch;
};

void MatchOnlyAccumulator::accumulate_match(const size_t lineNum, char * /* line_start */, char * /* line_end */) {
    mFoundMatch = true;
}

bool matchOnlyGrep(re::RE * pattern, const char * buffer, size_t bufSize) {
    MatchOnlyAccumulator accum;
    CPUDriver driver("driver");
    grep::InternalSearchEngine engine(driver);
    engine.setRecordBreak(grep::GrepRecordBreakKind::Null);
    engine.grepCodeGen(pattern);
    assert ((((uintptr_t)buffer) % (512 / 8)) == 0);
    engine.doGrep(buffer, bufSize, accum);
    return accum.foundAnyMatches();
}

}

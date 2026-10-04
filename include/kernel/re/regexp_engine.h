/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <kernel/core/streamset.h>
#include <map>
#include <re/transforms/name_intro.h>
#include <re/adt/adt.h>
#include <re/unicode/regex_passes.h>
#include <pablo/pablo.h>
namespace kernel { class StreamSet; }
namespace cc { class Alphabet; }
namespace kernel { class PipelineBuilder; }

using Alphabets = std::vector<std::pair<const cc::Alphabet *, kernel::StreamSet *>>;

enum ExternalStreamKind {ZeroWidth, FixedLength, StartIndexed, EndIndexed};

struct ExternalStream {
    ExternalStreamKind kind;
    unsigned offset;
    std::pair<int, int> lgthRange;
    kernel::StreamSet * extStream;
    // For a StartIndexed (lookahead) external: the stream marks where the
    // positive lookahead holds, and the consuming kernel negates it.
    bool negated = false;
};

enum class RE_CombiningType {None, Exclude, Include};

using ExternalNameMap = std::map<std::string, ExternalStream>;

//
// The simplified caller-facing description of the regex engine's input: a
// source stream, the alphabet it's expressed in, and the match regions
// within it (matchStarts/matchFollows are optional -- nullptr if the RE
// isn't anchored to any region). This one bundle is the entire "context"
// a client (grep_engine, json_support, csvgrep) hands to the engine.
//
//   - encoding == &cc::UTF8: source is UTF-8 code units, as either a raw
//     1-element byte stream or an already-transposed 8-element basis. The
//     engine decides for itself whether/how far to go (byte mode /
//     UTF8-indexed / full Unicode) and builds whatever derived streams that
//     requires, recalculating matchStarts/matchFollows into the new index
//     space if it ends up switching to full Unicode indexing.
//   - encoding == &cc::Unicode: source is already a 21-element Unicode
//     basis; the engine uses it, and matchStarts/matchFollows, as-is, no
//     analysis performed.
struct RE_context {
    const cc::Alphabet * encoding;
    kernel::StreamSet * source;
    kernel::StreamSet * matchStarts = nullptr;
    kernel::StreamSet * matchFollows = nullptr;
};

struct RE_ModeOptions {
    // Set by a caller for its own reasons (grep's Unicode record-break mode,
    // or -- until match-span support lands -- its coloring requirement)
    // without the engine needing to know why.
    bool forceUnicodeIndexing = false;
    bool unicodeIndexingOverride = false; // debug: always use full Unicode indexing
    bool unicodeBasisOverride = false;    // debug: disable the UTF8-indexed optimization
    unsigned byteCClimit = 6;
};

struct RE_Mode {
    const cc::Alphabet * lengthAlphabet;  // &cc::UTF8 or &cc::Unicode
    const cc::Alphabet * indexAlphabet;   // &cc::UTF8 or &cc::Unicode
};

// Pure analysis, no StreamSets touched.
RE_Mode determineREMode(re::RE * re, const RE_ModeOptions & opts);

// Common RE input preparation (mode resolution + name/property resolution,
// case folding, and general simplification), shared by every client of the
// regex engine -- whether it goes on to use the auto-determining RE_context
// path or builds an RE_CompilerContext by hand. grepCallback supports
// recursive regular expressions (a property value that is itself resolved by
// running a line-oriented grep over text); pass nullptr where that isn't
// needed.
re::RE * prepareInputRE(re::RE * re, re::GrepLinesFunctionType grepCallback = nullptr);

class RE_CompilerContext {
    friend class RE_Kernel;
    friend class RE_PipelineBuilder;
public:
    RE_CompilerContext();

    void setCodeUnitContext(const cc::Alphabet * a, kernel::StreamSet * s);

    void setMatchRegions(kernel::StreamSet * starts, kernel::StreamSet * follows);

    void setIndexingContext(const cc::Alphabet * a, kernel::StreamSet * s);

    void addAlphabet(const cc::Alphabet * a, kernel::StreamSet * basis);

    void addExternal(std::string extName, ExternalStream s);
    
    void setCombiningStream(kernel::StreamSet * combiningStream, RE_CombiningType k);

private:
    const cc::Alphabet * mCodeUnitAlphabet;
    kernel::StreamSet * mCodeUnitStream;
    kernel::StreamSet * mMatchStarts;
    kernel::StreamSet * mMatchFollows;
    const cc::Alphabet * mLengthAlphabet;
    kernel::StreamSet * mIndexStream;
    RE_CombiningType mCombiningType;
    kernel::StreamSet * mCombiningStream;
    Alphabets mAlphabets;
    ExternalNameMap mExternals;
    std::vector<std::string> mSpanNames;
};

class RE_Kernel : public pablo::PabloKernel {
public:
    RE_Kernel(LLVMTypeSystemInterface & ts,
              RE_CompilerContext & ctxt, re::RE * re, kernel::StreamSet * results);
    void generatePabloMethod() override;

    std::string makeSignature(RE_CompilerContext & ctxt, re::RE * r);
    kernel::Bindings makeInputBindings(RE_CompilerContext & ctxt, re::RE * r);
    kernel::Bindings makeOutputBindings(re::RE * r, kernel::StreamSet * results);

private:
    RE_CompilerContext mContext;
    re::RE * mRE;
};

class FixedDistanceMatchesKernel : public pablo::PabloKernel {
public:
    FixedDistanceMatchesKernel(LLVMTypeSystemInterface & ts, unsigned distance, unsigned length,
                               kernel::StreamSet * Basis, kernel::StreamSet * Matches, kernel::StreamSet * ToCheck  = nullptr);
protected:
    void generatePabloMethod() override;
private:
    unsigned mMatchDistance;
    unsigned mMatchLength;
    bool mHasCheckStream;
};

class CodePointMatchKernel : public pablo::PabloKernel {
public:
    CodePointMatchKernel(LLVMTypeSystemInterface & ts,
                         UCD::property_t prop, unsigned distance, unsigned length,
                         kernel::StreamSet * Basis, kernel::StreamSet * Matches);
protected:
    void generatePabloMethod() override;
private:
    unsigned mMatchDistance;
    unsigned mMatchLength;
    UCD::property_t mProperty;
};

class FixedMatchSpansKernel : public pablo::PabloKernel {
public:
    FixedMatchSpansKernel(LLVMTypeSystemInterface & ts, unsigned length, unsigned offset, kernel::StreamSet * MatchMarks, kernel::StreamSet * MatchSpans);
protected:
    void generatePabloMethod() override;
    unsigned mMatchLength;
    unsigned mOffset;
};

class LongestSpan : public pablo::PabloKernel {
public:
    LongestSpan(LLVMTypeSystemInterface & ts, unsigned pfxOffset, unsigned endOffset, 
                kernel::StreamSet * pfxStrm, kernel:: StreamSet * endBack, kernel::StreamSet * matchEnd,
                kernel::StreamSet * spans);
protected:
    void generatePabloMethod() override;
private:
    unsigned mPfxOffset;
    unsigned mEndOffset;
};

//
// Kernels for a lookahead (?=B{lb,}C) (see re::parseStarLookahead), with
// one stream position per character.  With B' the B positions other than
// record breaks, StarLookaheadIndex marks the positions not in B' together
// with the first position of each run of B'.  Shifting the start of each C
// match back to the preceding index position (IndexedShiftBack) reaches the
// start of the run of B' before it, if there is one.  StarLookaheadSpans then
// marks, from each such run start (and each C start), the positions through
// the C start, keeping those followed by at least lb characters of B': these
// are the first characters after the positions where the lookahead holds.
//
class StarLookaheadIndex : public pablo::PabloKernel {
public:
    StarLookaheadIndex(LLVMTypeSystemInterface & ts, kernel::StreamSet * B, kernel::StreamSet * breaks,
                       kernel::StreamSet * index);
protected:
    void generatePabloMethod() override;
private:
    const bool mHasBreaks;
};

class StarLookaheadSpans : public pablo::PabloKernel {
public:
    StarLookaheadSpans(LLVMTypeSystemInterface & ts, unsigned lb, kernel::StreamSet * B, kernel::StreamSet * breaks,
                       kernel::StreamSet * runStarts, kernel::StreamSet * Cstarts, kernel::StreamSet * spans);
protected:
    void generatePabloMethod() override;
private:
    const unsigned mLB;
    const bool mHasBreaks;
};

class RE_PipelineBuilder {
public:
    RE_PipelineBuilder(kernel::PipelineBuilder & P, RE_CompilerContext & ctxt) :
        mPB(P), mCtxt(ctxt), mMatchSpans(false),
        mHaveSourceContext(false), mPrepared(false), mHaveMode(false),
        mLineBreakHint(nullptr), mU8IndexHint(nullptr),
        mUsesUnicodeIndexing(false), mU8Index(nullptr),
        mFinalMatchStarts(nullptr), mFinalMatchFollows(nullptr) {}

    // Auto-determining entry point: the engine decides (and builds) whatever
    // representation it needs to compile a given RE against this source.
    RE_PipelineBuilder(kernel::PipelineBuilder & P, RE_context context);

    // Only meaningful for the RE_context constructor.
    void setModeOptions(const RE_ModeOptions & opts) {mModeOptions = opts;}
    void setMode(const RE_Mode & mode) {mMode = mode; mHaveMode = true;}
    void setLineBreakStream(kernel::StreamSet * lb) {mLineBreakHint = lb;}
    void setU8IndexHint(kernel::StreamSet * idx) {mU8IndexHint = idx;}

    bool usesUnicodeIndexing() const {return mUsesUnicodeIndexing;}
    kernel::StreamSet * getU8Index() const {return mU8Index;}
    kernel::StreamSet * getMatchStarts() const {return mFinalMatchStarts;}
    kernel::StreamSet * getMatchFollows() const {return mFinalMatchFollows;}

    void matchSearchPipeline(re::RE * re, kernel::StreamSet * results);
    void matchSpanPipeline(re::RE * re, kernel::StreamSet * matches, kernel::StreamSet * spans);
    kernel::PipelineBuilder & getPipelineBuilder() {return mPB;}

protected:
    // Internal methods.
    void addExternal(std::string extName, ExternalStream s);

    re::RE * prepareRE(re::RE * re);

    re::RE * spanFactoring(re::RE * re);

    re::RE * processReferences(re::RE * re);

    void prepareExternals(re::RE * re);

    void compileExternal(re::Name * name);

    void compileProperty(re::PropertyExpression * pe);

    void getSpan(re::RE * re, kernel::StreamSet * spans);

    // Mode determination + source preparation, run once before compiling.
    void ensurePrepared(re::RE *& re);

private:
    kernel::PipelineBuilder & mPB;
    RE_CompilerContext mCtxt;
    bool mMatchSpans;
    re::RE * mRE;
    re::UniquePrefixNamer mUPnamer;

    bool mHaveSourceContext;
    RE_context mSourceContext;
    RE_ModeOptions mModeOptions;
    bool mPrepared;
    bool mHaveMode;
    RE_Mode mMode;
    kernel::StreamSet * mLineBreakHint;
    kernel::StreamSet * mU8IndexHint;
    bool mUsesUnicodeIndexing;
    kernel::StreamSet * mU8Index;
    kernel::StreamSet * mFinalMatchStarts;
    kernel::StreamSet * mFinalMatchFollows;
};

//
// Create the kernels and pipeline necessary for any Unicode boundary or codepoint property.
// BasisBits may either be 8 basis streams for UTF-8 or 21 streams for Unicode.
// In the case of UTF-8, u8index is required as the IndexStream.
//
void UnicodePropertyLogic(kernel::PipelineBuilder & P, re::PropertyExpression * pe,
                          kernel::StreamSet * BasisBits, kernel::StreamSet * IndexStream,
                          kernel::StreamSet * PropertyStream);
void UnicodePropertyLogic(kernel::PipelineBuilder & P, re::PropertyExpression * pe,
                          kernel::StreamSet * BasisBits, kernel::StreamSet * PropertyStream);

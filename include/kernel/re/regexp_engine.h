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
namespace re { struct LookaheadSegment; }
namespace cc { class Alphabet; }
namespace kernel { class PipelineBuilder; }

using Alphabets = std::vector<std::pair<const cc::Alphabet *, kernel::StreamSet *>>;

// A StringClassRep external is the repeated string class of an unbounded
// repetition (see re::StringClassRepNamer); its stream set holds the Fill,
// Starts and Ends streams of the string class (see StringClassKernel).
enum ExternalStreamKind {ZeroWidth, FixedLength, StartIndexed, EndIndexed, StringClassRep};

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

//
// The first step of the regular expression engine: the RE as parsed is
// prepared (modes and external symbols resolved -- escapes, properties,
// including property values given by regular expressions, case folding,
// grapheme mode -- and the RE passes), and the mode for compiling it is
// determined (see RE_Mode), given the client's options.  Clients that need
// to know about the prepared RE before building a pipeline (for example, its
// length range) prepare it with prepareRE and pass the result to the
// pipeline methods; otherwise the pipeline methods prepare it themselves.
//
struct PreparedRE {
    re::RE * re = nullptr;
    RE_Mode mode{nullptr, nullptr};
    RE_ModeOptions options;
    // The range of match lengths, in the mode's length alphabet.
    std::pair<int, int> lengthRange() const;
    // Must every match end at the end anchor $?
    bool endAnchored() const;
    // The offset of the match positions after the last matched character
    // (see re::grepOffset).
    unsigned grepOffset() const;
};

PreparedRE prepareRE(re::RE * re, const RE_ModeOptions & opts = RE_ModeOptions{});

// The numbers (from 0) of the lines of a buffer that contain a match to an
// RE (as parsed).  This resolves property values given by regular
// expressions, as a search over the lines of the property value names.
std::vector<uint64_t> matchingLineNumbers(re::RE * pattern, const char * buffer, size_t bufSize);

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
// Kernels for a lookahead chain such as (?=B*C D{2,}E) (see
// re::parseLookaheadChain), with one stream position per character.  The
// chain is processed from the right, maintaining a stream H that marks the
// first character after each position where the rest of the chain holds.
//
// The end of the text gives the region follows (the positions following the
// ends of the match regions), as End is compiled.  A fixed segment F of length
// k (StarChainFixedStep) gives the starts of F matches followed immediately by
// a position of H.
//
// A star segment B{lb,} uses the other two kernels.  With B' the B positions
// other than region follows, StarLookaheadIndex marks the positions not in B'
// together with the first position of each run of B'.  Shifting each position
// of H back to the preceding index position (IndexedShiftBack) reaches the
// start of the run of B' before it, if there is one.  StarLookaheadSpans then
// marks, from each such run start (and each position of H), the positions
// through that position of H, keeping those followed by at least lb
// characters of B'.
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

class StarChainFixedStep : public pablo::PabloKernel {
public:
    // H may be nullptr, standing for all positions.
    StarChainFixedStep(LLVMTypeSystemInterface & ts, unsigned length, kernel::StreamSet * Fends,
                       kernel::StreamSet * H, kernel::StreamSet * result);
protected:
    void generatePabloMethod() override;
private:
    const unsigned mLength;
    const bool mHasH;
};

//
// A string class: an alternation of strings, each a sequence of character
// classes over the alphabet of the basis (e.g. 21-bit Unicode, or 8-bit UTF-8
// code units after toUTF8).  With L the length of the longest string, the
// basis is read with a lookahead of L - 1: each character of a string is
// tested on the basis looked ahead by its offset, giving the start positions
// of the occurrences of the string.  Fill marks every position within an
// occurrence (each start, shifted forward up to the length of its string
// less one); Starts marks the start positions and Ends the final positions.
// The output stream set holds Fill, Starts and Ends, in that order.
//
class StringClassKernel : public pablo::PabloKernel {
public:
    // With the region follows (the position after each match region),
    // occurrences that include a region follow are excluded.
    StringClassKernel(LLVMTypeSystemInterface & ts, std::vector<std::vector<re::CC *>> strings,
                      kernel::StreamSet * basis, kernel::StreamSet * fillStartsEnds,
                      kernel::StreamSet * follows = nullptr);
    bool hasSignature() const override { return true; }
    llvm::StringRef getSignature() const override { return mSignature; }
protected:
    void generatePabloMethod() override;
private:
    const std::vector<std::vector<re::CC *>> mStrings;
    const bool mHasFollows;
    const std::string mSignature;
};

//
// The fill, starts and ends (see StringClassKernel) of the string class of
// the one-character strings of a class X, given its stream: X itself,
// excluding the region follows.
//
class CharClassFillStartsEnds : public pablo::PabloKernel {
public:
    CharClassFillStartsEnds(LLVMTypeSystemInterface & ts, kernel::StreamSet * X, kernel::StreamSet * follows,
                            kernel::StreamSet * fillStartsEnds);
protected:
    void generatePabloMethod() override;
private:
    const bool mHasFollows;
};

//
// A star segment (s1|s2|...){lb,} (lb <= 1) of a lookahead chain, for a
// string class satisfying re::isRepeatableStringClass.  From a start of an
// occurrence p, the repetitions reach exactly the positions just after the
// end of an occurrence that lie after p within its run of Fill (or just
// after that run).  The rest of the chain must hold at one of them: these are
// the good positions G = H & Advance(Ends, 1).  StringClassStarIndex marks
// G together with the positions not in Fill (or at region follows) and the
// first position of each run of Fill, as the index I.  Shifting G back along I (IndexedShiftBack) marks each index
// position whose next index position is good; StringClassStarSpans spreads
// that mark forward over the positions before the next index position, and
// keeps the starts of occurrences (and, with lb = 0, the positions of H).
//
class StringClassStarIndex : public pablo::PabloKernel {
public:
    StringClassStarIndex(LLVMTypeSystemInterface & ts, kernel::StreamSet * fillStartsEnds, kernel::StreamSet * H,
                         kernel::StreamSet * breaks, kernel::StreamSet * index, kernel::StreamSet * good);
protected:
    void generatePabloMethod() override;
private:
    const bool mHasBreaks;
};

class StringClassStarSpans : public pablo::PabloKernel {
public:
    StringClassStarSpans(LLVMTypeSystemInterface & ts, unsigned lb, kernel::StreamSet * fillStartsEnds,
                         kernel::StreamSet * H, kernel::StreamSet * index, kernel::StreamSet * goodNext,
                         kernel::StreamSet * result);
protected:
    void generatePabloMethod() override;
private:
    const unsigned mLB;
};

//
// One-character lookaheads (?=Y) or (?!Y) of a lookahead chain, at its end
// or within it: given the streams of the classes Y (one per lookahead,
// negated as given), H marks the positions at which all of them hold (and,
// within the chain, the rest of the chain holds: Hrest).  At a region follow
// (the end of the text), only negative lookaheads hold.
//
class ChainAssertionEnd : public pablo::PabloKernel {
public:
    // Hrest: for lookaheads within the chain, where the rest of the chain holds
    // (nullptr at the end of the chain).
    ChainAssertionEnd(LLVMTypeSystemInterface & ts, std::vector<kernel::StreamSet *> classes,
                      std::vector<bool> negated, kernel::StreamSet * follows, kernel::StreamSet * Hrest,
                      kernel::StreamSet * H);
protected:
    void generatePabloMethod() override;
private:
    const std::vector<bool> mNegated;
    const bool mHasFollows;
    const bool mHasRest;
};

//
// The coverage of the matches of a lookahead chain (as parsed for match
// spans, see RE_PipelineBuilder::chainSpans): every position within some
// match.  From the starts of the matches, the segments are followed forward,
// each from its entries (the positions where it begins on the way to a
// complete match) to the entries of the next segment, which are where the
// rest of the chain holds (the stream H of the next segment):
//   - a fixed segment of length k covers each entry and the k - 1 positions
//     after it, and the next entries are k positions after;
//   - a star of a class covers the run of the class from each entry, and
//     the next entry is the end of the run (the rest holds only there);
//   - a star of a string class covers the positions reached from entries
//     that are starts of occurrences, up to the last end of an occurrence
//     followed by a position of H in the run of fill (see
//     StringClassStarSpans), and the next entries are the reached positions
//     after ends of occurrences where H holds (and the entries themselves
//     where H holds, with no lower bound);
//   - the end of the text and final lookaheads are zero-width.
// Final stars (of a class, or a string class with no lower bound) follow:
// each covers its runs from the entries, and every position it reaches is
// an entry of the next.  Runs exclude the region follows.
//
class ChainCoverage : public pablo::PabloKernel {
public:
    // Segment kinds: 'f' fixed (length), 's' class star, 'c' string-class
    // star (lower bound), 'z' zero-width.
    struct Step {
        char kind;
        unsigned n;     // fixed: the length; star: the lower bound
        kernel::StreamSet * Hnext;
        kernel::StreamSet * cls;
        kernel::StreamSet * fse;
        kernel::StreamSet * index;
        kernel::StreamSet * goodNext;
    };
    // A final star: 's' of a class (its stream), 'c' of a string class
    // (fill, starts and ends).
    struct Final {
        char kind;
        kernel::StreamSet * strm;
    };
    ChainCoverage(LLVMTypeSystemInterface & ts, kernel::StreamSet * starts, std::vector<Step> steps,
                  std::vector<Final> finals, kernel::StreamSet * follows, kernel::StreamSet * coverage);
protected:
    void generatePabloMethod() override;
private:
    const std::vector<Step> mSteps;
    const std::vector<Final> mFinals;
    const bool mHasFollows;
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

//
// The region follows (e.g. line breaks) of the regions that contain a match,
// given the matches (each marked at or after the end of the match, within
// its region).
//
namespace kernel {
class MatchedLinesKernel : public pablo::PabloKernel {
public:
    MatchedLinesKernel(LLVMTypeSystemInterface & ts, StreamSet * OriginalMatches, StreamSet * LineBreakStream, StreamSet * Matches);
protected:
    void generatePabloMethod() override;
};
}

//
// The records of a buffer that contain a match to an RE (as parsed), marked
// at their record breaks, for searches of records (e.g. lines) internal to
// clients.  The RE is compiled by the regular expression engine in the mode
// that it chooses; results of full Unicode indexing are spread back to code
// unit positions.
//   basis: the UTF-8 basis bits of the buffer.
//   u8index: the final UTF-8 code units of the characters.
//   breaks: the record breaks.
//   matchStarts: the starts of the records (LineStartsKernel of breaks).
//   records: the output, a bit at each break of a matching record.
//
void matchingRecords(kernel::PipelineBuilder & P, re::RE * re,
                     kernel::StreamSet * basis, kernel::StreamSet * u8index,
                     kernel::StreamSet * breaks, kernel::StreamSet * matchStarts,
                     kernel::StreamSet * records);

class RE_PipelineBuilder {
public:
    RE_PipelineBuilder(kernel::PipelineBuilder & P, RE_CompilerContext & ctxt) :
        mPB(P), mCtxt(ctxt),
        mHaveSourceContext(false), mPrepared(false), mHaveMode(false),
        mLineBreakHint(nullptr), mU8IndexHint(nullptr),
        mUsesUnicodeIndexing(false), mU8Index(nullptr),
        mFinalMatchStarts(nullptr), mFinalMatchFollows(nullptr) {}

    // Auto-determining entry point: the engine decides (and builds) whatever
    // representation it needs to compile a given RE against this source.
    RE_PipelineBuilder(kernel::PipelineBuilder & P, RE_context context);

    // Only meaningful for the RE_context constructor.
    // The options used when the pipeline methods prepare the RE themselves.
    void setModeOptions(const RE_ModeOptions & opts) {mModeOptions = opts;}
    void setLineBreakStream(kernel::StreamSet * lb) {mLineBreakHint = lb;}
    void setU8IndexHint(kernel::StreamSet * idx) {mU8IndexHint = idx;}

    bool usesUnicodeIndexing() const {return mUsesUnicodeIndexing;}
    kernel::StreamSet * getU8Index() const {return mU8Index;}
    kernel::StreamSet * getMatchStarts() const {return mFinalMatchStarts;}
    kernel::StreamSet * getMatchFollows() const {return mFinalMatchFollows;}

    // With the RE_context constructor, an RE as parsed is first prepared
    // (prepareRE); with a hand-built RE_CompilerContext, the RE is compiled
    // as given.
    void matchSearchPipeline(re::RE * re, kernel::StreamSet * results);
    void matchSpanPipeline(re::RE * re, kernel::StreamSet * matches, kernel::StreamSet * spans);
    void matchSearchPipeline(const PreparedRE & re, kernel::StreamSet * results);
    void matchSpanPipeline(const PreparedRE & re, kernel::StreamSet * matches, kernel::StreamSet * spans);
    kernel::PipelineBuilder & getPipelineBuilder() {return mPB;}

protected:
    // Internal methods.
    void addExternal(std::string extName, ExternalStream s);

    // The mode-specific steps for a prepared RE (expandPermutes, the RE passes
    // for the mode's length alphabet, externals, toUTF8, LookAheadNamer...).
    re::RE * applyModePasses(re::RE * re);

    re::RE * spanFactoring(re::RE * re);

    re::RE * processReferences(re::RE * re);

    void prepareExternals(re::RE * re);

    void compileExternal(re::Name * name);

    void compileProperty(re::PropertyExpression * pe);

    void getSpan(re::RE * re, kernel::StreamSet * spans);

    // The starts of the matches of a prepared RE (with its externals
    // compiled): stream marks, for each start position s of a match, the
    // position s + offset - 1 (as for a StartIndexed external).  A fixed
    // length RE gives its match ends, with the length as offset; an RE
    // Seq[P, S] for a named unique prefix P gives the ends of the matches of
    // P that begin matches of the RE, with the length of P as offset; a
    // lookahead chain (see parseLookaheadChain, with Unicode code units)
    // gives the starts themselves, with offset 1.
    struct MatchStarts {
        kernel::StreamSet * stream;
        unsigned offset;
    };
    MatchStarts matchStartPipeline(re::RE * re);

    // The streams used for a segment of a lookahead chain by chainMatchStarts,
    // as needed for the coverage of its matches (see ChainCoverage).
    struct ChainStepStreams {
        kernel::StreamSet * Hnext = nullptr;    // where the rest of the chain holds
        kernel::StreamSet * cls = nullptr;      // class star: the class
        kernel::StreamSet * fse = nullptr;      // string-class star: fill, starts, ends
        kernel::StreamSet * index = nullptr;    // string-class star: see StringClassStarIndex
        kernel::StreamSet * goodNext = nullptr;
    };

    // The starts of the matches of a lookahead chain (offset 1), recording
    // the streams of each segment if steps is given.  Hend marks where the
    // rest after the chain holds (nullptr for all positions).
    kernel::StreamSet * chainMatchStarts(const std::vector<re::LookaheadSegment> & segments,
                                         std::vector<ChainStepStreams> * steps = nullptr,
                                         kernel::StreamSet * Hend = nullptr);

    // The spans of the matches of an RE that is a chain (with no star
    // segment required), possibly followed by final stars, each of a class
    // or of a string class (with a lower bound of 1 only for the first; the
    // chain may then be empty).  Returns false if the RE is not of this
    // form, or positions are not code units.
    bool chainSpans(re::RE * re, kernel::StreamSet * spans);

    // The spans of the matches of an RE with a unique prefix (see
    // ParseUniquePrefix), from each match of the prefix that begins a match
    // to the longest end before the next (LongestSpan).  Returns false if
    // the RE has no unique prefix.
    bool uniquePrefixSpans(re::RE * re, kernel::StreamSet * spans);

    // The basis bits of the code units (transposed from a byte stream if
    // needed), for CC kernels.
    kernel::StreamSet * codeUnitBasis();

    // The stream of a character class, given as the RE re (which may be the
    // name of an external, whose stream is then used).
    kernel::StreamSet * classStream(re::RE * re, re::CC * cc);

    // For the matches of an RE Seq[P, S] with a named unique prefix P (see
    // matchStartPipeline), marked at their ends: the ends shifted back to the
    // preceding position of P or of an end.  Marked at a match of P, it
    // identifies those that begin matches of the RE.
    kernel::StreamSet * uniquePrefixEndsBack(kernel::StreamSet * prefix, kernel::StreamSet * ends);

    // Use the mode and options of a prepared RE.
    void usePrepared(const PreparedRE & prepared);

    // Mode determination + source preparation, run once before compiling.
    void ensurePrepared(re::RE *& re);

private:
    kernel::PipelineBuilder & mPB;
    RE_CompilerContext mCtxt;
    re::RE * mRE;

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
    kernel::StreamSet * mCodeUnitBasis = nullptr;
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

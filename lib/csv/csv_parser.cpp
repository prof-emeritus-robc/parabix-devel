#include <csv/csv_cmdline.h>
#include <csv/csv_parser.h>
#include <kernel/unicode/charclasses.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <pablo/bixnum/bixnum.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/re_cc.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/sentinel.h>

using namespace kernel;
using namespace pablo;

namespace csv {

char charLF = 0xA;
char charCR = 0xD;
//
//  Parse Quoted data within CSV files, based on the convention that
//  two quote marks in a row represent an escape sequence for an actual
//  quote character within the quoted string. 
//  Return a streamset consisting of two streams:
//  (a) a marker stream identifying the contents of each quoted string.
//  (b) a marker stream marking each quote escape character (first of
//      each pair of quotes within a quoted string).
//  Quote marks are at the final code unit of each quote character, which is
//  quoteLength code units long (a multibyte UTF-8 quote character), so the
//  two quotes of an escape pair are marked quoteLength apart.
class CSV_QuoteParser : public PabloKernel {
public:
    CSV_QuoteParser(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * quotedData, unsigned quoteLength)
        : PabloKernel(ts, "CSV_QuoteParser" + (quoteLength == 1 ? std::string() : "_" + std::to_string(quoteLength)),
                      {Binding{"csvCCs", csvCCs, FixedRate(), LookAhead(quoteLength)}},
                      {Binding{"quotedData", quotedData}})
    , mQuoteLength(quoteLength) {}
protected:
    void generatePabloMethod() override;
    const unsigned mQuoteLength;
};

void CSV_QuoteParser::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> csvMarks = getInputStreamSet("csvCCs");
    PabloAST * dquote = csvMarks[markDQ];
    PabloAST * dquote_odd = pb.createEveryNth(dquote, pb.getInteger(2));
    PabloAST * dquote_even = pb.createXor(dquote, dquote_odd);
    PabloAST * quote_escape = pb.createAnd(dquote_even, pb.createLookahead(dquote, mQuoteLength));
    PabloAST * escaped_quote = pb.createAdvance(quote_escape, mQuoteLength);
    PabloAST * start_dquote = pb.createXor(dquote_odd, escaped_quote);
    PabloAST * end_dquote = pb.createXor(dquote_even, quote_escape);
    PabloAST * start_data = pb.createAdvance(start_dquote, 1);
    PabloAST * quoted_data = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {start_data, end_dquote});
    writeOutputStreamSet("quotedData",  std::vector<PabloAST*>{quoted_data, quote_escape});
}

class CSV_RecordParser : public PabloKernel {
public:
    CSV_RecordParser(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * quotedData,
                    StreamSet * recordSeparators)
        : PabloKernel(ts, "CSVrecord_parser",
                      {Binding{"csvCCs", csvCCs}, Binding{"quotedData", quotedData}},
                      {Binding{"recordSeparators", recordSeparators}}) {}
protected:
    void generatePabloMethod() override;
};

void CSV_RecordParser::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> csvMarks = getInputStreamSet("csvCCs");
    std::vector<PabloAST *> quotedData = getInputStreamSet("quotedData");
    PabloAST * unquoted = pb.createNot(quotedData[0]);
    PabloAST * recordMarks = pb.createAnd(csvMarks[markLF], unquoted);
    pb.createAssign(pb.createExtract(getOutputStreamVar("recordSeparators"), pb.getInteger(0)), recordMarks);
}

class CSV_FieldParser : public PabloKernel {
public:
    CSV_FieldParser(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * quotedData,
                    StreamSet * fieldStarts, StreamSet * fieldFollows)
        : PabloKernel(ts, "CSVparser",
                      {Binding{"csvCCs", csvCCs},
                       Binding{"quotedData", quotedData}},
                      {Binding{"fieldStarts", fieldStarts},
                       Binding{"fieldFollows", fieldFollows}}) {}
protected:
    void generatePabloMethod() override;
};

//  The CSV character classes with the quote and delimiter streams replaced by
//  syntaxMarks (<quote marks, delimiter marks>).
class CSV_SyntaxMarks : public PabloKernel {
public:
    CSV_SyntaxMarks(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * syntaxMarks, StreamSet * markedCCs)
        : PabloKernel(ts, "CSV_SyntaxMarks",
                      {Binding{"csvCCs", csvCCs}, Binding{"syntaxMarks", syntaxMarks}},
                      {Binding{"markedCCs", markedCCs}}) {}
protected:
    void generatePabloMethod() override {
        pablo::PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> ccs = getInputStreamSet("csvCCs");
        std::vector<PabloAST *> marks = getInputStreamSet("syntaxMarks");
        ccs[markDQ] = marks[0];
        ccs[markComma] = marks[1];
        writeOutputStreamSet("markedCCs", ccs);
    }
};

void CSV_FieldParser::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> csvMarks = getInputStreamSet("csvCCs");
    std::vector<PabloAST *> quotedData = getInputStreamSet("quotedData");
    PabloAST * unquoted = pb.createNot(quotedData[0]);
    PabloAST * recordMarks = pb.createAnd(csvMarks[markLF], unquoted);
    PabloAST * fieldFollows = pb.createAnd(pb.createOr(csvMarks[markComma], csvMarks[markCR]), unquoted);
    // In the event that CSV records are terminated by bare LF and not CRLF, we update
    // fieldFollows to include this case.
    PabloAST * bareLF = pb.createAnd(pb.createNot(pb.createAdvance(csvMarks[markCR], 1)), recordMarks);
    fieldFollows = pb.createOr(fieldFollows, bareLF);
    PabloAST * fieldFinals = pb.createAnd(pb.createOr(csvMarks[markComma], csvMarks[markLF]), unquoted);
    PabloAST * fieldStarts = pb.createInFile(pb.createNot(pb.createAdvance(pb.createNot(fieldFinals), 1)));
    pb.createAssign(pb.createExtract(getOutputStreamVar("fieldStarts"), pb.getInteger(0)), fieldStarts);
    pb.createAssign(pb.createExtract(getOutputStreamVar("fieldFollows"), pb.getInteger(0)), fieldFollows);
}

CSV_Parser::CSV_Parser(PipelineBuilder & P, codepoint_t quoteCp, codepoint_t separatorCp) :
    mPB(P), mQuoteCp(quoteCp), mSeparatorCp(separatorCp), mCsvCCs(nullptr), mQuoteLength(1), mQuotedData(nullptr),
    mFieldStarts(nullptr), mFieldFollows(nullptr), mLineEnds(nullptr) {

    }

void CSV_Parser::setSource(StreamSet * basis, StreamSet * syntaxMarks) {
    mQuotedData = nullptr;
    mLineEnds = nullptr;
    mFieldStarts = nullptr;
    mFieldFollows = nullptr;
    std::vector<re::CC *> csv_marks(4);
    csv_marks[markLF] = re::makeCC(charLF);
    csv_marks[markCR] = re::makeCC(charCR);
    csv_marks[markDQ] = re::makeCC(mQuoteCp);
    csv_marks[markComma] = re::makeCC(mSeparatorCp);
    mCsvCCs = mPB.CreateStreamSet(4);
    mPB.CreateKernelCall<CharClassesKernel>(csv_marks, basis, mCsvCCs);
    mQuoteLength = mQuoteCp < 0x80 ? 1 : mQuoteCp < 0x800 ? 2 : mQuoteCp < 0x10000 ? 3 : 4;
    if (syntaxMarks) {
        StreamSet * markedCCs = mPB.CreateStreamSet(4);
        mPB.CreateKernelCall<CSV_SyntaxMarks>(mCsvCCs, syntaxMarks, markedCCs);
        mCsvCCs = markedCCs;
        mQuoteLength = 1;
    }
}

StreamSet * CSV_Parser::getCsvCCs() {return mCsvCCs;}
StreamSet * CSV_Parser::getQuotedData() {
    if (mQuotedData == nullptr) {
        mQuotedData = mPB.CreateStreamSet(2);
        mPB.CreateKernelCall<CSV_QuoteParser>(mCsvCCs, mQuotedData, mQuoteLength);
    }
    return mQuotedData;
}
StreamSet * CSV_Parser::getLineEnds() {
    if (mLineEnds == nullptr) {
        mFieldStarts = mPB.CreateStreamSet(1);
        mFieldFollows = mPB.CreateStreamSet(1);
        mLineEnds = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<CSV_RecordParser>(mCsvCCs, getQuotedData(), mLineEnds);
    }
    return mLineEnds;
}

StreamSet * CSV_Parser::getFieldStarts() {
    if (mFieldStarts == nullptr) {
        mFieldStarts = mPB.CreateStreamSet(1);
        mFieldFollows = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<CSV_FieldParser>(mCsvCCs, getQuotedData(), mFieldStarts, mFieldFollows);
    }
    return mFieldStarts;
}
StreamSet * CSV_Parser::getFieldFollows() {
    if (mFieldFollows == nullptr) {
        mFieldStarts = mPB.CreateStreamSet(1);
        mFieldFollows = mPB.CreateStreamSet(1);
        mPB.CreateKernelCall<CSV_FieldParser>(mCsvCCs, getQuotedData(), mFieldStarts, mFieldFollows);
    }
    return mFieldFollows;
}



class SelectMultiField : public PabloKernel {
public:
    SelectMultiField(LLVMTypeSystemInterface & ts,
                     StreamSet * Record_separators,
                     StreamSet * fieldStarts,
                     StreamSet * fieldFollows,
                     StreamSet * toKeep,
                     const std::vector<unsigned> & columnNos,
                     bool forCut);
protected:
    std::string colNoString(const std::vector<unsigned> & cols, bool forCut);
    void generatePabloMethod() override;
    const std::vector<unsigned> & mColumnNos;
    bool mForCut;
};

SelectMultiField::SelectMultiField(LLVMTypeSystemInterface & ts,
                                   StreamSet * Record_separators,
                                   StreamSet * fieldStarts,
                                   StreamSet * fieldFollows,
                                   StreamSet * toKeep,
                                   const std::vector<unsigned> & columnNos,
                                   bool forCut)
: PabloKernel(ts, "SelectMultiField_" + colNoString(columnNos, forCut),
  {Binding{"Record_separators", Record_separators},
   Binding{"fieldStarts", fieldStarts},
   Binding{"fieldFollows", fieldFollows}},
  {Binding{"toKeep", toKeep}}), mColumnNos(columnNos), mForCut(forCut) {
}

std::string SelectMultiField::colNoString(const std::vector<unsigned> & columnNos, bool forCut) {
    std::stringstream ss;
    ss << columnNos[0];
    for (unsigned i = 1; i < columnNos.size(); i++) {
        ss << "," << columnNos[i];
    }
    if (forCut) {
        ss << "_c";
    }
    return ss.str();
}

void SelectMultiField::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * Record_separators = pb.createExtract(getInputStreamVar("Record_separators"), pb.getInteger(0));
    PabloAST * fieldStarts = pb.createExtract(getInputStreamVar("fieldStarts"), pb.getInteger(0));
    PabloAST * fieldFollows = pb.createExtract(getInputStreamVar("fieldFollows"), pb.getInteger(0));
    PabloAST * recordStarts = pb.createNot(pb.createAdvance(pb.createNot(Record_separators), 1));
    PabloAST * columnMark = recordStarts;
    PabloAST * toKeep = pb.createZeroes();
    unsigned nextCol = 0;
    for (unsigned i = 0; i < mColumnNos.size(); i++) {
        if (mColumnNos[i] > nextCol) {
            columnMark = pb.createIndexedAdvance(columnMark, fieldStarts, mColumnNos[i] - nextCol);
        }
        PabloAST * columnFollow = pb.createScanTo(columnMark, fieldFollows);
        PabloAST * columnMask  = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {columnMark, columnFollow});
        toKeep = pb.createOr(toKeep, columnMask);
        nextCol = mColumnNos[i] + 1;
        columnMark = pb.createAdvance(columnFollow, 1);
        if (mForCut && (i == mColumnNos.size() - 1)) {
            toKeep = pb.createOr(toKeep, Record_separators);
        } else {
            toKeep = pb.createOr(toKeep, columnFollow);
        }
    }
    pb.createAssign(pb.createExtract(getOutputStreamVar("toKeep"), pb.getInteger(0)), pb.createInFile(toKeep));
}

void ColumnSelectionMask(PipelineBuilder & P, StreamSet * Record_separators, StreamSet * fieldStarts, StreamSet * fieldFollows,
                         StreamSet * toKeep, const std::vector<unsigned> & columnNos, bool forCut) {
    P.CreateKernelCall<SelectMultiField>(Record_separators, fieldStarts, fieldFollows, toKeep, columnNos, forCut);
}

class FieldSeparatorsKernel : public PabloKernel {
public:
    FieldSeparatorsKernel(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * quotedData,
                          StreamSet * delimiterStarts, StreamSet * separators, StreamSet * keepMask)
        : PabloKernel(ts, std::string("CSV_FieldSeparators") + (delimiterStarts ? "_delimStarts" : "")
                          + (keepMask ? "" : "_keepCR") + (separators ? "" : "_CRmaskOnly"),
                      inputBindings(csvCCs, quotedData, delimiterStarts),
                      outputBindings(separators, keepMask)),
          mHasDelimiterStarts(delimiterStarts != nullptr), mHasSeparators(separators != nullptr),
          mHasKeepMask(keepMask != nullptr) {}
protected:
    static Bindings inputBindings(StreamSet * csvCCs, StreamSet * quotedData, StreamSet * delimiterStarts) {
        Bindings inputs{Binding{"csvCCs", csvCCs, FixedRate(), LookAhead(1)},
                        Binding{"quotedData", quotedData}};
        if (delimiterStarts) {
            inputs.emplace_back("delimiterStarts", delimiterStarts);
        }
        return inputs;
    }
    static Bindings outputBindings(StreamSet * separators, StreamSet * keepMask) {
        Bindings outputs;
        if (separators) {
            outputs.emplace_back("separators", separators);
        }
        if (keepMask) {
            outputs.emplace_back("keepMask", keepMask);
        }
        return outputs;
    }
    void generatePabloMethod() override;
    const bool mHasDelimiterStarts;
    const bool mHasSeparators;
    const bool mHasKeepMask;
};

void FieldSeparatorsKernel::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> csvMarks = getInputStreamSet("csvCCs");
    std::vector<PabloAST *> quotedData = getInputStreamSet("quotedData");
    PabloAST * unquoted = pb.createNot(quotedData[0]);
    PabloAST * recordEnds = pb.createAnd(csvMarks[markLF], unquoted);
    //  A delimiter is wholly inside or wholly outside quotes, so its first code
    //  unit is unquoted exactly when its last is.
    PabloAST * delimMarks = mHasDelimiterStarts ? getInputStreamSet("delimiterStarts")[0] : csvMarks[markComma];
    PabloAST * delimiters = pb.createAnd(delimMarks, unquoted);
    if (mHasSeparators) {
        PabloAST * separators = pb.createInFile(pb.createOr(delimiters, recordEnds));
        pb.createAssign(pb.createExtract(getOutputStreamVar("separators"), pb.getInteger(0)), separators);
    }
    if (mHasKeepMask) {
        PabloAST * CR_of_CRLF = pb.createAnd3(csvMarks[markCR], unquoted, pb.createLookahead(csvMarks[markLF], 1));
        PabloAST * keepMask = pb.createInFile(pb.createNot(CR_of_CRLF));
        pb.createAssign(pb.createExtract(getOutputStreamVar("keepMask"), pb.getInteger(0)), keepMask);
    }
}

void FieldSeparatorsAndCRMask(PipelineBuilder & P, StreamSet * csvCCs, StreamSet * quotedData,
                              StreamSet * separators, StreamSet * keepMask, StreamSet * delimiterStarts) {
    P.CreateKernelCall<FieldSeparatorsKernel>(csvCCs, quotedData, delimiterStarts, separators, keepMask);
}

unsigned UTF8_Length(codepoint_t cp) {
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

class CSV_StandardSyntax : public PabloKernel {
public:
    CSV_StandardSyntax(LLVMTypeSystemInterface & ts, StreamSet * basis, StreamSet * syntaxStarts,
                       StreamSet * quotedData, StreamSet * toKeep, unsigned quoteLength, unsigned delimiterLength,
                       StreamSet * translatedBasis, StreamSet * syntaxMarks, StreamSet * keepMask)
        : PabloKernel(ts, "CSV_StandardSyntax_" + std::to_string(quoteLength) + "_" + std::to_string(delimiterLength),
                      {Binding{"basis", basis}, Binding{"syntaxStarts", syntaxStarts},
                       Binding{"quotedData", quotedData, FixedRate(), LookAhead(quoteLength - 1)},
                       Binding{"toKeep", toKeep}},
                      {Binding{"translatedBasis", translatedBasis}, Binding{"syntaxMarks", syntaxMarks},
                       Binding{"keepMask", keepMask}})
    , mQuoteLength(quoteLength), mDelimiterLength(delimiterLength) {}
protected:
    void generatePabloMethod() override;
    const unsigned mQuoteLength;
    const unsigned mDelimiterLength;
};

void CSV_StandardSyntax::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    std::vector<PabloAST *> starts = getInputStreamSet("syntaxStarts");
    std::vector<PabloAST *> quotedData = getInputStreamSet("quotedData");
    PabloAST * unquoted = pb.createNot(quotedData[0]);
    //  The quote escapes are marked at the final code unit of the first quote
    //  of each pair; the second quote begins at the next code unit.
    PabloAST * quoteEscape = quotedData[1];
    PabloAST * escapeStarts = (mQuoteLength == 1) ? quoteEscape : pb.createLookahead(quoteEscape, mQuoteLength - 1);
    PabloAST * escapedStarts = pb.createAdvance(quoteEscape, 1);
    PabloAST * quotes = pb.createAnd(starts[0], pb.createNot(pb.createOr(escapeStarts, escapedStarts)));
    PabloAST * delims = pb.createAnd(starts[1], unquoted);
    PabloAST * toKeep = getInputStreamSet("toKeep")[0];
    PabloAST * rest = escapeStarts;
    for (unsigned i = 1; i < mQuoteLength; i++) {
        rest = pb.createOr(rest, pb.createAdvance(quotes, i));
        rest = pb.createOr(rest, pb.createAdvance(escapeStarts, i));
    }
    for (unsigned i = 1; i < mDelimiterLength; i++) {
        rest = pb.createOr(rest, pb.createAdvance(delims, i));
    }
    const unsigned quote = 0x22;
    const unsigned comma = 0x2C;
    PabloAST * marked = pb.createOr(quotes, delims);
    for (unsigned i = 0; i < 8; i++) {
        PabloAST * bit = pb.createOr(((quote >> i) & 1) ? quotes : pb.createZeroes(),
                                     ((comma >> i) & 1) ? delims : pb.createZeroes());
        basis[i] = pb.createSel(marked, bit, basis[i]);
    }
    writeOutputStreamSet("translatedBasis", basis);
    writeOutputStreamSet("syntaxMarks", std::vector<PabloAST *>{quotes, delims});
    writeOutputStreamSet("keepMask", std::vector<PabloAST *>{pb.createAnd(toKeep, pb.createNot(rest))});
}

void StandardSyntax(PipelineBuilder & P, StreamSet * basis, StreamSet * quotedData, StreamSet * toKeep,
                    codepoint_t quoteCp, codepoint_t delimiterCp,
                    StreamSet * translatedBasis, StreamSet * syntaxMarks, StreamSet * keepMask) {
    StreamSet * syntaxStarts = P.CreateStreamSet(2);
    P.CreateKernelCall<CharClassesKernel>(std::vector<re::CC *>{re::makeCC(quoteCp), re::makeCC(delimiterCp)},
                                          basis, syntaxStarts, pablo::BitMovementMode::LookAhead);
    P.CreateKernelCall<CSV_StandardSyntax>(basis, syntaxStarts, quotedData, toKeep,
                                           UTF8_Length(quoteCp), UTF8_Length(delimiterCp),
                                           translatedBasis, syntaxMarks, keepMask);
}

//  The quotes to be inserted in data of standard syntax (quote and delimiter
//  marks), and its separators.  A '"' in the data is to be doubled; a field not
//  quoted is to be quoted if it has a '"' or ',' in its data, and its quotedFollows
//  marks its fieldFollows.
class CSV_QuoteInsertions : public PabloKernel {
public:
    CSV_QuoteInsertions(LLVMTypeSystemInterface & ts, StreamSet * basis, StreamSet * csvCCs, StreamSet * quotedData,
                        StreamSet * fieldFollows, StreamSet * dataQuotes, StreamSet * quotedFollows, StreamSet * separators)
        : PabloKernel(ts, "CSV_QuoteInsertions",
                      {Binding{"basis", basis}, Binding{"csvCCs", csvCCs}, Binding{"quotedData", quotedData},
                       Binding{"fieldFollows", fieldFollows}},
                      {Binding{"dataQuotes", dataQuotes}, Binding{"quotedFollows", quotedFollows},
                       Binding{"separators", separators}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        cc::Parabix_CC_Compiler_Builder ccc(getInputStreamSet("basis"));
        std::vector<PabloAST *> csvMarks = getInputStreamSet("csvCCs");
        PabloAST * unquoted = pb.createNot(getInputStreamSet("quotedData")[0]);
        PabloAST * fieldFollows = getInputStreamSet("fieldFollows")[0];
        PabloAST * quoteChars = ccc.compileCC(re::makeByte(0x22), pb);
        PabloAST * commaChars = ccc.compileCC(re::makeByte(0x2C), pb);
        PabloAST * dataQuotes = pb.createAnd(quoteChars, pb.createNot(csvMarks[markDQ]));
        PabloAST * dataCommas = pb.createAnd(commaChars, pb.createNot(csvMarks[markComma]));
        PabloAST * triggers = pb.createAnd(pb.createOr(dataQuotes, dataCommas), unquoted);
        PabloAST * quotedFollows = pb.createScanTo(triggers, fieldFollows);
        PabloAST * separators = pb.createOr(pb.createAnd(csvMarks[markComma], unquoted),
                                            pb.createAnd(csvMarks[markLF], unquoted));
        writeOutputStreamSet("dataQuotes", std::vector<PabloAST *>{dataQuotes});
        writeOutputStreamSet("quotedFollows", std::vector<PabloAST *>{quotedFollows});
        writeOutputStreamSet("separators", std::vector<PabloAST *>{pb.createInFile(separators)});
    }
};

//  The number of quotes to insert before each position.
class CSV_QuoteInsertBixNum : public PabloKernel {
public:
    CSV_QuoteInsertBixNum(LLVMTypeSystemInterface & ts, StreamSet * dataQuotes, StreamSet * quotedStarts,
                          StreamSet * quotedFollows, StreamSet * insertBixNum)
        : PabloKernel(ts, "CSV_QuoteInsertBixNum",
                      {Binding{"dataQuotes", dataQuotes}, Binding{"quotedStarts", quotedStarts},
                       Binding{"quotedFollows", quotedFollows}},
                      {Binding{"insertBixNum", insertBixNum}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * dataQuotes = getInputStreamSet("dataQuotes")[0];
        PabloAST * quotedStarts = getInputStreamSet("quotedStarts")[0];
        PabloAST * quotedFollows = getInputStreamSet("quotedFollows")[0];
        //  A field start may have a data quote (two insertions); a field follow
        //  is a separator, never a data quote or the start of a field to quote.
        PabloAST * bit0 = pb.createXor(pb.createXor(dataQuotes, quotedStarts), quotedFollows);
        PabloAST * bit1 = pb.createAnd(dataQuotes, quotedStarts);
        writeOutputStreamSet("insertBixNum", std::vector<PabloAST *>{bit0, bit1});
    }
};

//  The inserted positions (zero in the spread mask) become '"'; keepMask marks
//  every position but the sentinel.
class CSV_FillQuotes : public PabloKernel {
public:
    CSV_FillQuotes(LLVMTypeSystemInterface & ts, StreamSet * basis, StreamSet * spreadMask, StreamSet * sentinel,
                   StreamSet * filled, StreamSet * keepMask)
        : PabloKernel(ts, "CSV_FillQuotes",
                      {Binding{"basis", basis}, Binding{"spreadMask", spreadMask}, Binding{"sentinel", sentinel}},
                      {Binding{"filled", filled}, Binding{"keepMask", keepMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("basis");
        PabloAST * inserted = pb.createInFile(pb.createNot(getInputStreamSet("spreadMask")[0]));
        //  '"' is 0x22: bits 1 and 5.
        basis[1] = pb.createOr(basis[1], inserted);
        basis[5] = pb.createOr(basis[5], inserted);
        writeOutputStreamSet("filled", basis);
        PabloAST * keep = pb.createInFile(pb.createNot(getInputStreamSet("sentinel")[0]));
        writeOutputStreamSet("keepMask", std::vector<PabloAST *>{keep});
    }
};

void StandardizeCSV(PipelineBuilder & P, StreamSet * basis, codepoint_t quoteCp, codepoint_t delimiterCp,
                    StreamSet * standardBasis, StreamSet * separators) {
    CSV_Parser parser(P, quoteCp, delimiterCp);
    parser.setSource(basis);
    //  The CRs of CRLFs, then the translation to standard syntax.
    StreamSet * crMask = P.CreateStreamSet(1);
    FieldSeparatorsAndCRMask(P, parser.getCsvCCs(), parser.getQuotedData(), nullptr, crMask);
    StreamSet * translatedBasis = P.CreateStreamSet(8);
    StreamSet * allSyntaxMarks = P.CreateStreamSet(2);
    StreamSet * keepMask = P.CreateStreamSet(1);
    StandardSyntax(P, basis, parser.getQuotedData(), crMask, quoteCp, delimiterCp, translatedBasis, allSyntaxMarks, keepMask);
    StreamSet * filteredBasis = P.CreateStreamSet(8);
    FilterByMask(P, keepMask, translatedBasis, filteredBasis);
    StreamSet * syntaxMarks = P.CreateStreamSet(2);
    FilterByMask(P, keepMask, allSyntaxMarks, syntaxMarks);
    //  The quotes to insert in the standard syntax.  A sentinel position (an
    //  0xFF code unit, marked as a delimiter) ends the data, so that the last
    //  field of an unterminated final record has a follow position.  It is
    //  removed after the insertions.
    StreamSet * sentinelBasis = P.CreateStreamSet(8);
    P.CreateKernelCall<AddSentinel>(filteredBasis, sentinelBasis);
    StreamSet * sentinelMarks = P.CreateStreamSet(2);
    P.CreateKernelCall<AddSentinel>(syntaxMarks, sentinelMarks);
    StreamSet * sentinel = P.CreateStreamSet(1);
    P.CreateKernelCall<EOFbit>(filteredBasis, sentinel);
    filteredBasis = sentinelBasis;
    parser.setSource(filteredBasis, sentinelMarks);
    StreamSet * fieldStarts = parser.getFieldStarts();
    StreamSet * fieldFollows = parser.getFieldFollows();
    StreamSet * dataQuotes = P.CreateStreamSet(1);
    StreamSet * quotedFollows = P.CreateStreamSet(1);
    StreamSet * filteredSeparators = P.CreateStreamSet(1);
    P.CreateKernelCall<CSV_QuoteInsertions>(filteredBasis, parser.getCsvCCs(), parser.getQuotedData(), fieldFollows,
                                            dataQuotes, quotedFollows, filteredSeparators);
    StreamSet * quotedByField = P.CreateStreamSet(1);
    FilterByMask(P, fieldFollows, quotedFollows, quotedByField);
    StreamSet * quotedStarts = P.CreateStreamSet(1);
    SpreadByMask(P, fieldStarts, quotedByField, quotedStarts);
    StreamSet * insertBixNum = P.CreateStreamSet(2);
    P.CreateKernelCall<CSV_QuoteInsertBixNum>(dataQuotes, quotedStarts, quotedFollows, insertBixNum);
    StreamSet * spreadMask = P.CreateStreamSet(1);
    InsertionSpreadMask(P, insertBixNum, spreadMask, InsertPosition::Before);
    StreamSet * expandedBasis = P.CreateStreamSet(8);
    SpreadByMask(P, spreadMask, filteredBasis, expandedBasis);
    StreamSet * expandedSentinel = P.CreateStreamSet(1);
    SpreadByMask(P, spreadMask, sentinel, expandedSentinel);
    StreamSet * filledBasis = P.CreateStreamSet(8);
    StreamSet * withoutSentinel = P.CreateStreamSet(1);
    P.CreateKernelCall<CSV_FillQuotes>(expandedBasis, spreadMask, expandedSentinel, filledBasis, withoutSentinel);
    FilterByMask(P, withoutSentinel, filledBasis, standardBasis);
    StreamSet * expandedSeparators = P.CreateStreamSet(1);
    SpreadByMask(P, spreadMask, filteredSeparators, expandedSeparators);
    FilterByMask(P, withoutSentinel, expandedSeparators, separators);
}

class EmptyFieldsKernel : public PabloKernel {
public:
    EmptyFieldsKernel(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * fieldFollows,
                      StreamSet * EmptyFields);
protected:
    void generatePabloMethod() override;
};

EmptyFieldsKernel::EmptyFieldsKernel(LLVMTypeSystemInterface & ts, StreamSet * csvCCs, StreamSet * fieldFollows,
                                     StreamSet * EmptyMarks)
: PabloKernel(ts, "EmptyFieldsKernel" + csvCCs->shapeString(),
  {Binding{"csvCCs", csvCCs},
   Binding{"fieldFollows", fieldFollows, FixedRate(), LookAhead(1)}},
  {Binding{"EmptyMarks", EmptyMarks}})  {
}

void EmptyFieldsKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> csvMarks = getInputStreamSet("csvCCs");
    PabloAST * fieldFollows = pb.createExtract(getInputStreamVar("fieldFollows"), pb.getInteger(0));
    PabloAST * fsNext = pb.createAdvance(fieldFollows, 1);
    PabloAST * quotedStart = pb.createAnd(fsNext, csvMarks[csv::markDQ]);
    PabloAST * quoted2 = pb.createAnd(pb.createAdvance(quotedStart, 1), csvMarks[csv::markDQ]);
    PabloAST * empties = pb.createAnd(pb.createOr(fsNext, pb.createAdvance(quoted2, 1)), fieldFollows);
    pb.createAssign(pb.createExtract(getOutputStreamVar("EmptyMarks"), pb.getInteger(0)), empties);
}

void GetEmptyFields(PipelineBuilder & P, StreamSet * csvCCs, StreamSet * fieldFollows,
                    StreamSet * EmptyFieldMarks) {
    P.CreateKernelCall<EmptyFieldsKernel>(csvCCs, fieldFollows, EmptyFieldMarks);
}
}

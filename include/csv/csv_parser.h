/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  This file defines the command line options interface for Parabix csv tools.
 *
 */
#pragma once

#include <string>
#include <vector>
#include <kernel/pipeline/pipeline_builder.h>
#include <ucd/core/UCD_Config.h>

using PipelineBuilder = kernel::PipelineBuilder;
using StreamSet = kernel::StreamSet;
using codepoint_t = UCD::codepoint_t;

namespace csv {

// Produce a set of 4 character class streams for CSV processing.
// LF - line feed stream
// CR - carriage return stream
// DQ - quote stream possibly overriden by csv::QuoteChar.
// Comma - comma stream, possibly overriden by csv::FieldDelimiter.
enum {markLF = 0, markCR = 1, markDQ = 2, markComma = 3};

void ColumnSelectionMask(PipelineBuilder & P,
                         StreamSet * Record_separators, StreamSet * fieldStarts, StreamSet * fieldFollows,
                         StreamSet * toKeep, const std::vector<unsigned> & columnNos, bool forCut = false);

// Separators for field rearrangement (DataRearrangementKernel): unquoted field
// delimiters and unquoted LFs (record separators).  keepMask marks every
// position except the unquoted CR of each CRLF, which is to be deleted from the
// data (and from the separators) so that each record ends at its LF.
// For a multibyte UTF-8 delimiter, delimiterStarts marks the first code unit of
// each delimiter (CharClassesKernel in LookAhead mode), and the separators mark
// the delimiters there, so that the whole delimiter follows the field it ends.
// Otherwise (nullptr), the delimiter marks of csvCCs are used.
// With keepMask nullptr, only the separators are computed (CRs are kept);
// with separators nullptr, only the keepMask.
void FieldSeparatorsAndCRMask(PipelineBuilder & P, StreamSet * csvCCs, StreamSet * quotedData,
                              StreamSet * separators, StreamSet * keepMask,
                              StreamSet * delimiterStarts = nullptr);

// The length of a code point in UTF-8 code units.
unsigned UTF8_Length(codepoint_t cp);

// Standard CSV syntax for a CSV with quote character quoteCp and field delimiter
// delimiterCp (either possibly a multibyte UTF-8 character; quotedData parsed
// with them).  Each opening and closing quote becomes '"' and each delimiter
// outside quotes ',': the first code unit is translated and marked in
// syntaxMarks (<quotes, delimiters>), and its other code units are removed from
// toKeep, giving keepMask.  Of each escape pair of quote characters, the first
// is removed and the second kept as data.  Parsing the translated data
// (CSV_Parser::setSource with the filtered syntaxMarks) takes the quotes and
// delimiters at the marks, so that '"' and ',' characters in the data are data.
void StandardSyntax(PipelineBuilder & P, StreamSet * basis, StreamSet * quotedData, StreamSet * toKeep,
                    codepoint_t quoteCp, codepoint_t delimiterCp,
                    StreamSet * translatedBasis, StreamSet * syntaxMarks, StreamSet * keepMask);

// Standard CSV (RFC 4180 style) from a CSV with quote character quoteCp and
// field delimiter delimiterCp: ',' delimiters, '"' quotes and LF line ends
// (the CR of each CRLF is removed).  A '"' in the data is doubled, and a field
// not quoted that has a '"' or ',' in its data is quoted.  separators marks the
// delimiters and line ends of the standard data.
void StandardizeCSV(PipelineBuilder & P, StreamSet * basis, codepoint_t quoteCp, codepoint_t delimiterCp,
                    StreamSet * standardBasis, StreamSet * separators);

void GetEmptyFields(PipelineBuilder & P, StreamSet * csvCCs, StreamSet * fieldSeparators,
                    StreamSet * EmptyFieldMarks);

class CSV_Parser {
public:
    CSV_Parser(PipelineBuilder & P, codepoint_t quoteCp, codepoint_t separatorCp);

    //  With syntaxMarks, <quote marks, delimiter marks> (one code unit each),
    //  the quotes and field delimiters are at those marks rather than at the
    //  quote and delimiter characters, so that occurrences of these characters
    //  in the data are not syntax.
    void setSource(StreamSet * basis, StreamSet * syntaxMarks = nullptr);
    StreamSet * getCsvCCs();
    StreamSet * getQuotedData();
    StreamSet * getLineEnds();
    StreamSet * getFieldStarts();
    StreamSet * getFieldFollows();

private:
    PipelineBuilder mPB;
    codepoint_t mQuoteCp;
    codepoint_t mSeparatorCp;
    StreamSet * mCsvCCs;
    unsigned mQuoteLength;
    StreamSet * mQuotedData;
    StreamSet * mFieldStarts;
    StreamSet * mFieldFollows;
    StreamSet * mLineEnds;
};

}

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
void FieldSeparatorsAndCRMask(PipelineBuilder & P, StreamSet * csvCCs, StreamSet * quotedData,
                              StreamSet * separators, StreamSet * keepMask);

void GetEmptyFields(PipelineBuilder & P, StreamSet * csvCCs, StreamSet * fieldSeparators,
                    StreamSet * EmptyFieldMarks);

class CSV_Parser {
public:
    CSV_Parser(PipelineBuilder & P, codepoint_t quoteCp, codepoint_t separatorCp);

    void setSource(StreamSet * basis);
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
    StreamSet * mQuotedData;
    StreamSet * mFieldStarts;
    StreamSet * mFieldFollows;
    StreamSet * mLineEnds;
};

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <cstddef>
#include <toolchain/toolchain.h>

namespace kernel { class StreamSet; class PipelineBuilder; }

namespace kernel {

//
//  Well-formed UTF-8 from UTF-8 input, for tools that process the code units
//  of a text (its 8 basis bit streams) and need well-formed text: the maximal
//  ill-formed subparts of the input (the Unicode Standard, section 3.9, see
//  UTF8_structure) are treated as the --utf8-errors option says
//  (codegen::UTF8Errors):
//    - UseReplacementChar: each replaced by U+FFFD (EF BF BD);
//    - DropBadInput: each deleted;
//    - Abort: the text up to the first of them (the rest is not processed;
//      the tool reports the error, e.g. at the offset UTF8_ErrorOffset gives).
//  This is the text that decoding UTF-8 gives (see UTF8_Decoder), as UTF-8.
//
//  The repair is a few kernels: one finding the subparts (and setting the
//  first byte of each to EF, with replacement characters), then the deletion
//  of the rest of their bytes (FilterByMask) and, with replacement characters,
//  the insertion of two positions after each first byte (InsertionSpreadMask,
//  SpreadByMask) and the writing of BF BD there.  On well-formed text, nothing
//  is deleted or inserted: the fast paths of these kernels copy their strides.
//
StreamSet * WellFormedUTF8(PipelineBuilder & P, StreamSet * Basis,
                           codegen::UTF8ErrorMode mode = codegen::UTF8Errors);
//  The same, written to Result (8 streams; e.g. an output of a pipeline).
void WellFormedUTF8(PipelineBuilder & P, StreamSet * Basis, StreamSet * Result,
                    codegen::UTF8ErrorMode mode = codegen::UTF8Errors);

//  The offset of the first maximal ill-formed subpart of UTF-8 text (length if
//  none): for a tool to report the error of the Abort mode.
size_t UTF8_ErrorOffset(const char * text, size_t length);
//  Whether the input of a file descriptor, read again from its start (a
//  regular file), has ill-formed UTF-8, and the offset of the first error
//  (false if it cannot be read again).
bool UTF8_FileError(int fd, bool useMMap, size_t & offset);

}

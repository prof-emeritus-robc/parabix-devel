/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  Line Break Support
 *
 *  This module provides methods for identifying lines/records in a file based
 *  on three standard conventions, together with attributes for null data and
 *  unterminated final lines.
 *
 *  1.   Unix Lines terminated by LF
 *  2.   Unicode lines terminated by LF, CR, VT, FF, NEL, PS, LS and CRLF
 *  3.   Strings terminated by NUL.
 */
#ifndef LINEBREAK_KERNEL_H
#define LINEBREAK_KERNEL_H

#include <pablo/pablo.h>
#include <re/alphabet/alphabet.h>

namespace kernel { class KernelBuilder; }
namespace kernel { class PipelineBuilder; }

/*  An input file may contain a final line without a line terminator.
    Line break kernels can add logic to produce a mark one past EOF
    to indicate the end of such an unterminated final line. */
enum class UnterminatedLineAtEOF {Ignore, Add1};

/*  Line break kernels may handle null characters in an input file using
    one of three different modes:
    Data - the null character is accepted as a data character
    Break - the null character is accepted as an alternative line break.
    Abort - the null character is interpreted as an indication that the
            file is not a text file and that processing should be aborted. */
enum class NullCharMode {Data, Break, Abort};

/*  To signal that processing should be aborted, a call back object must
    be provided whenever NullCharMode::Abort is specified.   This call back
    object is provide as an input Scalar to the kernel, and must be a pointer
    following the conventions of the Pablo TerminateAt instruction. */

class UnixLinesKernelBuilder final : public pablo::PabloKernel {
public:
    UnixLinesKernelBuilder(LLVMTypeSystemInterface & ts,
                           kernel::StreamSet * Source,
                           kernel::StreamSet * UnixLineEnds,
                           UnterminatedLineAtEOF m = UnterminatedLineAtEOF::Ignore,
                           NullCharMode nullMode = NullCharMode::Data,
                           kernel::Scalar * signalNullObject = nullptr);
protected:
    void generatePabloMethod() override;
    UnterminatedLineAtEOF mEOFmode;
    NullCharMode mNullMode;
};

void UnicodeLinesLogic(kernel::PipelineBuilder & P,
                       kernel::StreamSet * Basis,
                       kernel::StreamSet * LineEnds,
                       kernel::StreamSet * u8index,
                       UnterminatedLineAtEOF m = UnterminatedLineAtEOF::Ignore,
                       NullCharMode nullMode = NullCharMode::Data,
                       kernel::Scalar * signalNullObject = nullptr);

class NullDelimiterKernel final : public pablo::PabloKernel {
public:
    NullDelimiterKernel(LLVMTypeSystemInterface & ts,
                        kernel::StreamSet * Source,
                        kernel::StreamSet * NullDelimiters,
                        UnterminatedLineAtEOF m = UnterminatedLineAtEOF::Ignore);
protected:
    void generatePabloMethod() override;
    UnterminatedLineAtEOF mEOFmode;
};

//
// Given a stream marking end of line characters, compute a
// a stream marking start of line characters, including
// the first character position in the file.   In case
// that the input stream has multiple code units per character,
// an index stream may be provided to mark a single position
// per code unit sequence.   In this case, the reported LineStarts
// characters will be marked at positions identified in the index
// stream.
class LineStartsKernel final : public pablo::PabloKernel {
public:
    LineStartsKernel(LLVMTypeSystemInterface & ts, kernel::StreamSet * LineEnds, kernel::StreamSet * LineStarts, kernel::StreamSet * index = nullptr);
protected:
    void generatePabloMethod() override;
private:
    bool mHasIndex;
};

class LineSpansKernel final : public pablo::PabloKernel {
public:
    LineSpansKernel(LLVMTypeSystemInterface & ts, kernel::StreamSet * LineStarts, kernel::StreamSet * LineEnds, kernel::StreamSet * LineSpans);
protected:
    void generatePabloMethod() override;
};

#endif

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/toolchain/pablo_toolchain.h>
#include <pablo/pablo.h>
#include <toolchain/toolchain.h>

namespace kernel {

//
// The structure of UTF-8 text, given its code units (the 8 basis bit
// streams), with ill-formed input divided into maximal ill-formed
// subparts (the Unicode Standard, section 3.9: the longest prefixes of
// well-formed sequences, Table 3-7, and the single bytes that begin none):
// each well-formed character and each maximal ill-formed subpart, marked at
// its last byte (final) and, if requested, at its first byte (initial).
// The source must be read with a lookahead of 1 (3 with initials).
//
struct UTF8_Structure {
    pablo::PabloAST * validFinal = nullptr;
    pablo::PabloAST * errorFinal = nullptr;
    pablo::PabloAST * validInitial = nullptr;
    pablo::PabloAST * errorInitial = nullptr;
};

UTF8_Structure UTF8_structure(pablo::PabloBuilder & pb, const std::vector<pablo::PabloAST *> & source,
                              bool initials = false);

//
// Compute a stream marking UTF-8 character positions.   Each valid
// character is marked at the position of its final UTF-8 byte, and, unless
// ill-formed input is dropped (see codegen::UTF8Errors), each maximal
// ill-formed subpart at its final byte (the character U+FFFD, see
// UTF8_Decoder).
// If the optional linebreak parameter is included, also include
// any positions marked in this stream, including a possible extra
// bit just past EOF if the file is unterminated.
//
class UTF8_index : public pablo::PabloKernel {
public:
    UTF8_index(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * u8index, StreamSet * linebreak = nullptr,
               codegen::UTF8ErrorMode mode = codegen::UTF8Errors);
protected:
    void generatePabloMethod() override;
private:
    const codegen::UTF8ErrorMode mMode;
};

//
// The first byte of each UTF-8 character (see UTF8_index), as marked by
// BitMovementMode::LookAhead kernels: each well-formed character and, unless
// ill-formed input is dropped, each maximal ill-formed subpart.
//
class UTF8_InitialMask : public pablo::PabloKernel {
public:
    UTF8_InitialMask(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * initial,
                     codegen::UTF8ErrorMode mode = codegen::UTF8Errors);
protected:
    void generatePabloMethod() override;
private:
    const codegen::UTF8ErrorMode mMode;
};

//
// The maximal ill-formed subparts of UTF-8 text, marked at their last bytes
// (BitMovementMode::Advance) or their first bytes (LookAhead): with the
// first bytes, the errors at which processing stops for
// codegen::UTF8ErrorMode::Abort (see kernel::ErrorMonitorKernel).
//
class UTF8_ErrorMarks : public pablo::PabloKernel {
public:
    UTF8_ErrorMarks(LLVMTypeSystemInterface & ts, StreamSet * Source, StreamSet * errors,
                    pablo::BitMovementMode m = pablo::BitMovementMode::LookAhead);
protected:
    void generatePabloMethod() override;
private:
    const pablo::BitMovementMode mBitMovement;
};

//  Given selected UTF-8 characters identified in the marks stream, and
//  a u8index stream marking the last position of each UTF-8 character,
//  produce a stream of UTF-8 character spans, in which each position of
//  marked UTF-8 characters are identified with 1 bits.   The marks
//  stream may be marking the first byte of each UTF-8 sequence in the
//  BitMovementMode::Advance mode or the last byte of each UTF-8 sequence
//  in the BitMovementMode::LookAhead mode (default).
class U8Spans : public pablo::PabloKernel {
public:
    U8Spans(LLVMTypeSystemInterface & ts, StreamSet * marks, StreamSet * u8index, StreamSet * spans,
            pablo::BitMovementMode m = pablo::BitMovementMode::LookAhead);
protected:
    void generatePabloMethod() override;
private:
    pablo::BitMovementMode mBitMovement;
};
}


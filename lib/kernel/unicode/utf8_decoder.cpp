/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/unicode/utf8_decoder.h>
#include <re/adt/re_cc.h>
#include <pablo/pablo.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <llvm/IR/Module.h>

using namespace kernel;
using namespace pablo;
using namespace llvm;

void UTF8_Decoder::generatePabloMethod() {
    PabloBuilder main(getEntryScope());
    //  input: 8 basis bit streams
    std::vector<PabloAST *> UTF8_bit = getInputStreamSet("utf8_bit");
    //  output: 21 basis bit streams for Unicode codepoint values.
    Var * Unicode_bit[21];
    
    cc::Parabix_CC_Compiler_Builder ccc(UTF8_bit);
    Zeroes * zeroes = main.createZeroes();
    //
    // 1.  ASCII decoding
    // Initialize the low 7 bits of the Unicode basis to values from ASCII bytes.
    PabloAST * ASCII = ccc.compileCC("ASCII", re::makeByte(0x0, 0x7F), main);
    for (unsigned i = 0; i < 7; i++) {
        Unicode_bit[i] = main.createVar("U" + std::to_string(i), main.createAnd(ASCII, UTF8_bit[i]));
    }
    //
    // Initialize the remaining 14 bits to zeroes.
    for (unsigned i = 7; i < 21; i++) {
        Unicode_bit[i] = main.createVar("U" + std::to_string(i), zeroes);
    }
    //
    // Multibyte sequences are computed within Pablo if structures.
    PabloAST * u8pfx = ccc.compileCC("u8pfx", re::makeByte(0xC0, 0xFF), main);
    //
    // The first nesting level is entered when we have any UTF8 prefix byte.
    auto level1 = main.createScope();
    main.createIf(u8pfx, level1);
    //
    // Decode 2-byte UTF-8 sequences.
    PabloAST * u8pfx2 = ccc.compileCC("u8pfx2", re::makeByte(0xC0, 0xDF), level1);
    //
    // Unicode bits 6 through 10 are derived from bits 0 through 4 of the prefix byte.
    //
    PabloAST * pfx2_bit[5];
    for (unsigned i = 0; i < 5; i++) {
        pfx2_bit[i] = level1.createAnd(u8pfx2, UTF8_bit[i]);
        if (mBitMovement == BitMovementMode::Advance) {
            pfx2_bit[i] = level1.createAdvance(pfx2_bit[i], 1);
        }
        level1.createAssign(Unicode_bit[i+6], level1.createOr(Unicode_bit[i+6], pfx2_bit[i]));
    }
    // The low 6 bits of the 2nd byte of a 2-byte sequence become the
    // low 6 bits of the Unicode value.
    PabloAST * suffix = nullptr;
    PabloAST * suffix2 = nullptr;
    PabloAST * scope22 = nullptr;
    if (mBitMovement == BitMovementMode::LookAhead) {
        // bits to identify valid UTF-8 suffixes.
        PabloAST * bit7_ahead1 = level1.createLookahead(UTF8_bit[7], 1);
        PabloAST * bit6_ahead1 = level1.createLookahead(UTF8_bit[6], 1);
        suffix2 = level1.createAnd(bit7_ahead1, level1.createNot(bit6_ahead1));
        scope22 = level1.createAnd(u8pfx2, suffix2, "scope22");
    }
    if (mBitMovement == BitMovementMode::Advance) {
        suffix = level1.createAnd(UTF8_bit[7], level1.createNot(UTF8_bit[6]));
        scope22 = level1.createAnd(suffix, level1.createAdvance(u8pfx2, 1), "scope22");
    }
    PabloAST * suffix2_bit[6];
    for (unsigned i = 0; i < 6; i++) {
        suffix2_bit[i] = UTF8_bit[i];
        if (mBitMovement == BitMovementMode::LookAhead) {
            suffix2_bit[i] = level1.createAnd(suffix2, level1.createLookahead(suffix2_bit[i], 1));
        }
        level1.createAssign(Unicode_bit[i],
                            level1.createOr(Unicode_bit[i], level1.createAnd(scope22, suffix2_bit[i])));
    }
    // Three and four-byte sequences are further nested.
    PabloAST * u8pfx34 = ccc.compileCC("u8pfx34", re::makeByte(0xE0, 0xFF), level1);
    auto level2 = level1.createScope();
    level1.createIf(u8pfx34, level2);
    //
    // Decode 3-byte UTF-8 sequences.
    PabloAST * u8pfx3 = ccc.compileCC("u8pfx3", re::makeByte(0xE0, 0xEF), level2);
    //
    // Bits 12 through 15 are derived from bits 0 through 3 of the prefix byte.
    //
    PabloAST * pfx3_bit[4];
    for (unsigned i = 0; i < 4; i++) {
        pfx3_bit[i] = level2.createAnd(u8pfx3, UTF8_bit[i]);
        if (mBitMovement == BitMovementMode::Advance) {
            pfx3_bit[i] = level2.createAdvance(pfx3_bit[i], 2);
        }
        level2.createAssign(Unicode_bit[i+12], level2.createOr(Unicode_bit[i+12], pfx3_bit[i]));
    }
    // The low 6 bits of the 2nd byte of a 3-byte sequence become the
    // bit 6-11 of the Unicode value, while the low 6 bits of the 3rd byte
    // become the low 6 bits of the Unicode value.
    PabloAST * suffix3 = nullptr;
    PabloAST * scope32 = nullptr;
    PabloAST * scope33 = nullptr;
    if (mBitMovement == BitMovementMode::LookAhead) {
        // bits to identify valid UTF-8 suffixes.
        PabloAST * bit7_ahead2 = level2.createLookahead(UTF8_bit[7], 2);
        PabloAST * bit6_ahead2 = level2.createLookahead(UTF8_bit[6], 2);
        suffix3 = level2.createAnd(bit7_ahead2, level2.createNot(bit6_ahead2));
        scope32 = level2.createAnd(u8pfx3, suffix2, "scope32");
        scope33 = level2.createAnd(u8pfx3, suffix3, "scope33");
    }
    if (mBitMovement == BitMovementMode::Advance) {
        scope32 = level2.createAnd(suffix, level2.createAdvance(u8pfx3, 1), "scope32");
        scope33 = level2.createAnd(suffix, level2.createAdvance(scope32, 1), "scope33");
    }
    PabloAST * suffix3_bit[6];
    for (unsigned i = 0; i < 6; i++) {
        suffix3_bit[i] = UTF8_bit[i];
        if (mBitMovement == BitMovementMode::LookAhead) {
            suffix3_bit[i] = level2.createLookahead(suffix3_bit[i], 2);
        }
        PabloAST * sfx32bit = level2.createAnd(scope32, suffix2_bit[i]);
        PabloAST * sfx33bit = level2.createAnd(scope33, suffix3_bit[i]);
        if (mBitMovement == BitMovementMode::Advance) {
            sfx32bit = level2.createAdvance(sfx32bit, 1);
        }
        level2.createAssign(Unicode_bit[i+6], level2.createOr(Unicode_bit[i+6], sfx32bit));
        level2.createAssign(Unicode_bit[i], level2.createOr(Unicode_bit[i], sfx33bit));
    }
    //
    PabloAST * u8pfx4 = ccc.compileCC("u8pfx4", re::makeByte(0xF0, 0xFF), level2);
    // Further nest for 4-byte sequences.
    auto level3 = level2.createScope();
    level2.createIf(u8pfx4, level3);
    //
    // Bits 18 through 20 are derived from bits 0 through 2 of the prefix byte.
    //
    PabloAST * pfx4_bit[3];
    for (unsigned i = 0; i < 3; i++) {
        pfx4_bit[i] = level3.createAnd(u8pfx4, UTF8_bit[i]);
        if (mBitMovement == BitMovementMode::Advance) {
            pfx4_bit[i] = level3.createAdvance(pfx4_bit[i], 3);
        }
        level3.createAssign(Unicode_bit[i+18], level3.createOr(Unicode_bit[i+18], pfx4_bit[i]));
    }
    // The low 6 bits of the 2nd byte of a 4-byte sequence become the
    // bit 12-17 of the Unicode value, the low 6 bits of the 3rd byte
    // of a 4-byte sequence become bits 6-11 of the Unicode value,
    // while the low 6 bits of the 4th byte become the low 6 bits.
    PabloAST * suffix4 = nullptr;
    PabloAST * scope42 = nullptr;
    PabloAST * scope43 = nullptr;
    PabloAST * scope44 = nullptr;
    if (mBitMovement == BitMovementMode::Advance) {
        scope42 = level3.createAnd(suffix, level3.createAdvance(u8pfx4, 1), "scope42");
        scope43 = level3.createAnd(suffix, level3.createAdvance(scope42, 1), "scope43");
        scope44 = level3.createAnd(suffix, level3.createAdvance(scope43, 1), "scope44");
    }
    if (mBitMovement == BitMovementMode::LookAhead) {
        // bits to identify valid UTF-8 suffixes.
        PabloAST * bit7_ahead3 = level3.createLookahead(UTF8_bit[7], 3);
        PabloAST * bit6_ahead3 = level3.createLookahead(UTF8_bit[6], 3);
        suffix4 = level3.createAnd(bit7_ahead3, level3.createNot(bit6_ahead3));
        scope42 = level3.createAnd(u8pfx4, suffix2, "scope42");
        scope43 = level3.createAnd(u8pfx4, suffix3, "scope43");
        scope44 = level3.createAnd(u8pfx4, suffix4, "scope44");
    }
    PabloAST * suffix4_bit[6];
    for (unsigned i = 0; i < 6; i++) {
        suffix4_bit[i] = UTF8_bit[i];
        if (mBitMovement == BitMovementMode::LookAhead) {
            suffix4_bit[i] = level3.createLookahead(suffix4_bit[i], 3);
        }
        PabloAST * sfx42bit = level3.createAnd(scope42, suffix2_bit[i]);
        PabloAST * sfx43bit = level3.createAnd(scope43, suffix3_bit[i]);
        PabloAST * sfx44bit = level3.createAnd(scope44, suffix4_bit[i]);
        if (mBitMovement == BitMovementMode::Advance) {
            sfx42bit = level3.createAdvance(sfx42bit, 2);
            sfx43bit = level3.createAdvance(sfx43bit, 1);
        }
        level3.createAssign(Unicode_bit[i+12], level3.createOr(Unicode_bit[i+12], sfx42bit));
        level3.createAssign(Unicode_bit[i+6], level3.createOr(Unicode_bit[i+6], sfx43bit));
        level3.createAssign(Unicode_bit[i], level3.createOr(Unicode_bit[i], sfx44bit));
    }
    Var * output = getOutputStreamVar("unicode_bit");
    for (unsigned i = 0; i < 21; i++) {
        main.createAssign(main.createExtract(output, i), Unicode_bit[i]);
    }
}

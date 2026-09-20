/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "json-kernel.h"
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <pablo/bixnum/bixnum.h>
#include <pablo/pablo.h>
#include <re/unicode/re_name_resolve.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>

using namespace pablo;
using namespace kernel;

static PabloAST * sanitizeLexInput(PabloBuilder & pb, PabloAST * span, PabloAST * out) {
    return pb.createXorAnd(out, out, span);
}

void JSONStringMarker::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    cc::Parabix_CC_Compiler_Builder ccc(basis);
    Var * const strMarker = getOutputStreamVar("marker");
    Var * const strSpan = getOutputStreamVar("span");

    PabloAST * dQuotes = ccc.compileCC(re::makeByte('"'), pb);
    PabloAST * backslash = ccc.compileCC(re::makeByte('\\'), pb);

    // keeping the names as the ones in paper PGJS (Lemire)
    PabloAST * B = backslash;
    PabloAST * E = pb.createRepeat(1, pb.getInteger(0xAAAAAAAAAAAAAAAA, 64)); // constant
    PabloAST * O = pb.createRepeat(1, pb.getInteger(0x5555555555555555, 64)); // constant

    // identify 'starts' - backslashes not preceded by backslashes
    // paper does S = B & ~(B << 1)
    PabloAST * notB = pb.createNot(B);
    PabloAST * S = pb.createAnd(B, pb.createAdvance(notB, 1));
    
    // paper does S & E, but we advanced notB by 1, so it became S & O
    PabloAST * ES = pb.createAnd(S, O);
    // we don't have add, so we will have to use ScanThru on ES
    // eg.:          ES = ..............1............
    //                B = ..............1111.........
    // ScanThru(ES, EB) = ..................1........
    // This way we don't need to filter after the sum :)
    PabloAST * EC = pb.createScanThru(ES, B);
    // Checking with odd instead of even because we're one step ahead
    PabloAST * OD1 = pb.createAnd(EC, pb.createNot(O));
    // inverted even/odd
    PabloAST * OS = pb.createAnd(S, E);
    // no add/clean again
    PabloAST * OC = pb.createScanThru(OS, B);
     // Checking with odd instead of even because we're one step ahead
    PabloAST * OD2 = pb.createAnd(OC, O);
    PabloAST * OD = pb.createOr(OD1, OD2);

    // There is a bug on the quotes
    PabloAST * Q = dQuotes;
    PabloAST * QEq = pb.createAnd(Q, pb.createNot(OD));

    // Find the string spans
    PabloAST * beginDQuotes = pb.createEveryNth(QEq, pb.getInteger(2));
    PabloAST * endDQuotes = pb.createXor(QEq, beginDQuotes);
    PabloAST * inSpan = pb.createIntrinsicCall(Intrinsic::InclusiveSpan, {beginDQuotes, endDQuotes});

    pb.createAssign(pb.createExtract(strMarker, pb.getInteger(0)), QEq);
    pb.createAssign(pb.createExtract(strSpan, pb.getInteger(0)), inSpan);
}

void JSONClassifyBytes::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    cc::Parabix_CC_Compiler_Builder ccc(basis);
    PabloAST * notStrSpan = pb.createNot(getInputStreamSet("strSpan")[0]);

    Var * const lexStream = getOutputStreamVar("lexStream");

    auto makeFn = [&](auto c){ return ccc.compileCC(re::makeByte(c), pb); };

    PabloAST * digit = ccc.compileCC(re::makeByte('0', '9'), pb);
    PabloAST * ws = pb.createOr(pb.createOr3(makeFn(' '), makeFn('\n'), makeFn('\r')), makeFn('\t'));

    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::lCurly)), pb.createAnd(notStrSpan, makeFn('{')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::rCurly)), pb.createAnd(notStrSpan, makeFn('}')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::lBracket)), pb.createAnd(notStrSpan, makeFn('[')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::rBracket)), pb.createAnd(notStrSpan, makeFn(']')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::colon)), pb.createAnd(notStrSpan, makeFn(':')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::comma)), pb.createAnd(notStrSpan, makeFn(',')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::hyphen)), pb.createAnd(notStrSpan, makeFn('-')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::digit)), pb.createAnd(notStrSpan, digit));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::n)), pb.createAnd(notStrSpan, makeFn('n')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::f)), pb.createAnd(notStrSpan, makeFn('f')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::t)), pb.createAnd(notStrSpan, makeFn('t')));
    pb.createAssign(pb.createExtract(lexStream, pb.getInteger(Lex::ws)), pb.createAnd(notStrSpan, ws));
}

void JSONKeywordEndMarker::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    cc::Parabix_CC_Compiler_Builder ccc(basis);
    Var * const kwEndMarker = getOutputStreamVar("kwEndMarker");

    PabloAST * N = getInputStreamSet("lexIn")[Lex::n];
    PabloAST * T = getInputStreamSet("lexIn")[Lex::t];
    PabloAST * F = getInputStreamSet("lexIn")[Lex::f];

    // null
    PabloAST * U = ccc.compileCC(re::makeByte('u'), pb);
    PabloAST * L = ccc.compileCC(re::makeByte('l'), pb);
    // true
    PabloAST * R = ccc.compileCC(re::makeByte('r'), pb);
    PabloAST * E = ccc.compileCC(re::makeByte('e'), pb);
    // false
    PabloAST * A = ccc.compileCC(re::makeByte('a'), pb);
    PabloAST * S = ccc.compileCC(re::makeByte('s'), pb);

    Var * seqNULL = pb.createVar("null", pb.createZeroes());
    Var * seqTRUE = pb.createVar("true", pb.createZeroes());
    Var * seqFALSE = pb.createVar("false", pb.createZeroes());

    auto it = pb.createScope();
    pb.createIf(pb.createOr3(N, T, F), it);
    {
        PabloAST * advNU = it.createAnd(U, it.createAdvance(N, 1));
        PabloAST * advNUL = it.createAnd(L, it.createAdvance(advNU, 1));
        auto itNUL = it.createScope();
        it.createIf(advNUL, itNUL);
        {
            PabloAST * advNULL = itNUL.createAnd(L, itNUL.createAdvance(advNUL, 1));
            itNUL.createAssign(seqNULL, advNULL);
        }

        PabloAST * advTR = it.createAnd(R, it.createAdvance(T, 1));
        PabloAST * advTRU = it.createAnd(U, it.createAdvance(advTR, 1));
        auto itTRU = it.createScope();
        it.createIf(advTRU, itTRU);
        {
            PabloAST * advTRUE = itTRU.createAnd(E, itTRU.createAdvance(advTRU, 1));
            itTRU.createAssign(seqTRUE, advTRUE);
        }

        PabloAST * advFA = it.createAnd(A, it.createAdvance(F, 1));
        PabloAST * advFAL = it.createAnd(L, it.createAdvance(advFA, 1));
        PabloAST * advFALS = it.createAnd(S, it.createAdvance(advFAL, 1));
        auto itFALS = it.createScope();
        it.createIf(advFALS, itFALS);
        {
            PabloAST * advFALSE = itFALS.createAnd(E, itFALS.createAdvance(advFALS, 1));
            itFALS.createAssign(seqFALSE, advFALSE);
        }
    }

    pb.createAssign(pb.createExtract(kwEndMarker, pb.getInteger(KwMarker::kwNullEnd)), seqNULL);
    pb.createAssign(pb.createExtract(kwEndMarker, pb.getInteger(KwMarker::kwTrueEnd)), seqTRUE);
    pb.createAssign(pb.createExtract(kwEndMarker, pb.getInteger(KwMarker::kwFalseEnd)), seqFALSE);
}

void JSONNumberSpan::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    cc::Parabix_CC_Compiler_Builder ccc(basis);
    PabloAST * hyphen = getInputStreamSet("lexIn")[Lex::hyphen];
    PabloAST * digit = getInputStreamSet("lexIn")[Lex::digit];

    PabloAST * strSpan = getInputStreamSet("strSpan")[0];
    Var * const nbrLex = getOutputStreamVar("nbrLex");
    Var * const nbrSpan = getOutputStreamVar("nbrSpan");
    Var * const nbrErr = getOutputStreamVar("nbrErr");

    PabloAST * alleE = pb.createOr(ccc.compileCC(re::makeByte('e'), pb), ccc.compileCC(re::makeByte('E'), pb));
    PabloAST * allDot = ccc.compileCC(re::makeByte('.'), pb);
    PabloAST * allPlusMinus = pb.createOr(hyphen, ccc.compileCC(re::makeByte('+'), pb));

    PabloAST * notStrSpan = pb.createNot(strSpan);
    PabloAST * alleEAfterDigit = pb.createAnd(pb.createAdvance(digit, 1), alleE);
    PabloAST * eE = pb.createAnd(notStrSpan, alleEAfterDigit);
    PabloAST * allDotAfterDigit = pb.createAnd(pb.createAdvance(digit, 1), allDot);
    PabloAST * dot = pb.createAnd(notStrSpan, allDotAfterDigit);
    PabloAST * allPlusMinusAftereE = pb.createAnd(pb.createAdvance(eE, 1), allPlusMinus);
    PabloAST * plusMinus = pb.createAnd(notStrSpan, pb.createOr(hyphen, allPlusMinusAftereE));

    PabloAST * nondigit = pb.createNot(digit);
    PabloAST * nonDigitNorEe = pb.createAnd(nondigit, pb.createNot(eE));
    PabloAST * begin = pb.createNot(pb.createAdvance(pb.createOnes(), 1));
    PabloAST * beginIsHyphen = pb.createAnd(hyphen, begin);
    PabloAST * otherIsHyphen = pb.createAnd(hyphen, pb.createAdvance(nonDigitNorEe, 1));
    PabloAST * advHyphen = pb.createOr(beginIsHyphen, otherIsHyphen);

    PabloAST * nonDigitEePlusMinus = pb.createAnd(nonDigitNorEe, pb.createNot(plusMinus));
    PabloAST * nonDigitEePlusMinusDot = pb.createAnd(nonDigitEePlusMinus, pb.createNot(dot));
    PabloAST * advDigit = pb.createAnd(digit, pb.createAdvance(nonDigitEePlusMinusDot, 1));
    PabloAST * beginNbr = pb.createOr(advDigit, advHyphen);
    pb.createAssign(pb.createExtract(nbrLex, pb.getInteger(0)), beginNbr);

    PabloAST * errDot = pb.createAnd(pb.createAdvance(dot, 1), nondigit);
    PabloAST * errPlusMinus = pb.createAnd(pb.createAdvance(plusMinus, 1), nondigit);
    PabloAST * eENotPlusMinus = pb.createAnd(pb.createAdvance(eE, 1), pb.createNot(plusMinus));
    PabloAST * erreENotPlusMinus = pb.createAnd(eENotPlusMinus, nondigit);
    PabloAST * err = pb.createOr3(errDot, errPlusMinus, erreENotPlusMinus);
    pb.createAssign(pb.createExtract(nbrErr, pb.getInteger(0)), err);

    PabloAST * endNbr = pb.createAnd(pb.createAdvance(digit, 1), nonDigitEePlusMinusDot);
    PabloAST * finalNbr = pb.createIntrinsicCall(Intrinsic::SpanUpTo, {beginNbr, endNbr});
    pb.createAssign(pb.createExtract(nbrSpan, pb.getInteger(0)), finalNbr);
}

void JSONFindKwAndExtraneousChars::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());

    PabloAST * strSpan = getInputStreamSet("strSpan")[0];
    PabloAST * numSpan = getInputStreamSet("numSpan")[0];

    std::vector<PabloAST *> kwEndMarkers = getInputStreamSet("kwEndMarkers");
    PabloAST * ws = getInputStreamSet("lexIn")[Lex::ws];

    Var * const nbrErr = getOutputStreamVar("extraErr");
    Var * const combinedOut = getOutputStreamVar("combinedLexs");

    PabloAST * sanitizelCurly = getInputStreamSet("lexIn")[Lex::lCurly];
    PabloAST * sanitizerCurly = getInputStreamSet("lexIn")[Lex::rCurly];
    PabloAST * sanitizelBracket = getInputStreamSet("lexIn")[Lex::lBracket];
    PabloAST * sanitizerBracket = getInputStreamSet("lexIn")[Lex::rBracket];
    PabloAST * sanitizeColon = getInputStreamSet("lexIn")[Lex::colon];
    PabloAST * sanitizeComma = getInputStreamSet("lexIn")[Lex::comma];
    PabloAST * sanitizeHyphen = getInputStreamSet("lexIn")[Lex::hyphen];

    PabloAST * first3Lex = pb.createOr3(sanitizelCurly, sanitizerCurly, sanitizelBracket);
    PabloAST * last3Lex = pb.createOr3(sanitizerBracket, sanitizeColon, sanitizeComma);
    PabloAST * validLexs = pb.createOr3(first3Lex, last3Lex, sanitizeHyphen);

    PabloAST * combinedSpans = pb.createOr3(strSpan, numSpan, validLexs);

    PabloAST * nBegin = pb.createLookahead(kwEndMarkers[KwMarker::kwNullEnd], 3);
    PabloAST * tBegin = pb.createLookahead(kwEndMarkers[KwMarker::kwTrueEnd], 3);
    PabloAST * fBegin = pb.createLookahead(kwEndMarkers[KwMarker::kwFalseEnd], 4);

    PabloAST * nSpan = pb.createIntrinsicCall(
        Intrinsic::InclusiveSpan,
        { nBegin, kwEndMarkers[KwMarker::kwNullEnd] }
    );

    PabloAST * tSpan = pb.createIntrinsicCall(
        Intrinsic::InclusiveSpan,
        { tBegin, kwEndMarkers[KwMarker::kwTrueEnd] }
    );

    PabloAST * fSpan = pb.createIntrinsicCall(
        Intrinsic::InclusiveSpan,
        { fBegin, kwEndMarkers[KwMarker::kwFalseEnd] }
    );

    PabloAST * keywordSpans = pb.createOr3(nSpan, tSpan, fSpan);

    PabloAST * EOFbit = pb.createAtEOF(pb.createAdvance(pb.createOnes(), 1));
    PabloAST * extraneousChars = pb.createNot(pb.createOr(keywordSpans, combinedSpans));
    PabloAST * sanitizedErr = sanitizeLexInput(pb, pb.createOr(ws, EOFbit), extraneousChars);

    // ------------------- Validate values and terminals

    PabloAST * beforeKwMarker = pb.createOr3(
        kwEndMarkers[KwMarker::kwNullEnd],
        kwEndMarkers[KwMarker::kwTrueEnd],
        kwEndMarkers[KwMarker::kwFalseEnd]
    );
    PabloAST * kwMarker = pb.createAdvance(beforeKwMarker, 1);

    PabloAST * realNumSpan = pb.createOr(sanitizeHyphen, numSpan);
    PabloAST * notNumSpan = pb.createNot(realNumSpan);
    PabloAST * prepNumMarker = pb.createAnd(pb.createAdvance(realNumSpan, 1), notNumSpan);
    PabloAST * numMarker = sanitizeLexInput(pb, beforeKwMarker, prepNumMarker);

    PabloAST * notStrSpan = pb.createNot(strSpan);
    PabloAST * strMarker = pb.createAnd(pb.createAdvance(strSpan, 1), notStrSpan);

    PabloAST * allValues = pb.createOr3(kwMarker, numMarker, strMarker);
    PabloAST * lBrak = pb.createOr(sanitizelCurly, sanitizelBracket);
    PabloAST * rBrak = pb.createOr(sanitizerCurly, sanitizerBracket);
    PabloAST * specialSymbols = pb.createOr(sanitizeColon, sanitizeComma);
    PabloAST * allSymbols = pb.createOr3(lBrak, rBrak, specialSymbols);

    pb.createAssign(pb.createExtract(nbrErr, pb.getInteger(0)), sanitizedErr);
    pb.createAssign(pb.createExtract(combinedOut, pb.getInteger(Combined::symbols)), allSymbols);
    pb.createAssign(pb.createExtract(combinedOut, pb.getInteger(Combined::lBrak)), lBrak);
    pb.createAssign(pb.createExtract(combinedOut, pb.getInteger(Combined::rBrak)), rBrak);
    pb.createAssign(pb.createExtract(combinedOut, pb.getInteger(Combined::values)), allValues);
}

void JSONParserArr::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);

    bool genSingleBlock = mOnlyDepth > -1;
    BixNum ND = getInputStreamSet("ND");

    PabloAST * symbols = getInputStreamSet("combinedLexs")[Combined::symbols];
    PabloAST * validLBrak = getInputStreamSet("combinedLexs")[Combined::lBrak];
    PabloAST * validRBrak = getInputStreamSet("combinedLexs")[Combined::rBrak];
    PabloAST * allValues = getInputStreamSet("combinedLexs")[Combined::values];
    PabloAST * valueToken = pb.createLookahead(allValues, 1);
    PabloAST * anyToken = pb.createOr(symbols, valueToken);

    PabloAST * rCurly = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::rCurly]);
    PabloAST * lBracket = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::lBracket]);
    PabloAST * rBracket = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::rBracket]);
    PabloAST * comma = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::comma]);
    PabloAST * ws = getInputStreamSet("lexIn")[Lex::ws];
    PabloAST * str = pb.createAnd(valueToken, getInputStreamSet("strMarker")[0]);

    Var * const syntaxErr = getOutputStreamVar("syntaxErr");

    // parsing non-nesting values
    PabloAST * otherND = bnc.UGT(ND, 0);
    PabloAST * zeroND = bnc.EQ(ND, 0);
    PabloAST * EOFbit = pb.createAtEOF(pb.createAdvance(pb.createOnes(), 1));
    PabloAST * begin = pb.createNot(pb.createAdvance(pb.createOnes(), 1));
    PabloAST * valueAtZero = pb.createAnd(valueToken, zeroND);
    PabloAST * stopAtEOF = pb.createXor(pb.createOnes(), EOFbit);

    // If we have simple value at depth 0, we cannot have any other token
    PabloAST * firstValue = pb.createScanTo(begin, valueAtZero);
    PabloAST * nonNestedValue = pb.createScanTo(pb.createAdvance(firstValue, 1), anyToken);
    PabloAST * errValue = pb.createScanThru(pb.createAdvance(nonNestedValue, 1), stopAtEOF);
    
    // If we have any symbol, we cannot have any value at depth 0
    PabloAST * firstSymbol = pb.createScanTo(begin, symbols);
    PabloAST * valueAtZeroAfterSymbol = pb.createScanTo(pb.createAdvance(firstSymbol, 1), valueAtZero);
    PabloAST * errSymbol = pb.createScanThru(pb.createAdvance(valueAtZeroAfterSymbol, 1), stopAtEOF);
    
    // EOFbit is always at depth 0, otherwise we have unmatched parens
    PabloAST * errEOF = pb.createAnd(EOFbit, otherND);
    PabloAST * errSimpleValue = pb.createOr3(errValue, errSymbol, errEOF);

    // parsing arr

    // Validate that every value that is not a string is followed either by a comma or a the end validRBracket.
    // String is a special case and is checked on kernel JSONParserObj
    PabloAST * validEndValues = pb.createAnd(pb.createOr(valueToken, validRBrak), pb.createNot(zeroND));
    PabloAST * validEndValuesMinusStr = pb.createAnd(validEndValues, pb.createNot(str));
    PabloAST * afterToken = pb.createAdvance(validEndValuesMinusStr, 1);
    PabloAST * tokenNext = pb.createScanThru(afterToken, ws);
    PabloAST * notCommaRBracket = pb.createNot(pb.createOr3(comma, validRBrak, zeroND));
    PabloAST * errAfterValue = pb.createAnd(tokenNext, notCommaRBracket);

    // Every comma must be followed by a value
    PabloAST * scanAnyTkAfterComma = pb.createScanTo(pb.createAdvance(comma, 1), anyToken);
    PabloAST * validBeginValues = pb.createAnd(pb.createOr(valueToken, validLBrak), pb.createNot(zeroND));
    PabloAST * errAfterComma = pb.createAnd(scanAnyTkAfterComma, pb.createNot(validBeginValues));

    PabloAST * errElement = pb.createOr(errAfterComma, errAfterValue);
    Var * const errArray = pb.createVar("errArray", errElement);

    for (int i = mMaxDepth; i >= 1; --i) {
        PabloAST * atDepth = bnc.EQ(ND, genSingleBlock ? mOnlyDepth : i);
        PabloAST * nested = bnc.UGT(ND, genSingleBlock ? mOnlyDepth : i);
        PabloAST * arrayStart = pb.createAnd(atDepth, lBracket);

        auto it = pb.createScope();
        pb.createIf(arrayStart, it);
        {
            PabloAST * atDepthSpan = it.createAnd(atDepth, it.createNot(validRBrak));
            PabloAST * arrayEnd = it.createScanThru(arrayStart, it.createOr(nested, atDepthSpan));
            // it must not finish in rCurly
            PabloAST * errorAtEnd = it.createAnd(arrayEnd, rCurly);

            // After the lBracket we must have either a value or an rBracket.
            PabloAST * valueAtDepth = it.createAnd(atDepth, valueToken);
            PabloAST * nestedOrVTk = it.createOr(nested, valueAtDepth);
            PabloAST * nestedOrVTkRBracket = it.createOr(nestedOrVTk, rBracket);
            PabloAST * scanAnyTkAfterArrStart = it.createScanTo(it.createAdvance(arrayStart, 1), anyToken);
            PabloAST * errAfterLBracket = it.createAnd(scanAnyTkAfterArrStart, it.createNot(nestedOrVTkRBracket));

            PabloAST * errBracket = it.createOr(errorAtEnd, errAfterLBracket);
            it.createAssign(errArray, it.createOr(errArray, errBracket));
        }

        if (genSingleBlock) { break; }
    }

    PabloAST * allErrs = pb.createOr(errSimpleValue, errArray);
    pb.createAssign(pb.createExtract(syntaxErr, pb.getInteger(0)), allErrs);
}

void JSONParserObj::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);

    bool genSingleBlock = mOnlyDepth > -1;
    BixNum ND = getInputStreamSet("ND");

    PabloAST * symbols = getInputStreamSet("combinedLexs")[Combined::symbols];
    PabloAST * validRBrak = getInputStreamSet("combinedLexs")[Combined::rBrak];
    PabloAST * validLBrak = getInputStreamSet("combinedLexs")[Combined::lBrak];
    PabloAST * allValues = getInputStreamSet("combinedLexs")[Combined::values];
    PabloAST * valueToken = pb.createLookahead(allValues, 1);
    PabloAST * anyToken = pb.createOr(symbols, valueToken);

    PabloAST * lCurly = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::lCurly]);
    PabloAST * rCurly = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::rCurly]);
    PabloAST * rBracket = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::rBracket]);
    PabloAST * comma = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::comma]);
    PabloAST * colon = pb.createAnd(symbols, getInputStreamSet("lexIn")[Lex::colon]);
    PabloAST * ws = getInputStreamSet("lexIn")[Lex::ws];
    PabloAST * str = pb.createAnd(valueToken, getInputStreamSet("strMarker")[0]);
    PabloAST * zeroND = bnc.EQ(ND, 0);

    Var * const syntaxErr = getOutputStreamVar("syntaxErr");

    // parsing objects
    
    // process str as key and value
    PabloAST * validStr = pb.createAnd(str, pb.createNot(zeroND));
    PabloAST * afterTokenStr = pb.createAdvance(validStr, 1);
    PabloAST * tokenNextStr = pb.createScanThru(afterTokenStr, ws);
    PabloAST * commaColonRBrak = pb.createOr3(comma, colon, pb.createOr(validRBrak, zeroND));
    PabloAST * errAfterValue = pb.createAnd(tokenNextStr, pb.createNot(commaColonRBrak));

    // Every colon must be followed by a value
    PabloAST * validBeginValues = pb.createAnd(pb.createOr(valueToken, validLBrak), pb.createNot(zeroND));
    PabloAST * scanAnyTkAfterColon = pb.createScanTo(pb.createAdvance(colon, 1), anyToken);
    PabloAST * errAfterColon = pb.createAnd(scanAnyTkAfterColon, pb.createNot(validBeginValues));

    PabloAST * errElement = pb.createOr(errAfterColon, errAfterValue);
    Var * const errObj = pb.createVar("errObj", errElement);

    for (int i = mMaxDepth; i >= 1; --i) {
        PabloAST * atDepth = bnc.EQ(ND, genSingleBlock ? mOnlyDepth : i);
        PabloAST * nested = bnc.UGT(ND, genSingleBlock ? mOnlyDepth : i);
        PabloAST * objStart = pb.createAnd(atDepth, lCurly);

        auto it = pb.createScope();
        pb.createIf(objStart, it);
        {
            PabloAST * atDepthSpan = it.createAnd(atDepth, it.createNot(validRBrak));
            PabloAST * objEnd = it.createScanThru(objStart, it.createOr(nested, atDepthSpan));
            // it must not finish in rBracket
            PabloAST * errorAtEnd = it.createAnd(objEnd, rBracket);

            PabloAST * objSpan = it.createIntrinsicCall(
                Intrinsic::ExclusiveSpan,
                { objStart, objEnd }
            );

            // Every comma in an object must be followed by a key string
            PabloAST * strAtDepth = it.createAnd(str, atDepth);
            PabloAST * commaAtDepth = it.createAnd3(comma, atDepth, objSpan);
            PabloAST * scanAnyTkAfterComma = it.createScanTo(it.createAdvance(commaAtDepth, 1), anyToken);
            PabloAST * errAfterComma = it.createAnd(scanAnyTkAfterComma, it.createNot(strAtDepth));

            // After the lCurly we must have either a value or an rCurly.
            PabloAST * nestedOrVTk = it.createOr(nested, valueToken);
            PabloAST * nestedOrVTkRCurly = it.createOr(nestedOrVTk, rCurly);
            PabloAST * scanAnyTkAfterObjStart = it.createScanTo(it.createAdvance(objStart, 1), anyToken);
            PabloAST * errAfterLCurly = it.createAnd(scanAnyTkAfterObjStart, it.createNot(nestedOrVTkRCurly));

            PabloAST * errCurly = it.createOr3(errorAtEnd, errAfterLCurly, errAfterComma);
            it.createAssign(errObj, it.createOr(errObj, errCurly));
        }

        if (genSingleBlock) { break; }
    }

    pb.createAssign(pb.createExtract(syntaxErr, pb.getInteger(0)), errObj);
}

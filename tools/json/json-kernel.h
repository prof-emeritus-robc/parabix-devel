/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */
#pragma once

#include <pablo/pablo.h>
#include <kernel/core/kernel_builder.h>

namespace kernel {

// This enum MUST reflect the type Lex on json.pablo file
enum Lex {
    lCurly = 0,
    rCurly,
    lBracket,
    rBracket,
    colon,
    comma,
    hyphen,
    digit,
    n, // # first letter of null
    f, // # first letter of false
    t, // # first letter of true
    ws
};

enum KwMarker {
    kwNullEnd = 0,
    kwTrueEnd,
    kwFalseEnd,
};

enum Combined {
    symbols = 0,
    lBrak,
    rBrak,
    values
};

/*
    Given the JSON lex for characters backslash and double quotes,
    this kernel returns the marker of a JSON string, based on paper
    Parsing Gigabytes of JSON per Second (Daniel Lemire and Geoff Langdale)

             json: { "key1\"": value1, "key2"  : null }
    input example: ..1.....11..........1....1..........
    output marker: ..1......1..........1....1..........
      output span: ...111111............1111...........
*/
class JSONStringMarker : public pablo::PabloKernel {
public:
    JSONStringMarker(LLVMTypeSystemInterface & ts,
                     StreamSet * const basis,
                     StreamSet * strMarker, StreamSet * strSpan)
    : pablo::PabloKernel(ts,
                         "jsonStrMarker",
                         {Binding{"basis", basis}},
                         {Binding{"marker", strMarker}, Binding{"span", strSpan}}) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
};

class JSONClassifyBytes : public pablo::PabloKernel {
public:
    JSONClassifyBytes(LLVMTypeSystemInterface & ts,
                      StreamSet * const basis, StreamSet * const strSpan,
                      StreamSet * lexStream)
    : pablo::PabloKernel(ts,
                         "jsonClassifyBytes",
                         {Binding{"basis", basis}, Binding{"strSpan", strSpan}},
                         {Binding{"lexStream", lexStream}}) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
};

/*
   Marks keywords letters such as l', 'a', 's', 'r', 'u', 'e',
   joining it at the end with 'n', 't' and 'f'

            json: { "keynull": false, "keyt": true }
   input example: ......1......1..........1...1.....
          output:..................1.............1..

    Note: we do not return the beginning of the marker here because lookahead
    only works on input streams, so this will be done in a further step.
*/
class JSONKeywordEndMarker : public pablo::PabloKernel {
public:
    JSONKeywordEndMarker(LLVMTypeSystemInterface & ts,
                      StreamSet * const basis,
                      StreamSet * const lexIn,
                      StreamSet * kwMarker)
    : pablo::PabloKernel(ts,
                         "jsonKeywordMarker",
                         {
                            Binding{"basis", basis},
                            Binding{"lexIn", lexIn},
                         },
                         {
                            Binding{"kwEndMarker", kwMarker},
                         }) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
};

/*
   Finds symbols used in numbers such as 'e', 'E', '.'
   and join them at the end if they match the expression:
   \-?(0|[1-9][0-9]*)(.[0-9]+)?([Ee][+-]?[0-9]+)?
*/
class JSONNumberSpan : public pablo::PabloKernel {
public:
    JSONNumberSpan(LLVMTypeSystemInterface & ts,
                   StreamSet * const basis,
                   StreamSet * const lexIn,
                   StreamSet * const strSpan,
                   StreamSet * nbrLex, StreamSet * nbrSpan, StreamSet * nbrErr)
    : pablo::PabloKernel(ts,
                         "jsonNumberMarker",
                         {
                            Binding{"basis", basis, FixedRate(1), LookAhead(1)},
                            Binding{"lexIn", lexIn},
                            Binding{"strSpan", strSpan}
                         },
                         {
                            Binding{"nbrLex", nbrLex},
                            Binding{"nbrSpan", nbrSpan},
                            Binding{"nbrErr", nbrErr, FixedRate(), Add1()}
                        }) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
};

/*
   Marks keywords and find if there are other extra chars
   where they should not be

            json: { "keynull": false, "key{": truenull2 }
 firstLexsClning: 1..........1......1.....1.1...........1
      stringSpan: ..111111111.........111111.............
      numberSpan: ....................................1..
    kwEndMarkers: .................1.............1...1...
 output kwMarker: .............1..............1..........
 output firstLex: 1..........1......1.......1...........1
 output extraErr: ................................1......

*/
class JSONFindKwAndExtraneousChars : public pablo::PabloKernel {
public:
    JSONFindKwAndExtraneousChars(
                        LLVMTypeSystemInterface & ts,
                        StreamSet * const lexIn,
                        StreamSet * const stringSpan,
                        StreamSet * const numberSpan,
                        StreamSet * const kwEndMarkers,
                        StreamSet * const combinedLexs,
                        StreamSet * const extraErr
    )
    : pablo::PabloKernel(ts,
                         "jsonFindKwAndExtraneousChars",
                         {
                            Binding{"lexIn", lexIn},
                            Binding{"strSpan", stringSpan},
                            Binding{"numSpan", numberSpan},
                            Binding{"kwEndMarkers", kwEndMarkers, FixedRate(1), LookAhead(4)},
                         },
                         {
                            Binding{"combinedLexs", combinedLexs},
                            Binding{"extraErr", extraErr, FixedRate(), Add1()},
                         }) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
};

class JSONParserArr : public pablo::PabloKernel {
public:
    JSONParserArr(
       LLVMTypeSystemInterface & ts,
        StreamSet * const lexIn,
        StreamSet * const strMarker,
        StreamSet * const combinedLexs,
        StreamSet * const nestingDepth,
        StreamSet * const syntaxErr,
        unsigned maxDepth = 15,
        int onlyDepth = -1
    )
    : pablo::PabloKernel(ts,
                         "JSONParserArr-max=" +
                            std::to_string(maxDepth) + "-only=" + std::to_string(onlyDepth),
                         {
                            Binding{"lexIn", lexIn},
                            Binding{"strMarker", strMarker},
                            Binding{"combinedLexs", combinedLexs, FixedRate(1), LookAhead(1)},
                            Binding{"ND", nestingDepth}
                         },
                         {
                            Binding{"syntaxErr", syntaxErr, FixedRate(), Add1()}
                         }),
    mMaxDepth(maxDepth), mOnlyDepth(onlyDepth) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
    unsigned mMaxDepth;
    int mOnlyDepth;
};

class JSONParserObj: public pablo::PabloKernel {
public:
    JSONParserObj(
        LLVMTypeSystemInterface & ts,
        StreamSet * const lexIn,
        StreamSet * const strMarker,
        StreamSet * const combinedLexs,
        StreamSet * const nestingDepth,
        StreamSet * const syntaxErr,
        unsigned maxDepth = 15,
        int onlyDepth = -1
    )
    : pablo::PabloKernel(ts,
                         "JSONParserObj-max=" +
                            std::to_string(maxDepth) + "-only=" + std::to_string(onlyDepth),
                         {
                            Binding{"lexIn", lexIn},
                            Binding{"strMarker", strMarker},
                            Binding{"combinedLexs", combinedLexs, FixedRate(1), LookAhead(1)},
                            Binding{"ND", nestingDepth}
                         },
                         {
                            Binding{"syntaxErr", syntaxErr, FixedRate(), Add1()}
                         }),
    mMaxDepth(maxDepth), mOnlyDepth(onlyDepth) {}
    bool isCachable() const override { return true; }
    bool hasSignature() const override { return false; }
protected:
    void generatePabloMethod() override;
    unsigned mMaxDepth;
    int mOnlyDepth;
};

}


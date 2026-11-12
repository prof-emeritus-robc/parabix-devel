#include "json2csv.h"

#include <cassert>
#include <cstdlib>
#include <string>
#include <sstream>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/ADT/SmallVector.h>
#include <map>
#include <llvm/Support/raw_ostream.h>
#include <toolchain/toolio.h>

/*
 * <Json2CSV> ::= <Array>
 *
 * <Array> ::= '[' ']'
 *           | '[' <Elements> ']'
 * 
 * <Elements> ::= <Object>
 *              | <Object> ',' <Elements>
 *
 * <Object> ::= '{' '}'
 *            | '{' <Members> '}'
 * 
 * <Members> ::= <Pair>
 *             | <Pair> ',' <Members>
 * 
 * <Pair> ::= String ':' String
 * 
 */

enum JSONState {
    JInit = 0,
    JKStrBegin,
    JKStrEnd,
    JVStrBegin,
    JVStrEnd,
    JObjInit,
    JObjColon,
    JArrInit,
    JNextComma,
    JDone,
    JReadKStr,
    JReadVStr
};

static llvm::SmallVector<const u_int8_t *, 32> stack{};
static JSONState currentState = JInit;
static std::map<std::string, std::map<int, std::string>> info;
static uint64_t currentIndex = 0;
static llvm::SmallVector<u_int8_t, 32> keyVector{};
static llvm::SmallVector<u_int8_t, 32> valueVector{};

static void json2csv_reportError(const std::string str) {
    fprintf(stderr, "%s\n", str.c_str());
    exit(-1);
}

static void json2csv_resetVector() {
    if (currentState == JKStrBegin) {
        keyVector.clear();
    } else if (currentState == JVStrBegin) {
        valueVector.clear();
    }
}

static void json2csv_resetAll() {
    keyVector.clear();
    valueVector.clear();
    info.clear();
    currentIndex = 0;
}

static void json2csv_printCSV() {
    llvm::raw_ostream & out = toolio::out();
    std::map<std::string, std::map<int, std::string>>::iterator it;
    std::map<std::string, std::map<int, std::string>>::iterator nextIt;
    for (it = info.begin(); it != info.end(); it++) {
        nextIt = it;
        const char * sep = ++nextIt == info.end() ? "" : ",";
        out << it->first << sep;
    }
    for (uint64_t idx = 0; idx < currentIndex + 1; idx++) {
        out << "\n";
        for (it = info.begin(); it != info.end(); it++) {
            nextIt = it;
            const char * sep = ++nextIt == info.end() ? "" : ",";
            std::map<int, std::string> innerMap = it->second;
            std::map<int, std::string>::const_iterator pos = innerMap.find(idx);
            if (pos != innerMap.end()) {
                out << pos->second;
            }
            out << sep;
        }
    }
}

static void json2csv_saveKeyValuePair() {
     std::string key = std::string(keyVector.begin(), keyVector.end());
     std::string value = std::string(valueVector.begin(), valueVector.end());
     std::map<std::string, std::map<int, std::string>>::const_iterator pos = info.find(key);

     if (pos == info.end()) {
         std::map<int, std::string> innerMap{};
         info[key][currentIndex] = value;
     } else {
         std::map<int, std::string> innerMap = pos->second;
         info[key][currentIndex] = value;
     }
}

static ptrdiff_t json2csv_getColumn(const uint8_t * ptr, const uint8_t * lineBegin) {
    if (lineBegin == nullptr) {
        return 0;
    }
    ptrdiff_t column = ptr - lineBegin;
    assert (column >= 0);
    return column;
}

static std::string json2csv_getLineAndColumnInfo(const std::string str, const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum) {
    ptrdiff_t column = json2csv_getColumn(ptr, lineBegin);
    std::stringstream ss;
    ss << str << " at line " << lineNum << " column " << column << " starting in:\n\n" << ptr;
    return ss.str();
}

static void json2csv_popAndFindNewState() {
    assert(!stack.empty());
    stack.pop_back();

    if (stack.empty()) {
        currentState = JDone;
        return;
    }
 
    const uint8_t last = *stack.back();
    if (last == '[') {
        currentState = JNextComma;
    } else {
        json2csv_reportError("The stack has an unknown char");
    }
}

static void json2csv_parseArr(const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (*ptr == '[') {
        currentState = JArrInit;
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing array", ptr, lineBegin, lineNum));
    }
    stack.push_back(ptr);
}

static bool json2csv_parseObj(const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (*ptr == '{') {
        currentState = JObjInit;
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing object", ptr, lineBegin, lineNum));
        return false;
    }
    stack.push_back(ptr);
    return true;
}

static void json2csv_parseStrOrPop(bool popAllowed, const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (*ptr == '"') {
        currentState = JKStrBegin;
    } else if (*ptr == '}' && popAllowed) {
        json2csv_popAndFindNewState();
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing object", ptr, lineBegin, lineNum));
    }
}

static bool json2csv_parseStrValue(const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (*ptr == '"') {
        currentState = JVStrBegin;
        return true;
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing string value", ptr, lineBegin, lineNum));
    }
    return false;
}

static void json2csv_parseObjOrPop(bool popAllowed, const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (!json2csv_parseObj(ptr, lineBegin, lineNum, position)) {
        if (*ptr == ']' && popAllowed) {
            currentState = JDone;
        } else {
            json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing array", ptr, lineBegin, lineNum));
        }
    }
}

static void json2csv_parseStr(const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (*ptr != '"' && (currentState == JKStrBegin || currentState == JReadKStr)) {
        keyVector.push_back(*ptr);
	currentState = JReadKStr;
    } else if (*ptr != '"' && (currentState == JVStrBegin || currentState == JReadVStr)) {
        valueVector.push_back(*ptr);
	currentState = JReadVStr;
    } else if (*ptr == '"' && (currentState == JKStrBegin || currentState == JReadKStr)) {
        currentState = JKStrEnd;
    } else if (*ptr == '"' && (currentState == JVStrBegin || currentState == JReadVStr)) {
        currentState = JVStrEnd;
        json2csv_saveKeyValuePair();
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing string", ptr, lineBegin, lineNum));
    }
}

static void json2csv_parseColon(const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (*ptr == ':') {
        currentState = JObjColon;
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing object", ptr, lineBegin, lineNum));
    }
}

static void json2csv_parseCommaOrPop(const uint8_t * ptr, const uint8_t * lineBegin, uint64_t lineNum, uint64_t position) {
    if (stack.empty()) {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Stack is empty", ptr, lineBegin, lineNum));
        return;
    }

    const uint8_t last = *stack.back();
    if (*ptr == ',' && last == '[') {
        currentIndex += 1;
        currentState = JArrInit;
    } else if (*ptr == ',' && last == '{') {
        currentState = JObjInit;
    } else if (*ptr == '}' || *ptr == ']') {
        json2csv_popAndFindNewState();
    } else {
        json2csv_reportError(json2csv_getLineAndColumnInfo("Error parsing object", ptr, lineBegin, lineNum));
    }
}

void json2csv_doneCallback() {
    if (stack.empty() || currentState == JDone) {
        json2csv_printCSV();
        currentState = JDone;
        return;
    }
    json2csv_reportError("Found EOF but the JSON is missing elements");
}

void json2csv_validateObjectsAndArrays(const uint8_t * ptr, const uint8_t * lineBegin, const uint8_t * /*lineEnd*/, uint64_t lineNum, uint64_t position) {
    if (currentState == JInit) {
        json2csv_resetAll();
        json2csv_parseArr(ptr, lineBegin, lineNum, position);
    } else if (currentState == JObjInit) {
        json2csv_parseStrOrPop(true, ptr, lineBegin, lineNum, position);
    } else if (currentState == JArrInit) {
        json2csv_parseObjOrPop(true, ptr, lineBegin, lineNum, position);
    } else if (currentState == JKStrBegin || currentState == JVStrBegin) {
        json2csv_resetVector();
        json2csv_parseStr(ptr, lineBegin, lineNum, position);
    } else if (currentState == JReadKStr || currentState == JReadVStr) {
        json2csv_parseStr(ptr, lineBegin, lineNum, position);
    } else if (currentState == JKStrEnd) {
        json2csv_parseColon(ptr, lineBegin, lineNum, position);
    } else if (currentState == JObjColon) {
        json2csv_parseStrValue(ptr, lineBegin, lineNum, position);
    } else if (currentState == JVStrEnd || currentState == JNextComma) {
        json2csv_parseCommaOrPop(ptr, lineBegin, lineNum, position);
    } else if (currentState == JDone && *ptr != ']') {
        json2csv_reportError(json2csv_getLineAndColumnInfo("JSON has been already processed", ptr, lineBegin, lineNum));
    }
}

void json2csv_simpleValidateObjectsAndArrays(const uint8_t * ptr) {
    json2csv_validateObjectsAndArrays(ptr, nullptr, nullptr, 0, 0);
}

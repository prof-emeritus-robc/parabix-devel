/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules_parser.h>
#include <re/adt/adt.h>
#include <algorithm>
#include <set>
#include <re/adt/re_utility.h>
#include <ldml/transform_rules_printer.h>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

constexpr char32_t RIGHT_ARROW = 0x2192;       // →
constexpr char32_t LEFT_ARROW = 0x2190;        // ←
constexpr char32_t LEFT_RIGHT_ARROW = 0x2194;  // ↔
constexpr char32_t NONE = 0xFFFFFFFF;

inline bool isPatternWhiteSpace(const char32_t c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85
        || c == 0x200E || c == 0x200F || c == 0x2028 || c == 0x2029;
}

inline bool isASCIIAlpha(const char32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool isASCIIDigit(const char32_t c) {
    return c >= '0' && c <= '9';
}

inline bool isArrow(const char32_t c) {
    return c == RIGHT_ARROW || c == LEFT_ARROW || c == LEFT_RIGHT_ARROW;
}

// Characters that may appear unquoted as literal text: ASCII letters and
// digits and non-ASCII characters other than the arrows and white space.
// All other ASCII characters are reserved for the rule syntax.
inline bool isLiteralChar(const char32_t c) {
    if (c < 0x80) return isASCIIAlpha(c) || isASCIIDigit(c);
    return !isArrow(c) && !isPatternWhiteSpace(c) && c <= 0x10FFFF;
}

// An approximation to the Unicode identifier syntax of UAX #31 for
// variable names.
inline bool isIdStart(const char32_t c) {
    return isASCIIAlpha(c) || (c >= 0x80 && isLiteralChar(c));
}

inline bool isIdContinue(const char32_t c) {
    return isIdStart(c) || isASCIIDigit(c) || c == '_';
}

inline bool isHexDigit(const char32_t c) {
    return isASCIIDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline unsigned hexValue(const char32_t c) {
    if (isASCIIDigit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

void appendUTF8(std::string & s, const char32_t cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string toUTF8(const std::u32string & s) {
    std::string result;
    for (const char32_t c : s) appendUTF8(result, c);
    return result;
}

[[noreturn]] void UTF8Error(const std::string & s, const size_t pos) {
    size_t line = 1;
    size_t column = 1;
    for (size_t i = 0; i < pos; i++) {
        if (s[i] == '\n') {
            line++;
            column = 1;
        } else if ((s[i] & 0xC0) != 0x80) {
            column++;
        }
    }
    throw TransformRuleParseError("Invalid UTF-8 encoding", line, column);
}

std::u32string decodeUTF8(const std::string & s) {
    std::u32string result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char b = s[i];
        unsigned length;
        char32_t cp;
        if (b < 0x80) {
            length = 1; cp = b;
        } else if (b >= 0xC2 && b <= 0xDF) {
            length = 2; cp = b & 0x1F;
        } else if (b >= 0xE0 && b <= 0xEF) {
            length = 3; cp = b & 0x0F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            length = 4; cp = b & 0x07;
        } else {
            UTF8Error(s, i);
        }
        if (i + length > s.size()) UTF8Error(s, i);
        for (unsigned j = 1; j < length; j++) {
            const unsigned char sfx = s[i + j];
            if ((sfx & 0xC0) != 0x80) UTF8Error(s, i);
            cp = (cp << 6) | (sfx & 0x3F);
        }
        // Reject overlong forms, surrogates and out of range values.
        if ((length == 3 && cp < 0x800) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF))
                || (cp >= 0xD800 && cp <= 0xDFFF)) {
            UTF8Error(s, i);
        }
        result.push_back(cp);
        i += length;
    }
    return result;
}

std::string canonicalizePropertyName(const std::u32string & s) {
    std::string result;
    for (const char32_t c : s) {
        if (c == ' ' || c == '_' || c == '-' || isPatternWhiteSpace(c)) continue;
        if (c >= 'A' && c <= 'Z') appendUTF8(result, c - 'A' + 'a');
        else appendUTF8(result, c);
    }
    return result;
}

std::u32string trim(const std::u32string & s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isPatternWhiteSpace(s[b])) b++;
    while (e > b && isPatternWhiteSpace(s[e - 1])) e--;
    return s.substr(b, e - b);
}

//  A UnicodeSet under construction: the characters (and strings) of
//  the set, and whether the set includes the text boundary "$".
struct SetValue {
    RE * chars;
    bool boundary;
};

RE * toSetRE(const SetValue & v) {
    if (v.boundary) return makeAlt({v.chars, makeTextBoundary()});
    return v.chars;
}

SetValue fromSetRE(RE * set) {
    RE * def = set;
    if (Name * n = dyn_cast<Name>(set)) {
        if (n->getDefinition() == nullptr) return SetValue{set, false};
        def = n->getDefinition();
    }
    if (!includesTextBoundary(def)) return SetValue{set, false};
    std::vector<RE *> members;
    for (RE * a : *cast<Alt>(def)) {
        if (!isBoundary(a)) members.push_back(a);
    }
    RE * chars = members.empty() ? makeCC() : makeAlt(members.begin(), members.end());
    return SetValue{chars, true};
}

RE * setUnion(std::vector<RE *> & items) {
    if (items.empty()) return makeCC();
    return makeAlt(items.begin(), items.end());
}

RE * setDifference(RE * a, RE * b) {
    if (isa<CC>(a) && isa<CC>(b)) return subtractCC(cast<CC>(a), cast<CC>(b));
    return makeDiff(a, b);
}

RE * setIntersection(RE * a, RE * b) {
    return makeIntersect(a, b);
}

RE * setComplement(RE * a) {
    return makeComplement(a);
}

// Is the RE a set usable as an operand of the set operators?
bool isSetOperand(const RE * re) {
    if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() && isSetOperand(n->getDefinition());
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * a : *alt) {
            if (!isSetOperand(a) && !isBoundary(a) && !isa<Seq>(a)) return false;
        }
        return true;
    }
    return isa<CC>(re) || isa<PropertyExpression>(re) || isa<Diff>(re) || isa<Intersect>(re) || isa<Any>(re);
}

class RuleTextParser {
public:
    RuleTextParser(const std::u32string & text, std::map<std::string, Name *> & variables,
                   std::set<std::string> * failedVariables = nullptr)
    : mText(text), mPos(0), mVariables(variables), mFailedVariables(failedVariables) {}

    // Parse the next rule, or return nullptr at the end of the text.
    Rule * parseRule();

    // Recovery after an error in the rule starting at the given position:
    // continue after the ';' ending it.
    void skipRule(const size_t ruleStart);
    size_t position() const {return mPos;}

    // Parse a complete text consisting of a single UnicodeSet.
    RE * parseCompleteSet();

private:
    enum class SeqMode {Variable, Pattern, FunctionArgument};

    Rule * parseTransformOrFilterRule();
    Rule * parseVariableDefinition(const std::string & name);
    Rule * parseConversionRule();
    Direction parseOperator();
    RuleSide * parseSide();
    void parseSequence(std::vector<RE *> & items, const SeqMode mode);
    RE * parseSegment();
    RE * parseFunctionCall();
    RE * parseReference();
    RE * parseVariableReference();
    std::string parseVariableName();
    std::pair<RE *, TransformID> parseTransformPart();

    bool atSetStart() const;
    RE * parseSet();
    RE * parseBracketSet();
    RE * parsePropertySet(const char32_t closer, bool negated);
    RE * parseSetString();
    std::u32string parseEscape();
    char32_t parseEscapedChar();
    char32_t parseHex(unsigned minDigits, unsigned maxDigits);
    void parseQuotedString(std::u32string & s);

    bool more() const {return mPos < mText.size();}
    char32_t cur() const {return more() ? mText[mPos] : NONE;}
    char32_t peek(size_t k = 1) const {return mPos + k < mText.size() ? mText[mPos + k] : NONE;}
    bool at(const char32_t c) const {return cur() == c;}
    bool at(const char * s) const {
        for (size_t i = 0; s[i]; i++) {
            if (mPos + i >= mText.size() || mText[mPos + i] != static_cast<char32_t>(s[i])) return false;
        }
        return true;
    }
    bool accept(const char32_t c) {
        if (at(c)) {mPos++; return true;}
        return false;
    }
    bool accept(const char * s) {
        if (at(s)) {mPos += std::char_traits<char>::length(s); return true;}
        return false;
    }
    void require(const char32_t c, const char * what) {
        if (!accept(c)) {
            std::string msg = std::string("Expected ") + what;
            if (more()) {
                msg += " but found '";
                appendUTF8(msg, cur());
                msg += "'";
            } else {
                msg += " but found end of rules";
            }
            ParseFailure(msg);
        }
    }
    // Skip white space and comments.
    void skipWhiteSpace() {
        while (more()) {
            if (isPatternWhiteSpace(cur())) {
                mPos++;
            } else if (at('#')) {
                while (more() && !at('\n') && !at('\r') && !at(0x2028) && !at(0x2029)) mPos++;
            } else {
                return;
            }
        }
    }
    // Within UnicodeSets, white space is ignored but # is not a comment.
    void skipSetWhiteSpace() {
        while (more() && isPatternWhiteSpace(cur())) mPos++;
    }

    [[noreturn]] void ParseFailure(const std::string & msg) const {
        size_t line = 1;
        size_t column = 1;
        for (size_t i = 0; i < mPos && i < mText.size(); i++) {
            if (mText[i] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        throw TransformRuleParseError(msg, line, column);
    }

    const std::u32string & mText;
    size_t mPos;
    std::map<std::string, Name *> & mVariables;
    std::set<std::string> * const mFailedVariables;

    // State for the conversion rule side being parsed.
    std::vector<Capture *> mCaptures;
    const std::vector<Capture *> * mOtherSideCaptures = nullptr;
    bool mUnresolvedReferences = false;
    unsigned mReferenceInstance = 0;
    bool mEndAnchorSeen = false;
};

Rule * RuleTextParser::parseRule() {
    skipWhiteSpace();
    if (!more()) return nullptr;
    Rule * rule = nullptr;
    if (accept("::")) {
        rule = parseTransformOrFilterRule();
    } else if (at('$') && isIdStart(peek())) {
        const size_t start = mPos;
        mPos++;
        const std::string name = parseVariableName();
        skipWhiteSpace();
        if (accept('=')) {
            try {
                rule = parseVariableDefinition(name);
            } catch (const TransformRuleParseError &) {
                if (mFailedVariables && mVariables.count(name) == 0) mFailedVariables->insert(name);
                throw;
            }
        } else {
            mPos = start;
            rule = parseConversionRule();
        }
    } else {
        rule = parseConversionRule();
    }
    skipWhiteSpace();
    if (more()) require(';', "';' at end of rule");
    return rule;
}

// The end of a rule is the first ';' that is not quoted, escaped, within a
// set or within a comment.  If there is none (as for an unterminated quote
// or set), the first unescaped ';' after the error position is taken.
void RuleTextParser::skipRule(const size_t ruleStart) {
    const size_t errorPos = mPos;
    unsigned setDepth = 0;
    char32_t quote = 0;
    for (size_t i = ruleStart; i < mText.size(); i++) {
        const char32_t c = mText[i];
        if (c == '\\') {
            i++;
        } else if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '[') {
            setDepth++;
        } else if (c == ']') {
            if (setDepth) setDepth--;
        } else if (setDepth == 0) {
            if (c == ';') {
                mPos = i + 1;
                return;
            }
            if (c == '#') {
                while (i + 1 < mText.size()) {
                    const char32_t n = mText[i + 1];
                    if (n == '\n' || n == '\r' || n == 0x2028 || n == 0x2029) break;
                    i++;
                }
            }
        }
    }
    for (size_t i = std::max(errorPos, ruleStart); i < mText.size(); i++) {
        if (mText[i] == '\\') {
            i++;
        } else if (mText[i] == ';') {
            mPos = i + 1;
            return;
        }
    }
    mPos = mText.size();
}

std::pair<RE *, TransformID> RuleTextParser::parseTransformPart() {
    skipWhiteSpace();
    RE * filter = nullptr;
    if (atSetStart()) {
        filter = parseSet();
        skipWhiteSpace();
    }
    std::u32string id;
    while (more() && !isPatternWhiteSpace(cur()) && !at('(') && !at(')') && !at(';') && !at('#')) {
        const char32_t c = cur();
        if (!(isLiteralChar(c) || c == '-' || c == '_' || c == '/' || c == '.')) {
            std::string msg = "Invalid character '";
            appendUTF8(msg, c);
            ParseFailure(msg + "' in transform ID");
        }
        id.push_back(c);
        mPos++;
    }
    skipWhiteSpace();
    return std::make_pair(filter, id.empty() ? TransformID() : TransformID(toUTF8(id)));
}

//  :: [set] ;   :: ([set]) ;   :: [set]? ID ( [set]? ID? )? ;   :: ( [set]? ID ) ;
Rule * RuleTextParser::parseTransformOrFilterRule() {
    skipWhiteSpace();
    if (accept('(')) {
        auto backward = parseTransformPart();
        require(')', "')'");
        if (backward.second.empty()) {
            if (backward.first == nullptr) {
                ParseFailure("Expected a transform ID or UnicodeSet");
            }
            return makeInverseFilterRule(backward.first);
        }
        return makeTransformRule(TransformID(), nullptr, backward.second, backward.first);
    }
    auto forward = parseTransformPart();
    if (accept('(')) {
        if (forward.second.empty()) {
            ParseFailure("Expected a transform ID before '('");
        }
        auto backward = parseTransformPart();
        require(')', "')'");
        return makeTransformRule(forward.second, forward.first, backward.second, backward.first);
    }
    if (forward.second.empty()) {
        if (forward.first == nullptr) {
            ParseFailure("Expected a transform ID or UnicodeSet");
        }
        return makeFilterRule(forward.first);
    }
    return makeTransformRule(forward.second, forward.first);
}

std::string RuleTextParser::parseVariableName() {
    std::u32string name;
    while (more() && isIdContinue(cur())) {
        name.push_back(cur());
        mPos++;
    }
    return toUTF8(name);
}

Rule * RuleTextParser::parseVariableDefinition(const std::string & name) {
    if (mVariables.count(name) != 0) {
        ParseFailure("Variable $" + name + " is already defined");
    }
    std::vector<RE *> items;
    parseSequence(items, SeqMode::Variable);
    skipWhiteSpace();
    if (more() && !at(';')) {
        std::string msg = "Unexpected '";
        appendUTF8(msg, cur());
        ParseFailure(msg + "' in variable definition");
    }
    Name * variable = makeName(name, makeSeq(items.begin(), items.end()));
    mVariables.emplace(name, variable);
    return VariableDefinitionRule::Create(variable);
}

Direction RuleTextParser::parseOperator() {
    skipWhiteSpace();
    if (accept(RIGHT_ARROW) || accept('>')) return Direction::Forward;
    if (accept(LEFT_RIGHT_ARROW) || accept("<>")) return Direction::Both;
    if (accept(LEFT_ARROW) || accept('<')) return Direction::Backward;
    if (!more()) ParseFailure("Expected a conversion operator (→, ←, ↔) but found end of rules");
    std::string msg = "Expected a conversion operator (→, ←, ↔) but found '";
    appendUTF8(msg, cur());
    ParseFailure(msg + "'");
}

//  Segment references ($1) on each side refer to the segments of the other
//  side.   The left side is parsed first without resolving its references,
//  then the right side, resolving references to the left side segments.
//  If the left side has references, it is then reparsed resolving its
//  references to the right side segments.
Rule * RuleTextParser::parseConversionRule() {
    const size_t start = mPos;
    mCaptures.clear();
    mOtherSideCaptures = nullptr;
    mUnresolvedReferences = false;
    RuleSide * left = parseSide();
    const bool leftHasReferences = mUnresolvedReferences;
    std::vector<Capture *> leftCaptures = mCaptures;
    const Direction d = parseOperator();
    mCaptures.clear();
    mOtherSideCaptures = &leftCaptures;
    RuleSide * right = parseSide();
    if (leftHasReferences) {
        const size_t end = mPos;
        std::vector<Capture *> rightCaptures = mCaptures;
        mPos = start;
        mCaptures.clear();
        mOtherSideCaptures = &rightCaptures;
        left = parseSide();
        mPos = end;
    }
    mOtherSideCaptures = nullptr;
    return makeConversionRule(left, d, right);
}

//  before_context { completed_result | result_to_revisit } after_context
RuleSide * RuleTextParser::parseSide() {
    enum Part {Before = 0, Text = 1, After = 2};
    std::vector<RE *> parts[3];
    bool present[3] = {false, true, false};
    Part current = Text;          // Until '{' is seen, the items are text.
    bool openBrace = false;
    bool closeBrace = false;
    size_t cursorIndex = 0;
    bool cursor = false;
    mEndAnchorSeen = false;
    skipWhiteSpace();
    if (accept('^')) {
        parts[Text].push_back(makeStart());
    }
    for (;;) {
        parseSequence(parts[current], SeqMode::Pattern);
        skipWhiteSpace();
        if (accept('{')) {
            if (openBrace || closeBrace || cursor) ParseFailure("Misplaced '{'");
            // The items so far are the before context.
            parts[Before] = std::move(parts[Text]);
            parts[Text].clear();
            present[Before] = true;
            openBrace = true;
        } else if (accept('}')) {
            if (closeBrace) ParseFailure("Misplaced '}'");
            closeBrace = true;
            present[After] = true;
            current = After;
        } else if (accept('|')) {
            if (cursor || current != Text) ParseFailure("Misplaced cursor '|'");
            cursor = true;
            cursorIndex = parts[Text].size();
        } else if (accept('@')) {
            if (current != Text) ParseFailure("Misplaced '@'");
            parts[Text].push_back(nullptr);  // placeholder for '@'
        } else {
            break;
        }
    }
    // Validate and remove the '@' placeholders.
    std::vector<RE *> & text = parts[Text];
    int cursorOffset = 0;
    size_t completedEnd = cursorIndex;
    size_t revisitStart = cursorIndex;
    unsigned leading = 0;
    while (cursorIndex + leading < text.size() && text[cursorIndex + leading] == nullptr) leading++;
    unsigned trailing = 0;
    while (trailing < cursorIndex && text[cursorIndex - trailing - 1] == nullptr) trailing++;
    if (leading > 0) {
        if (!cursor || cursorIndex != 0) ParseFailure("Misplaced '@': fillers must follow a cursor at the start of the result");
        cursorOffset = -static_cast<int>(leading);
        revisitStart += leading;
    }
    if (trailing > 0) {
        if (!cursor || cursorIndex != text.size()) ParseFailure("Misplaced '@': fillers must precede a cursor at the end of the result");
        cursorOffset = static_cast<int>(trailing);
        completedEnd -= trailing;
    }
    if (!cursor) {
        completedEnd = revisitStart = text.size();
    }
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == nullptr && (i < completedEnd || i >= revisitStart)) {
            ParseFailure("Misplaced '@'");
        }
    }
    auto seq = [](std::vector<RE *> & items, size_t b, size_t e) -> RE * {
        return makeSeq(items.begin() + b, items.begin() + e);
    };
    // The text boundary is the start of the text in the before context, and
    // the end of the text in the after context.  Segments rebuilt by the
    // resolution replace the originals for the references of the other side.
    std::map<Capture *, Capture *> rebuilt;
    auto context = [&](Part p) -> RE * {
        if (!present[p] || parts[p].empty()) return nullptr;
        return resolveTextBoundary(seq(parts[p], 0, parts[p].size()),
                                   p == Before ? BoundaryResolution::Start : BoundaryResolution::End, &rebuilt);
    };
    // Within the text, the boundary may be matched only by its first item (at
    // the start of the text) or its last item (at the end of the text).
    size_t firstItem = 0;
    while (firstItem < text.size() && text[firstItem] == nullptr) firstItem++;
    size_t lastItem = text.size();
    while (lastItem > firstItem && text[lastItem - 1] == nullptr) lastItem--;
    for (size_t i = firstItem; i < lastItem; i++) {
        if (text[i] == nullptr) continue;
        const BoundaryResolution r = (i + 1 == lastItem) ? BoundaryResolution::End
                                   : (i == firstItem) ? BoundaryResolution::Start : BoundaryResolution::None;
        text[i] = resolveTextBoundary(text[i], r, &rebuilt);
    }
    RE * completed = seq(text, 0, completedEnd);
    RE * revisit = seq(text, revisitStart, text.size());
    RE * const before = context(Before);
    RE * const after = context(After);
    for (Capture * & c : mCaptures) {
        auto f = rebuilt.find(c);
        if (f != rebuilt.end()) c = f->second;
    }
    return RuleSide::Create(before, completed, cursor, revisit, cursorOffset, after);
}

//  Parse a sequence of items (literals, quoted strings, sets, variables,
//  segments, references, function calls, quantifiers, the $ anchor),
//  stopping at any other syntax character.
void RuleTextParser::parseSequence(std::vector<RE *> & items, const SeqMode mode) {
    // The index of the item to which a quantifier may apply, or -1.
    int quantifiable = -1;
    for (;;) {
        skipWhiteSpace();
        if (!more()) return;
        const char32_t c = cur();
        if (c == '*' || c == '+' || c == '?') {
            if (quantifiable < 0) ParseFailure("Quantifier without an operand");
            mPos++;
            const int lb = (c == '+') ? 1 : 0;
            const int ub = (c == '?') ? 1 : Rep::UNBOUNDED_REP;
            // Quantifiers may be stacked, as in x*?
            items[quantifiable] = makeRep(items[quantifiable], lb, ub);
            continue;
        }
        if (c == ';' || c == '{' || c == '}' || c == '|' || c == '@' || c == '=' || c == '<' || c == '>' || c == ')' || isArrow(c)) {
            return;
        }
        if (mEndAnchorSeen) {
            ParseFailure("The '$' anchor must be at the end of the pattern");
        }
        RE * item = nullptr;
        bool canQuantify = true;
        if (c == '\'' || c == '"') {
            std::u32string s;
            parseQuotedString(s);
            if (s.empty()) continue;
            std::vector<RE *> chars;
            for (const char32_t q : s) chars.push_back(makeCC(q));
            item = (chars.size() == 1) ? chars[0] : makeSeq(chars.begin(), chars.end());
        } else if (atSetStart()) {
            item = parseSet();
        } else if (c == '\\') {
            mPos++;
            // A multiple codepoint escape \x{61 62} is equivalent to \x{61}\x{62}.
            const std::u32string chars = parseEscape();
            for (size_t i = 0; i + 1 < chars.size(); i++) items.push_back(makeCC(chars[i]));
            item = makeCC(chars.back());
        } else if (c == '.') {
            mPos++;
            item = makeDotSet();
        } else if (c == '$') {
            if (isIdStart(peek())) {
                mPos++;
                item = parseVariableReference();
            } else if (peek() >= '1' && peek() <= '9') {
                if (mode == SeqMode::Variable) ParseFailure("Segment references are not allowed in variable definitions");
                mPos++;
                item = parseReference();
                canQuantify = false;
            } else {
                if (mode != SeqMode::Pattern) ParseFailure("Misplaced '$' anchor");
                mPos++;
                item = makeEnd();
                canQuantify = false;
                mEndAnchorSeen = true;
            }
        } else if (c == '(') {
            if (mode == SeqMode::Variable) ParseFailure("Segments are not allowed in variable definitions");
            mPos++;
            item = parseSegment();
        } else if (c == '&') {
            if (mode == SeqMode::Variable) ParseFailure("Function calls are not allowed in variable definitions");
            mPos++;
            item = parseFunctionCall();
            canQuantify = false;
        } else if (isLiteralChar(c)) {
            mPos++;
            item = makeCC(c);
        } else {
            std::string msg = "Unquoted syntax character '";
            appendUTF8(msg, c);
            ParseFailure(msg + "'");
        }
        items.push_back(item);
        quantifiable = canQuantify ? static_cast<int>(items.size()) - 1 : -1;
    }
}

RE * RuleTextParser::parseSegment() {
    const size_t slot = mCaptures.size();
    mCaptures.push_back(nullptr);
    std::vector<RE *> items;
    parseSequence(items, SeqMode::Pattern);
    skipWhiteSpace();
    require(')', "')' to close the segment");
    Capture * capture = makeCapture(std::to_string(slot + 1), makeSeq(items.begin(), items.end()));
    mCaptures[slot] = capture;
    return capture;
}

RE * RuleTextParser::parseFunctionCall() {
    std::u32string id;
    while (more() && (isLiteralChar(cur()) || at('-') || at('_') || at('/'))) {
        id.push_back(cur());
        mPos++;
    }
    if (id.empty()) ParseFailure("Expected a transform ID after '&'");
    skipWhiteSpace();
    require('(', "'(' after function name");
    std::vector<RE *> items;
    parseSequence(items, SeqMode::FunctionArgument);
    skipWhiteSpace();
    require(')', "')' to close the function call");
    return makeFunctionCall(TransformID(toUTF8(id)), makeSeq(items.begin(), items.end()));
}

// After "$", at a digit 1-9.
RE * RuleTextParser::parseReference() {
    const unsigned n = cur() - '0';
    mPos++;
    const std::string name = std::to_string(n);
    Capture * capture = nullptr;
    if (mOtherSideCaptures) {
        if (n > mOtherSideCaptures->size()) {
            ParseFailure("Reference $" + name + " to an undefined segment");
        }
        capture = (*mOtherSideCaptures)[n - 1];
    } else {
        mUnresolvedReferences = true;
    }
    return makeReference(name, capture, mReferenceInstance++);
}

// After "$", at an identifier start.
RE * RuleTextParser::parseVariableReference() {
    const std::string name = parseVariableName();
    auto f = mVariables.find(name);
    if (f == mVariables.end()) {
        if (mFailedVariables && mFailedVariables->count(name)) {
            ParseFailure("Variable $" + name + " is undefined, as its definition has an error");
        }
        ParseFailure("Undefined variable $" + name);
    }
    return f->second;
}

void RuleTextParser::parseQuotedString(std::u32string & s) {
    const char32_t quote = cur();
    mPos++;
    if (accept(quote)) {
        // '' outside of a quoted string is a literal quote.
        s.push_back(quote);
        return;
    }
    for (;;) {
        if (!more()) ParseFailure("Unterminated quoted string");
        if (accept(quote)) {
            if (accept(quote)) {
                s.push_back(quote);
            } else {
                return;
            }
        } else {
            s.push_back(cur());
            mPos++;
        }
    }
}

// After "\", an escape for a single codepoint.
char32_t RuleTextParser::parseEscapedChar() {
    const std::u32string chars = parseEscape();
    if (chars.size() != 1) ParseFailure("Expected an escape for a single codepoint");
    return chars[0];
}

// After "\".   Escapes \x{h h ...} may denote multiple codepoints.
std::u32string RuleTextParser::parseEscape() {
    if (!more()) ParseFailure("Incomplete escape sequence");
    if (accept("x{")) {
        std::u32string chars;
        skipSetWhiteSpace();
        do {
            chars.push_back(parseHex(1, 6));
            skipSetWhiteSpace();
        } while (!accept('}'));
        return chars;
    }
    const char32_t c = cur();
    mPos++;
    return std::u32string(1, [&]() -> char32_t {
    switch (c) {
        case 'u': return parseHex(4, 4);
        case 'U': return parseHex(8, 8);
        case 'x': return parseHex(2, 2);
        case 'N':
            ParseFailure("Named character escapes \\N{...} are not supported");
        case 'a': return 0x07;
        case 'b': return 0x08;
        case 'e': return 0x1B;
        case 'f': return 0x0C;
        case 'n': return 0x0A;
        case 'r': return 0x0D;
        case 't': return 0x09;
        case 'v': return 0x0B;
        default: return c;
    }
    }());
}

char32_t RuleTextParser::parseHex(unsigned minDigits, unsigned maxDigits) {
    char32_t cp = 0;
    unsigned digits = 0;
    while (digits < maxDigits && isHexDigit(cur())) {
        cp = cp * 16 + hexValue(cur());
        mPos++;
        digits++;
    }
    if (digits < minDigits) ParseFailure("Expected " + std::to_string(minDigits) + " hexadecimal digits");
    if (cp > 0x10FFFF) ParseFailure("Codepoint out of range");
    return cp;
}

bool RuleTextParser::atSetStart() const {
    return at('[') || at("\\p") || at("\\P");
}

//  A UnicodeSet: [...], [:prop:], \p{prop}, \P{prop}, \pL
RE * RuleTextParser::parseSet() {
    if (accept("[:")) {
        const bool negated = accept('^');
        return parsePropertySet(':', negated);
    }
    if (accept('[')) {
        return parseBracketSet();
    }
    if (accept("\\p") || accept("\\P")) {
        bool negated = mText[mPos - 1] == 'P';
        if (accept('{')) {
            if (accept('^')) negated = !negated;
            return parsePropertySet('}', negated);
        }
        if (!more() || !isASCIIAlpha(cur())) ParseFailure("Expected a property name after \\p");
        const std::u32string name(1, cur());
        mPos++;
        RE * prop = makePropertyExpression(canonicalizePropertyName(name));
        return negated ? setComplement(prop) : prop;
    }
    ParseFailure("Expected a UnicodeSet");
}

//  The contents of [:prop:] or \p{prop} after the opening delimiters, with
//  forms "value", "prop=value", "prop≠value".
RE * RuleTextParser::parsePropertySet(const char32_t closer, bool negated) {
    std::u32string content;
    for (;;) {
        if (!more()) ParseFailure("Unterminated property expression");
        if (closer == ':' ? accept(":]") : accept('}')) break;
        content.push_back(cur());
        mPos++;
    }
    PropertyExpression::Operator op = PropertyExpression::Operator::Eq;
    size_t split = content.find('=');
    size_t valueStart = split + 1;
    if (split == std::u32string::npos) {
        split = content.find(0x2260); // ≠
        valueStart = split + 1;
        if (split != std::u32string::npos) op = PropertyExpression::Operator::NEq;
    } else if (split > 0 && content[split - 1] == '!') {
        op = PropertyExpression::Operator::NEq;
        split--;
    }
    RE * prop = nullptr;
    if (split == std::u32string::npos) {
        const std::string ident = canonicalizePropertyName(content);
        if (ident.empty()) ParseFailure("Empty property expression");
        prop = (ident == "any") ? static_cast<RE *>(makeAny()) : makePropertyExpression(ident);
    } else {
        const std::string ident = canonicalizePropertyName(content.substr(0, split));
        const std::u32string value = trim(content.substr(valueStart));
        if (ident.empty() || value.empty()) ParseFailure("Malformed property expression");
        prop = makePropertyExpression(PropertyExpression::Kind::Codepoint, ident, op, toUTF8(value));
    }
    return negated ? setComplement(prop) : prop;
}

//  After "[".  The body is a sequence of items (characters, ranges, strings
//  {abc}, nested sets, variables, the text boundary $), with the binary
//  operators "-" (difference) and "&" (intersection) applying left to
//  right between the union of the preceding items and a following set.
RE * RuleTextParser::parseBracketSet() {
    skipSetWhiteSpace();
    const bool negated = accept('^');
    std::vector<RE *> members;
    bool boundary = false;
    enum class SetOp {None, Difference, Intersection} op = SetOp::None;
    char32_t lastChar = NONE;   // the last single character item, for ranges
    auto atOperand = [this]() {
        skipSetWhiteSpace();
        return atSetStart() || (at('$') && isIdStart(peek()));
    };
    for (;;) {
        skipSetWhiteSpace();
        if (!more()) ParseFailure("Unterminated UnicodeSet");
        if (accept(']')) break;
        const size_t opPos = mPos;
        if (op == SetOp::None && (at('-') || at('&')) && (!members.empty() || boundary)) {
            const bool difference = at('-');
            mPos++;
            if (atOperand()) {
                op = difference ? SetOp::Difference : SetOp::Intersection;
                lastChar = NONE;
                continue;
            }
            if (difference && lastChar != NONE && !at(']')) {
                // A range lastChar-hi.
                char32_t hi = NONE;
                if (accept('\\')) hi = parseEscapedChar();
                else if (more() && !at('[') && !at('{') && !at('$') && !at('^') && !at('&') && !at('-')) {
                    hi = cur();
                    mPos++;
                } else ParseFailure("Invalid range");
                if (hi < lastChar) ParseFailure("Invalid range: end precedes start");
                members.back() = makeCC(lastChar, hi);
                lastChar = NONE;
                continue;
            }
            mPos = opPos;   // a literal '-' or '&'
        }
        RE * item = nullptr;
        bool isSet = false;
        char32_t single = NONE;
        if (atSetStart()) {
            item = parseSet();
            isSet = true;
        } else if (at('$') && isIdStart(peek())) {
            mPos++;
            item = parseVariableReference();
            isSet = isSetOperand(item);
            if (!isSet) ParseFailure("Variables in a UnicodeSet must be sets");
        } else if (accept('$')) {
            if (op != SetOp::None) ParseFailure("Expected a set operand");
            boundary = true;
            lastChar = NONE;
            continue;
        } else if (accept('{')) {
            item = parseSetString();
        } else if (accept('\\')) {
            // A multiple codepoint escape \x{61 62} is equivalent to \x{61}\x{62}.
            const std::u32string chars = parseEscape();
            for (size_t i = 0; i + 1 < chars.size(); i++) members.push_back(makeCC(chars[i]));
            single = chars.back();
            item = makeCC(single);
        } else if (at('^') || at('&')) {
            ParseFailure("Unescaped syntax character in UnicodeSet");
        } else {
            single = cur();
            mPos++;
            item = makeCC(single);
        }
        if (op != SetOp::None) {
            if (!isSet) ParseFailure("The set operators - and & require a set operand");
            SetValue lhs{setUnion(members), boundary};
            SetValue rhs = fromSetRE(item);
            if (op == SetOp::Difference) {
                lhs = SetValue{setDifference(lhs.chars, rhs.chars), lhs.boundary && !rhs.boundary};
            } else {
                lhs = SetValue{setIntersection(lhs.chars, rhs.chars), lhs.boundary && rhs.boundary};
            }
            members.clear();
            members.push_back(lhs.chars);
            boundary = lhs.boundary;
            op = SetOp::None;
            lastChar = NONE;
            continue;
        }
        if (isSet) {
            SetValue v = fromSetRE(item);
            members.push_back(v.chars);
            boundary |= v.boundary;
        } else {
            members.push_back(item);
        }
        lastChar = single;
    }
    if (op != SetOp::None) ParseFailure("Missing set operand");
    SetValue v{setUnion(members), boundary};
    if (negated) {
        // The complement includes the text boundary, unless the set does.
        v = SetValue{setComplement(v.chars), !v.boundary};
    }
    return toSetRE(v);
}

// After "{" within a UnicodeSet.
RE * RuleTextParser::parseSetString() {
    std::vector<RE *> chars;
    for (;;) {
        skipSetWhiteSpace();
        if (!more()) ParseFailure("Unterminated string in UnicodeSet");
        if (accept('}')) break;
        if (accept('\\')) {
            for (const char32_t c : parseEscape()) chars.push_back(makeCC(c));
        } else {
            chars.push_back(makeCC(cur()));
            mPos++;
        }
    }
    return makeSeq(chars.begin(), chars.end());
}

RE * RuleTextParser::parseCompleteSet() {
    skipSetWhiteSpace();
    if (!atSetStart()) ParseFailure("Expected a UnicodeSet");
    RE * set = parseSet();
    skipSetWhiteSpace();
    if (more()) ParseFailure("Unexpected text after UnicodeSet");
    return set;
}

} // end anonymous namespace

static std::string joinMessages(const std::vector<TransformRuleParseError> & errors) {
    std::string msg;
    for (const TransformRuleParseError & e : errors) {
        if (!msg.empty()) msg += "\n";
        msg += e.what();
    }
    return msg;
}

TransformRuleParseErrors::TransformRuleParseErrors(std::vector<TransformRuleParseError> errors)
: TransformRuleParseError(joinMessages(errors)), mErrors(std::move(errors)) {}

std::vector<Rule *> TransformRuleParser::parse(const std::string & rules) {
    const std::u32string text = decodeUTF8(rules);
    RuleTextParser parser(text, mVariables, &mFailedVariables);
    std::vector<Rule *> parsed;
    std::vector<TransformRuleParseError> errors;
    for (;;) {
        const size_t start = parser.position();
        try {
            Rule * const r = parser.parseRule();
            if (r == nullptr) break;
            parsed.push_back(r);
        } catch (const TransformRuleParseError & e) {
            errors.push_back(e);
            parser.skipRule(start);
        }
    }
    mRules.insert(mRules.end(), parsed.begin(), parsed.end());
    if (!errors.empty()) throw TransformRuleParseErrors(std::move(errors));
    return parsed;
}

RE * TransformRuleParser::parseUnicodeSet(const std::string & set) {
    const std::u32string text = decodeUTF8(set);
    RuleTextParser parser(text, mVariables);
    return parser.parseCompleteSet();
}

void TransformRuleParser::validate() const {
    std::vector<TransformRuleParseError> errors;
    for (auto check : {&TransformRuleParser::validateRuleOrder, &TransformRuleParser::validateInsertions,
                       &TransformRuleParser::validateRepeatedSegments}) {
        const auto e = (this->*check)();
        errors.insert(errors.end(), e.begin(), e.end());
    }
    if (!errors.empty()) throw TransformRuleParseErrors(std::move(errors));
}

std::vector<TransformRuleParseError> TransformRuleParser::validateRuleOrder() const {
    std::vector<TransformRuleParseError> errors;
    const size_t n = mRules.size();
    for (size_t i = 0; i < n; i++) {
        if (const FilterRule * f = dyn_cast<FilterRule>(mRules[i])) {
            if (f->isInverse() && i != n - 1) {
                errors.emplace_back("Inverse filter rule " + printRule(f) + " is not the last rule");
            }
            if (!f->isInverse() && i != 0) {
                errors.emplace_back("Filter rule " + printRule(f) + " is not the first rule");
            }
        }
    }
    return errors;
}

// Whether an item of a rule may match the empty text (anchors constrain
// the position, and do not count as empty).
static bool matchesEmpty(RE * re) {
    if (re == nullptr) return true;
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        return std::all_of(seq->begin(), seq->end(), [](RE * e) {return matchesEmpty(e);});
    }
    if (const Alt * alt = dyn_cast<Alt>(re)) {
        return std::any_of(alt->begin(), alt->end(), [](RE * e) {return matchesEmpty(e);});
    }
    if (const Rep * rep = dyn_cast<Rep>(re)) return rep->getLB() == 0 || matchesEmpty(rep->getRE());
    if (const Capture * c = dyn_cast<Capture>(re)) return matchesEmpty(c->getCapturedRE());
    if (const Name * n = dyn_cast<Name>(re)) {
        return n->getDefinition() != nullptr && matchesEmpty(n->getDefinition());
    }
    return false;
}

std::vector<TransformRuleParseError> TransformRuleParser::validateInsertions() const {
    std::vector<TransformRuleParseError> errors;
    auto check = [&errors](const ConversionRule * r, const RuleSide * side) {
        if (side->hasBeforeContext() || side->hasAfterContext() || !matchesEmpty(side->getText())) return false;
        errors.emplace_back("Rule " + printRule(r) + " matches the empty text without contexts, indefinitely");
        return true;
    };
    for (const Rule * rule : mRules) {
        if (const ConversionRule * r = dyn_cast<ConversionRule>(rule)) {
            const unsigned d = static_cast<unsigned>(r->getDirection());
            // A rule is reported once, even if ill-formed in both directions.
            if (d & static_cast<unsigned>(Direction::Forward)) {
                if (check(r, r->getLeftSide())) continue;
            }
            if (d & static_cast<unsigned>(Direction::Backward)) check(r, r->getRightSide());
        }
    }
    return errors;
}

// The segments of a rule side within repetitions, and the segments referenced.
static void collectSegments(const RE * re, bool withinRep, std::set<const Capture *> & repeated,
                            std::set<const Capture *> & referenced) {
    if (re == nullptr) return;
    if (const Capture * c = dyn_cast<Capture>(re)) {
        if (withinRep) repeated.insert(c);
        collectSegments(c->getCapturedRE(), withinRep, repeated, referenced);
    } else if (const Reference * ref = dyn_cast<Reference>(re)) {
        referenced.insert(ref->getCapture());
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        collectSegments(rep->getRE(), withinRep || rep->getUB() != 1, repeated, referenced);
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) collectSegments(e, withinRep, repeated, referenced);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        for (const RE * e : *alt) collectSegments(e, withinRep, repeated, referenced);
    } else if (isFunctionCall(re)) {
        collectSegments(cast<Name>(re)->getDefinition(), withinRep, repeated, referenced);
    }
}

std::vector<TransformRuleParseError> TransformRuleParser::validateRepeatedSegments() const {
    std::vector<TransformRuleParseError> errors;
    for (const Rule * rule : mRules) {
        const ConversionRule * r = dyn_cast<ConversionRule>(rule);
        if (r == nullptr) continue;
        std::set<const Capture *> repeated;
        std::set<const Capture *> referenced;
        for (const RuleSide * side : {r->getLeftSide(), r->getRightSide()}) {
            for (const RE * part : {side->getBeforeContext(), side->getCompletedResult(), side->getResultToRevisit(), side->getAfterContext()}) {
                collectSegments(part, false, repeated, referenced);
            }
        }
        for (const Capture * c : repeated) {
            if (referenced.count(c)) {
                errors.emplace_back("Rule " + printRule(r) + " references a segment within a repetition, which captures only its last repetition");
                break;
            }
        }
    }
    return errors;
}

Name * TransformRuleParser::lookupVariable(const std::string & name) const {
    auto f = mVariables.find(name);
    return f == mVariables.end() ? nullptr : f->second;
}

std::vector<Rule *> parseTransformRules(const std::vector<std::string> & tRules) {
    TransformRuleParser parser;
    std::vector<TransformRuleParseError> errors;
    for (const std::string & r : tRules) {
        try {
            parser.parse(r);
        } catch (const TransformRuleParseErrors & e) {
            errors.insert(errors.end(), e.getErrors().begin(), e.getErrors().end());
        }
    }
    try {
        parser.validate();
    } catch (const TransformRuleParseErrors & e) {
        errors.insert(errors.end(), e.getErrors().begin(), e.getErrors().end());
    }
    if (!errors.empty()) throw TransformRuleParseErrors(std::move(errors));
    return parser.getRules();
}

std::string decodeXMLText(const std::string & s) {
    std::string result;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            const size_t end = s.find("]]>", i + 9);
            if (end == std::string::npos) throw std::runtime_error("Unterminated CDATA section");
            result += s.substr(i + 9, end - i - 9);
            i = end + 3;
        } else if (s.compare(i, 4, "<!--") == 0) {
            const size_t end = s.find("-->", i + 4);
            if (end == std::string::npos) throw std::runtime_error("Unterminated XML comment");
            i = end + 3;
        } else if (s[i] == '&') {
            const size_t semi = s.find(';', i);
            if (semi == std::string::npos) throw std::runtime_error("Malformed XML entity");
            const std::string entity = s.substr(i + 1, semi - i - 1);
            if (entity == "lt") result += "<";
            else if (entity == "gt") result += ">";
            else if (entity == "amp") result += "&";
            else if (entity == "quot") result += "\"";
            else if (entity == "apos") result += "'";
            else throw std::runtime_error("Unsupported XML entity &" + entity + ";");
            i = semi + 1;
        } else {
            result.push_back(s[i++]);
        }
    }
    return result;
}

std::vector<std::string> extractTRules(const std::string & xml) {
    std::vector<std::string> rules;
    size_t pos = 0;
    for (;;) {
        const size_t open = xml.find("<tRule", pos);
        if (open == std::string::npos) break;
        const size_t start = xml.find('>', open);
        if (start == std::string::npos) break;
        if (xml[start - 1] == '/') {    // <tRule/>
            pos = start + 1;
            continue;
        }
        const size_t end = xml.find("</tRule>", start);
        if (end == std::string::npos) throw std::runtime_error("Unterminated <tRule> element");
        rules.push_back(decodeXMLText(xml.substr(start + 1, end - start - 1)));
        pos = end + 8;
    }
    return rules;
}

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Parser for the rules of LDML transforms (the CDATA content of <tRule>
//  elements), producing the rule objects of <ldml/transform_rules.h>.
//
//  A single parser object should be used for all the <tRule> elements of
//  a <transform>, as variables defined in one element may be used in later
//  elements.

#pragma once

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <ldml/transform_rules.h>

namespace re { class RE; class Name; }

namespace ldml {

class TransformRuleParseError : public std::runtime_error {
public:
    TransformRuleParseError(const std::string & msg, size_t line, size_t column)
    : std::runtime_error(msg + " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")")
    , mLine(line), mColumn(column) {}
    // An error not associated with a position in the rule text.
    explicit TransformRuleParseError(const std::string & msg)
    : std::runtime_error(msg), mLine(0), mColumn(0) {}
    size_t getLine() const {return mLine;}
    size_t getColumn() const {return mColumn;}
private:
    size_t mLine;
    size_t mColumn;
};

// All the errors of a rule text (or of the rules of a transform), thrown
// once parsing and validation are complete.  The message lists the errors,
// one per line.
class TransformRuleParseErrors : public TransformRuleParseError {
public:
    explicit TransformRuleParseErrors(std::vector<TransformRuleParseError> errors);
    const std::vector<TransformRuleParseError> & getErrors() const {return mErrors;}
private:
    std::vector<TransformRuleParseError> mErrors;
};

class TransformRuleParser {
public:
    TransformRuleParser() = default;

    // Parse UTF-8 text consisting of zero or more rules, each terminated
    // by ";" (the final ";" may be omitted).  The new rules are appended
    // to the rule list and returned.   After an error in a rule, parsing
    // resumes after the ";" ending it, so that all the errors of the text
    // are found; if there are any, TransformRuleParseErrors is thrown at
    // the end (the rules without errors are still in the rule list).
    std::vector<Rule *> parse(const std::string & rules);

    // Parse a single UnicodeSet, e.g. "[[:Latin:]-[a-z]]" or "\p{Lu}".
    re::RE * parseUnicodeSet(const std::string & set);

    // All rules parsed so far.
    const std::vector<Rule *> & getRules() const {return mRules;}

    // Check the constraints on the rule list as a whole (see below),
    // throwing TransformRuleParseErrors with all the violations.
    void validate() const;
    // Each check returns its violations:
    // A filter rule may only be the first rule and an inverse filter rule
    // may only be the last rule.
    std::vector<TransformRuleParseError> validateRuleOrder() const;
    // A conversion rule whose text to match (in a direction in which it
    // applies) may be empty, without contexts, would match again at the
    // same position indefinitely: the rules are ill-formed.
    std::vector<TransformRuleParseError> validateInsertions() const;
    // A segment within a repetition captures only its last repetition (or
    // nothing): a rule referencing one is ill-formed.
    std::vector<TransformRuleParseError> validateRepeatedSegments() const;

    // The variable of the given name, or nullptr if not defined.
    re::Name * lookupVariable(const std::string & name) const;

private:
    std::map<std::string, re::Name *> mVariables;
    // Variables whose definitions have errors (their uses are reported as such).
    std::set<std::string> mFailedVariables;
    std::vector<Rule *> mRules;
};

// Parse the contents of all the <tRule> elements of a transform.  All the
// syntax errors of all the elements and the violations of the rule list
// constraints are reported together, as TransformRuleParseErrors.
std::vector<Rule *> parseTransformRules(const std::vector<std::string> & tRules);

// The text content of an XML element: CDATA sections are copied, comments are
// removed and the predefined entities (&lt; &gt; &amp; &quot; &apos;) are decoded.
// Throws std::runtime_error for an unterminated CDATA section or comment, or
// another entity.
std::string decodeXMLText(const std::string & s);

// The decoded contents of the <tRule> elements of an LDML transform file, in
// order (empty <tRule/> elements are skipped).  Throws std::runtime_error for
// an unterminated element.
std::vector<std::string> extractTRules(const std::string & xml);

}

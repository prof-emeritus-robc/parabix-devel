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

class TransformRuleParser {
public:
    TransformRuleParser() = default;

    // Parse UTF-8 text consisting of zero or more rules, each terminated
    // by ";" (the final ";" may be omitted).  The new rules are appended
    // to the rule list and returned.   Throws TransformRuleParseError.
    std::vector<Rule *> parse(const std::string & rules);

    // Parse a single UnicodeSet, e.g. "[[:Latin:]-[a-z]]" or "\p{Lu}".
    re::RE * parseUnicodeSet(const std::string & set);

    // All rules parsed so far.
    const std::vector<Rule *> & getRules() const {return mRules;}

    // Check the rule list constraints: a filter rule may only be the first
    // rule and an inverse filter rule may only be the last rule.
    // Throws TransformRuleParseError.
    void validateRuleOrder() const;

    // The variable of the given name, or nullptr if not defined.
    re::Name * lookupVariable(const std::string & name) const;

private:
    std::map<std::string, re::Name *> mVariables;
    std::vector<Rule *> mRules;
};

// Parse the contents of all the <tRule> elements of a transform.
std::vector<Rule *> parseTransformRules(const std::vector<std::string> & tRules);

}

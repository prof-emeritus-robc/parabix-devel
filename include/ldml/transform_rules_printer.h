/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  Printing of LDML transform rules in the rule syntax of UTS #35.
//  Printing the result of parsing produces a canonical form of the
//  rules which parses to equivalent rule objects.

#pragma once

#include <string>
#include <vector>

namespace re { class RE; }

namespace ldml {

class Rule;
class RuleSide;

std::string printRule(const Rule * rule);

// One rule per line.
std::string printRules(const std::vector<Rule *> & rules);

std::string printRuleSide(const RuleSide * side);

// A pattern (context, text, variable content) in rule syntax.
std::string printPattern(const re::RE * re);

// A set in UnicodeSet syntax.
std::string printUnicodeSet(const re::RE * re);

}

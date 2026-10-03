/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/parse/fixed_string_parser.h>

#include <re/parse/parser.h>
#include <re/adt/re_alt.h>
#include <re/adt/re_seq.h>

namespace re {

RE * FixedStringParser::parse_alt() {
    std::vector<RE *> alt;
    do {
        alt.push_back(parse_seq());
    }
    while (accept('\n'));
    return Alt::Create(alt.begin(), alt.end());
}

RE * FixedStringParser::parse_seq() {
    std::vector<RE *> seq;
    while (mCursor.more() && (!at('\n'))) {
        seq.push_back(createCC(parse_literal_codepoint()));
    }
    return Seq::Create(seq.begin(), seq.end());
}

}

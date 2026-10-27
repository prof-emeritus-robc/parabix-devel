/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/parse/ERE_parser.h>

#include <re/adt/re_start.h>
#include <re/adt/re_end.h>
#include <re/adt/re_any.h>
#include <re/adt/re_alt.h>
#include <re/adt/re_seq.h>
#include <re/adt/re_utility.h>

namespace re {


RE * ERE_Parser::parse_next_item() {
    if (mCursor.noMore() || atany("*?+{|")) return nullptr;
    else if ((mGroupsOpen > 0) && at(')')) return nullptr;
    else if (accept('^')) return Start::Create();
    else if (accept('$')) return End::Create();
    else if (accept('.')) return Any::Create();
    else if (accept('(')) return parse_group();
    else if (accept('[')) return parse_bracket_expr();
    else if (accept('\\')) return parse_escaped();
    else return createCC(parse_literal_codepoint());
}

// A parenthesized capture group.  Input precondition: the opening ( has been consumed
RE * ERE_Parser::parse_group() {
    mGroupsOpen++;
    RE * captured = parse_capture_body();
    require(')');
    mGroupsOpen--;
    return captured;
}

RE * ERE_Parser::parse_escaped() {
    if (accept('b')) return makeWordBoundary();
    if (accept('B')) return makeWordNonBoundary();
    if (accept('s')) return PropertyExpression::Create(PropertyExpression::Kind::Codepoint, "whitespace");
    if (accept('S')) return makeComplement(PropertyExpression::Create(PropertyExpression::Kind::Codepoint, "whitespace"));
    if (accept('<')) return makeWordBegin();
    if (accept('>')) return makeWordEnd();
    if (isdigit(*mCursor)) return parse_back_reference();
    else {
        return createCC(parse_literal_codepoint());
    }
}


// Parsing items within a bracket expression.
// Items represent individual characters or sets of characters.
// Ranges may be formed by individual character items separated by '-'.
// Note that there are no backslash escapes for ERE or BRE bracket expressions.
RE * ERE_Parser::parse_bracket_expr () {
    bool negated = accept('^');
    std::vector<RE *> items;
    do {
        if (accept('[')) {
            if (accept('=')) items.push_back(parse_equivalence_class());
            else if (accept('.')) items.push_back(range_extend(parse_collation_element()));
            else if (accept(':')) items.push_back(parse_Posix_class());
            else items.push_back(parse_bracket_expr());
        } else {
            items.push_back(range_extend(CC::Create(parse_literal_codepoint())));
        }
    } while (mCursor.more() && !at(']'));
    RE * t = Alt::Create(items.begin(), items.end());
    require(']');
    if (negated) return makeComplement(t);
    else return t;
}
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_rules_printer.h>
#include <ldml/transform_rules.h>
#include <re/adt/adt.h>
#include <cstdio>

using namespace llvm;
using namespace re;

namespace ldml {

namespace {

void appendUTF8(std::string & s, const codepoint_t cp) {
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

bool isASCIIAlnum(const codepoint_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Characters printed in \u notation: controls, white space and
// separators, surrogates, noncharacters U+FFFE/U+FFFF.
bool needsHexEscape(const codepoint_t c) {
    return c < 0x20 || c == ' ' || (c >= 0x7F && c <= 0xA0) || c == 0x1680 || (c >= 0x2000 && c <= 0x200F)
        || (c >= 0x2028 && c <= 0x202F) || c == 0x205F || c == 0x3000 || (c >= 0xD800 && c <= 0xDFFF)
        || c == 0xFEFF || c == 0xFFFE || c == 0xFFFF;
}

bool isArrow(const codepoint_t c) {
    return c == 0x2190 || c == 0x2192 || c == 0x2194;
}

void appendHexEscape(std::string & s, const codepoint_t c) {
    char buf[16];
    if (c <= 0xFFFF) {
        std::snprintf(buf, sizeof(buf), "\\u%04X", c);
    } else {
        std::snprintf(buf, sizeof(buf), "\\U%08X", c);
    }
    s += buf;
}

// A character as literal rule text.
void appendLiteral(std::string & s, const codepoint_t c) {
    if (needsHexEscape(c)) {
        appendHexEscape(s, c);
    } else if (c < 0x80 && !isASCIIAlnum(c)) {
        s.push_back('\\');
        s.push_back(static_cast<char>(c));
    } else if (isArrow(c)) {
        s.push_back('\\');
        appendUTF8(s, c);
    } else {
        appendUTF8(s, c);
    }
}

// A character within a UnicodeSet.
void appendSetChar(std::string & s, const codepoint_t c) {
    appendLiteral(s, c);
}

bool isSingleCodepoint(const RE * re) {
    if (const CC * cc = dyn_cast<CC>(re)) {
        return cc->size() == 1 && lo_codepoint(cc->front()) == hi_codepoint(cc->front());
    }
    return false;
}

codepoint_t theCodepoint(const RE * re) {
    return lo_codepoint(cast<CC>(re)->front());
}

// A sequence of single codepoints, i.e., a string.
bool isString(const RE * re) {
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        for (const RE * e : *seq) {
            if (!isSingleCodepoint(e)) return false;
        }
        return true;
    }
    return false;
}

void appendCCBody(std::string & s, const CC * cc) {
    for (const auto & i : *cc) {
        const codepoint_t lo = lo_codepoint(i);
        const codepoint_t hi = hi_codepoint(i);
        appendSetChar(s, lo);
        if (hi == lo + 1) {
            appendSetChar(s, hi);
        } else if (hi > lo) {
            s.push_back('-');
            appendSetChar(s, hi);
        }
    }
}

std::string printSet(const RE * re);

std::string printProperty(const PropertyExpression * pe, const bool negated) {
    std::string s = negated ? "[:^" : "[:";
    s += pe->getPropertyIdentifier();
    if (!pe->getValueString().empty()) {
        s += (pe->getOperator() == PropertyExpression::Operator::NEq) ? "≠" : "=";
        s += pe->getValueString();
    }
    return s + ":]";
}

//  The members of a set, without the enclosing brackets.
std::string printSetBody(const RE * re) {
    std::string s;
    if (const CC * cc = dyn_cast<CC>(re)) {
        appendCCBody(s, cc);
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        bool boundary = false;
        bool afterVariable = false;
        for (const RE * a : *alt) {
            if (isa<Start>(a) || isa<End>(a)) {
                boundary = true;
            } else {
                const std::string member = printSetBody(a);
                // Separate a variable name from a following letter or digit.
                if (afterVariable && (isASCIIAlnum(member[0]) || static_cast<unsigned char>(member[0]) >= 0x80)) {
                    s += " ";
                }
                s += member;
                afterVariable = isa<Name>(a);
            }
        }
        if (boundary) s += "$";
    } else if (const Seq * seq = dyn_cast<Seq>(re)) {
        s += "{";
        for (const RE * e : *seq) {
            if (isSingleCodepoint(e)) {
                appendSetChar(s, theCodepoint(e));
            }
        }
        s += "}";
    } else if (const Name * n = dyn_cast<Name>(re)) {
        s += "$" + n->getName();
    } else {
        s += printSet(re);
    }
    return s;
}

//  A set with enclosing brackets.
std::string printSet(const RE * re) {
    if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(re)) {
        return printProperty(pe, false);
    } else if (isa<Any>(re)) {
        return "[:any:]";
    } else if (const Name * n = dyn_cast<Name>(re)) {
        return "$" + n->getName();
    } else if (const Diff * d = dyn_cast<Diff>(re)) {
        if (isa<Any>(d->getLH())) {
            if (const PropertyExpression * pe = dyn_cast<PropertyExpression>(d->getRH())) {
                return printProperty(pe, true);
            }
            // The complement of a set excluding the text boundary.
            return "[^" + printSetBody(d->getRH()) + "$]";
        }
        return "[" + printSet(d->getLH()) + "-" + printSet(d->getRH()) + "]";
    } else if (const Intersect * x = dyn_cast<Intersect>(re)) {
        return "[" + printSet(x->getLH()) + "&" + printSet(x->getRH()) + "]";
    } else if (const Alt * alt = dyn_cast<Alt>(re)) {
        if (includesTextBoundary(re) && alt->size() == 3) {
            for (const RE * a : *alt) {
                if (const Diff * d = dyn_cast<Diff>(a)) {
                    if (isa<Any>(d->getLH())) {
                        // A negated set, which includes the text boundary.
                        return "[^" + printSetBody(d->getRH()) + "]";
                    }
                }
            }
        }
    }
    return "[" + printSetBody(re) + "]";
}

bool isLiteralItem(const RE * re) {
    return isSingleCodepoint(re);
}

std::string printItem(const RE * re);

std::string printSequence(const RE * re) {
    if (re == nullptr) return "";
    if (const Seq * seq = dyn_cast<Seq>(re)) {
        std::string s;
        const RE * prev = nullptr;
        for (const RE * e : *seq) {
            if (prev && !(isLiteralItem(prev) && isLiteralItem(e))) s += " ";
            s += printItem(e);
            prev = e;
        }
        return s;
    }
    return printItem(re);
}

std::string printQuoted(const RE * re) {
    std::string s = "'";
    for (const RE * e : *cast<Seq>(re)) {
        const codepoint_t c = theCodepoint(e);
        if (c == '\'') s += "''";
        else appendUTF8(s, c);
    }
    return s + "'";
}

std::string printItem(const RE * re) {
    if (isSingleCodepoint(re)) {
        std::string s;
        appendLiteral(s, theCodepoint(re));
        return s;
    } else if (isa<Start>(re)) {
        return "^";
    } else if (isa<End>(re)) {
        return "$";
    } else if (isTextBoundary(re)) {
        return "[$]";
    } else if (isDotSet(re)) {
        return ".";
    } else if (const Name * n = dyn_cast<Name>(re)) {
        if (isFunctionCall(re)) {
            return "&" + n->getName() + "(" + printSequence(n->getDefinition()) + ")";
        }
        return "$" + n->getName();
    } else if (const Capture * c = dyn_cast<Capture>(re)) {
        return "(" + printSequence(c->getCapturedRE()) + ")";
    } else if (const Reference * r = dyn_cast<Reference>(re)) {
        return "$" + r->getName();
    } else if (const Rep * rep = dyn_cast<Rep>(re)) {
        const RE * operand = rep->getRE();
        std::string s = isString(operand) ? printQuoted(operand) : printItem(operand);
        const int lb = rep->getLB();
        const int ub = rep->getUB();
        if (lb == 0 && ub == 1) return s + "?";
        if (lb == 0 && ub == Rep::UNBOUNDED_REP) return s + "*";
        if (lb == 1 && ub == Rep::UNBOUNDED_REP) return s + "+";
        // Not expressible in rule syntax.
        return s + "{" + std::to_string(lb) + "," + (ub == Rep::UNBOUNDED_REP ? "" : std::to_string(ub)) + "}";
    } else if (isa<Seq>(re)) {
        if (isString(re)) return printQuoted(re);
        return printSequence(re);
    }
    return printSet(re);
}

void appendPart(std::string & s, const std::string & part) {
    if (part.empty()) return;
    if (!s.empty()) s += " ";
    s += part;
}

// A set standing alone as a filter: a variable must be within brackets.
std::string printFilterSet(const RE * re) {
    if (isa<Name>(re)) return "[" + printSet(re) + "]";
    return printSet(re);
}

std::string printTransformPart(const TransformID & id, const RE * filter) {
    std::string s;
    if (filter) appendPart(s, printFilterSet(filter));
    appendPart(s, id.getText());
    return s;
}

} // end anonymous namespace

std::string printPattern(const RE * re) {
    return printSequence(re);
}

std::string printUnicodeSet(const RE * re) {
    return printSet(re);
}

std::string printRuleSide(const RuleSide * side) {
    std::string s;
    if (side->hasBeforeContext()) {
        appendPart(s, printSequence(side->getBeforeContext()));
        appendPart(s, "{");
    }
    appendPart(s, printSequence(side->getCompletedResult()));
    if (side->hasCursor()) {
        const int offset = side->getCursorOffset();
        std::string cursor;
        if (offset > 0) cursor.append(offset, '@');
        cursor += "|";
        if (offset < 0) cursor.append(-offset, '@');
        appendPart(s, cursor);
    }
    appendPart(s, printSequence(side->getResultToRevisit()));
    if (side->hasAfterContext()) {
        appendPart(s, "}");
        appendPart(s, printSequence(side->getAfterContext()));
    }
    return s;
}

std::string printRule(const Rule * rule) {
    std::string s;
    if (const FilterRule * f = dyn_cast<FilterRule>(rule)) {
        if (f->isInverse()) {
            s = ":: (" + printFilterSet(f->getFilterSet()) + ")";
        } else {
            s = ":: " + printFilterSet(f->getFilterSet());
        }
    } else if (const TransformRule * t = dyn_cast<TransformRule>(rule)) {
        s = "::";
        appendPart(s, printTransformPart(t->getForwardID(), t->getForwardFilter()));
        if (t->hasExplicitInverse()) {
            appendPart(s, "(" + printTransformPart(t->getBackwardID(), t->getBackwardFilter()) + ")");
        }
    } else if (const VariableDefinitionRule * v = dyn_cast<VariableDefinitionRule>(rule)) {
        s = "$" + v->getName() + " =";
        appendPart(s, printSequence(v->getDefinition()));
    } else if (const ConversionRule * c = dyn_cast<ConversionRule>(rule)) {
        s = printRuleSide(c->getLeftSide());
        switch (c->getDirection()) {
            case Direction::Forward: appendPart(s, "→"); break;
            case Direction::Backward: appendPart(s, "←"); break;
            case Direction::Both: appendPart(s, "↔"); break;
        }
        appendPart(s, printRuleSide(c->getRightSide()));
    }
    return s + " ;";
}

std::string printRules(const std::vector<Rule *> & rules) {
    std::string s;
    for (const Rule * r : rules) {
        s += printRule(r);
        s += "\n";
    }
    return s;
}

}

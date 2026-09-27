/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  ldml_trules: parse LDML transform rules and print them in canonical form.
//
//  ldml_trules [--xml] [--quiet] [--forward | --backward]
//              [--eliminate-trivial-captures] [--eliminate-nullable-captures] file ...
//      Parse the rules of each file (plain rule text, or with --xml, the
//      <tRule> elements of an LDML transform file), print the canonical
//      form of the rules and check that the canonical form reparses to
//      the same canonical form.  With --forward or --backward, the rules
//      extracted for that direction (as forward rules) are printed.
//      With --eliminate-trivial-captures and --eliminate-nullable-captures,
//      trivial and then nullable capture elimination are applied to the
//      (forward) rules before printing.
//  ldml_trules [--xml] [--quiet] --overlaps file ...
//      Report the pairs of rules of the same group that may match at the
//      same position, in the forward and backward rules (listing the pairs
//      unless --quiet).
//  ldml_trules [--xml] [--quiet] --partition file ...
//      Partition the characters of the rules into mutually exclusive
//      classes: print the classes and the variables partitioned into
//      them (unless --quiet), and verify the partition.
//  ldml_trules [--xml] --count-trivial-captures file ...
//      Report the rules transformed and not transformed by trivial capture
//      elimination in the forward and backward rules.
//  ldml_trules [--xml] --count-nullable-captures file ...
//      Report the nullable captures in the source sides of conversion rules.
//  ldml_trules --self-test
//      Run the built-in test cases.

#include <ldml/transform_rules.h>
#include <ldml/transform_rules_parser.h>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include <cstring>
#include <map>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace ldml;

static std::string readFile(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open " + path);
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

static std::string decodeXMLText(const std::string & s) {
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

// The contents of the <tRule> elements of an LDML file.
static std::vector<std::string> extractTRules(const std::string & xml) {
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

// The printed rules reparse to themselves (exact), or at least reparse to a
// canonical form that is stable (as when classes are kept as distinct
// members of sets, which parsing merges).
static bool reparses(const std::string & printed, bool exact, std::string & reprinted) {
    try {
        reprinted = printRules(parseTransformRules({printed}));
        if (exact) return reprinted == printed;
        return printRules(parseTransformRules({reprinted})) == reprinted;
    } catch (const TransformRuleParseError & e) {
        reprinted = std::string("reparse failed: ") + e.what() + "\n";
        return false;
    }
}

enum class Extract {All, Forward, Backward};

static std::vector<Rule *> extract(const std::vector<Rule *> & rules, const Extract e) {
    switch (e) {
        case Extract::Forward: return ExtractForwardRules(rules);
        case Extract::Backward: return ExtractReverseBackwardRules(rules);
        default: return rules;
    }
}

// Parse, print, and check that the printed form reparses to the same form.
static bool processRules(const std::vector<std::string> & tRules, const std::string & label, bool quiet, const Extract e,
                         bool eliminateTrivial, bool eliminateNullable) {
    try {
        std::vector<Rule *> rules = extract(parseTransformRules(tRules), e);
        if (eliminateTrivial) rules = TrivialCaptureElimination(rules);
        if (eliminateNullable) rules = NullableCaptureElimination(rules);
        const std::string printed = printRules(rules);
        if (!quiet) std::cout << printed;
        std::string reprinted;
        if (!reparses(printed, true, reprinted)) {
            std::cerr << label << ": round trip mismatch\n--- printed\n" << printed << "--- reprinted\n" << reprinted;
            return false;
        }
        if (quiet) std::cout << label << ": " << rules.size() << " rules OK\n";
        return true;
    } catch (const std::exception & e) {
        std::cerr << label << ": " << e.what() << "\n";
        return false;
    }
}

struct TestCase {
    const char * input;
    const char * expected;    // canonical output, or nullptr if a parse error is expected
};

static const TestCase testCases[] = {
    // Conversion rules
    {"xy → z ;", "xy → z ;\n"},
    {"sch > sh ; ss > z ;", "sch → sh ;\nss → z ;\n"},
    {"\\← → arrow\\ sign ;", "\\← → arrow\\u0020sign ;\n"},
    {"'←' → 'arrow sign' ;", "\\← → arrow\\u0020sign ;\n"},
    {"'←' → arrow' 'sign ;", "\\← → arrow\\u0020sign ;\n"},
    {"x → ks ; # change every x into ks", "x → ks ;\n"},
    {"\\u03C0 → p ; \\x{3C0} → p ;", "π → p ;\nπ → p ;\n"},
    {"$pi = \\u03C0 ; $pi → p ; $pi ← p ; $pi ↔ p ; {$pi} ↔ {p} ;",
     "$pi = π ;\n$pi → p ;\n$pi ← p ;\n$pi ↔ p ;\n$pi ↔ p ;\n"},
    {"$pi = \\u03C0 ; $pi < p ; $pi <> p ;", "$pi = π ;\n$pi ← p ;\n$pi ↔ p ;\n"},
    {"[:Lowercase:] { '-' → ;", "[:lowercase:] { \\- → ;\n"},
    {"[:Lowercase:] { '-' } [:Uppercase:] → ;", "[:lowercase:] { \\- } [:uppercase:] → ;\n"},
    {"[[:Lowercase:]$] {'-' → ;", "[[:lowercase:]$] { \\- → ;\n"},
    {"[^[:Lowercase:]] {'-' → ;", "[^[:lowercase:]] { \\- → ;\n"},
    {"[^[:Lowercase:]$] {'-' → ;", "[:^lowercase:] { \\- → ;\n"},
    {"x → y | z ; z a → w ;", "x → y | z ;\nza → w ;\n"},
    {"[a-z]{x > |@ab ; ab > J; ca > M;", "[a-z] { x → |@ ab ;\nab → J ;\nca → M ;\n"},
    {"a → Ab@|;", "a → Ab @| ;\n"},
    {"a → Ab@@|;", "a → Ab @@| ;\n"},
    {"a { b | c } d ↔ e { f | g } h ;", "a { b | c } d ↔ e { f | g } h ;\n"},
    {"[a-c] → ;", "[a-c] → ;\n"},
    {"$single = \\' ; $space = ' ' ; $double = \\\" ; $back = \\` ;",
     "$single = \\' ;\n$space = \\u0020 ;\n$double = \\\" ;\n$back = \\` ;\n"},
    {"$double = \\\" ; ^ { $double → “ ;", "$double = \\\" ;\n^ { $double → “ ;\n"},
    {"$makeRight = [[:separator:][:start punctuation:][:initial punctuation:]] ;",
     "$makeRight = [[:separator:][:startpunctuation:][:initialpunctuation:]] ;\n"},
    {"'--' ↔ — ;", "\\-\\- ↔ — ;\n"},
    {"a?b → c ; a+b → c ; a*b → c ;", "a? b → c ;\na+ b → c ;\na* b → c ;\n"},
    {"'ab'* → c ;", "'ab'* → c ;\n"},
    {"([a-b]) > &hex($1);", "([ab]) → &hex($1) ;\n"},
    {"([a-b]) > &Any-Hex/Unicode($1);", "([ab]) → &Any-Hex/Unicode($1) ;\n"},
    {"$1 ← (x) ;", "$1 ← (x) ;\n"},
    {"(a)(b) ↔ $2 $1 ;", "(a) (b) ↔ $2 $1 ;\n"},
    {"a } $ → b ;", "a } $ → b ;\n"},
    {"[$] b → c ;", "[$] b → c ;\n"},
    {"[\\p{L}-[a-z]] → x ;", "[[:l:]-[a-z]] → x ;\n"},
    {"[[:L:]&[:Latin:]] → x ;", "[[:l:]&[:latin:]] → x ;\n"},
    {"[{ch}a-c] → x ;", "[{ch}a-c] → x ;\n"},
    {"\\p{Script=Greek} → x ; [:^gc=Lu:] → y ;", "[:script=Greek:] → x ;\n[:^gc=Lu:] → y ;\n"},
    {"$v = [aeiou] ; [$v y] → V ;", "$v = [aeiou] ;\n[$v y] → V ;\n"},
    {"[a\\-z] → x ;", "[\\-az] → x ;\n"},
    {"$mac = M [aA] [cC] ;", "$mac = M [Aa] [Cc] ;\n"},
    {"$space = [:Separator:]* ; high $space school → 'H.S.' ;",
     "$space = [:separator:]* ;\nhigh $space school → H\\.S\\. ;\n"},
    {"(.) → &Any-Lower($1) ;", "(.) → &Any-Lower($1) ;\n"},
    {"[:Greek:] [^[:ccc=Not_Reordered:][:ccc=Above:]]*? { \\u0345 → ;",
     "[:greek:] [^[:ccc=Not_Reordered:][:ccc=Above:]]* { \u0345 → ;\n"},
    {"s [’'] { h → x ;", "s [\\'’] { h → x ;\n"},
    {"\\x{61 62} → [\\x{63 64}] ;", "ab → [cd] ;\n"},
    {"[{\\x{ 061 62 0063}}] → x ;", "abc → x ;\n"},
    // Transform rules
    {":: NFD ; :: und_Latn-und_Greek ; :: Latin-Greek;",
     ":: NFD ;\n:: und_Latn-und_Greek ;\n:: Latin-Greek ;\n"},
    {":: lower () ; :: (lower) ; :: lower ;", ":: lower () ;\n:: (lower) ;\n:: lower ;\n"},
    {"::NFD (NFC) ;", ":: NFD (NFC) ;\n"},
    {":: [:Latin:] Upper ([:Latin:] Lower) ;", ":: [:latin:] Upper ([:latin:] Lower) ;\n"},
    // Filter rules
    {":: [:^Katakana:] ; :: Hiragana-Katakana; :: Katakana-Latin; :: ([:^Katakana:]) ;",
     ":: [:^katakana:] ;\n:: Hiragana-Katakana ;\n:: Katakana-Latin ;\n:: ([:^katakana:]) ;\n"},
    // Final ";" may be omitted
    {"a → b", "a → b ;\n"},
    // Errors
    {"a → b ; :: [a] ;", nullptr},          // filter rule not first
    {":: ([a]) ; a → b ;", nullptr},        // inverse filter rule not last
    {"a b ;", nullptr},                     // no operator
    {"$x → y ;", nullptr},                  // undefined variable
    {"$x = a ; $x = b ;", nullptr},         // redefined variable
    {"a - b → c ;", nullptr},               // unquoted reserved character
    {"a → b @ c | d ;", nullptr},           // misplaced @
    {"a → @b ;", nullptr},                  // @ without cursor
    {"a → b | c | d ;", nullptr},           // two cursors
    {"(a) → $2 ;", nullptr},                // undefined segment
    {"* a → b ;", nullptr},                 // quantifier without operand
    {"a { b { c → d ;", nullptr},           // misplaced {
    {"[a-z → b ;", nullptr},                // unterminated set
    {"'abc → d ;", nullptr},                // unterminated quote
    {"a $ b → c ;", nullptr},               // misplaced $ anchor
    {"$x = (a) ;", nullptr},                // segment in variable definition
    {"\\u12 → a ;", nullptr},               // short hex escape
    {":: ;", nullptr},                      // empty transform rule
    {"[z-a] → b ;", nullptr},               // invalid range
    {"[a^] → b ;", nullptr},                // unescaped ^ in set
    {"[a-\\x{62 63}] → b ;", nullptr},      // range end is not a single codepoint
};

struct NullableCounts {
    unsigned captures = 0;      // nullable captures not within repetitions
    unsigned repeated = 0;      // nullable captures within repetitions
    unsigned rules = 0;         // rule directions with eliminable captures
    unsigned forwardAdded = 0;  // rules added by elimination
    unsigned backwardAdded = 0;
    unsigned files = 0;
};

static void countNullableCaptures(const std::vector<std::string> & tRules, const std::string & label, NullableCounts & total) {
    const std::vector<Rule *> rules = parseTransformRules(tRules);
    NullableCounts n;
    for (const Rule * r : rules) {
        if (const ConversionRule * c = llvm::dyn_cast<ConversionRule>(r)) {
            for (const Direction d : {Direction::Forward, Direction::Backward}) {
                if (d == Direction::Forward ? !appliesForward(c->getDirection()) : !appliesBackward(c->getDirection())) continue;
                const RuleSide * source = c->getSourceSide(d);
                const auto eliminable = findNullableCaptures(source).size();
                n.captures += eliminable;
                n.repeated += findNullableCaptures(source, true).size() - eliminable;
                n.rules += eliminable > 0;
            }
        }
    }
    const std::vector<Rule *> fwd = ExtractForwardRules(rules);
    const std::vector<Rule *> bwd = ExtractReverseBackwardRules(rules);
    n.forwardAdded = NullableCaptureElimination(fwd).size() - fwd.size();
    n.backwardAdded = NullableCaptureElimination(bwd).size() - bwd.size();
    if (n.captures + n.repeated > 0) {
        std::cout << label << ": " << n.captures << " nullable captures in " << n.rules << " rules"
                  << " (+" << n.forwardAdded << " forward rules, +" << n.backwardAdded << " backward rules)";
        if (n.repeated) std::cout << ", " << n.repeated << " within repetitions";
        std::cout << "\n";
        total.files++;
    }
    total.captures += n.captures;
    total.repeated += n.repeated;
    total.rules += n.rules;
    total.forwardAdded += n.forwardAdded;
    total.backwardAdded += n.backwardAdded;
}

// A character class as an element of a set: the character or the variable.
static std::string classElement(const CharacterClass & c) {
    if (c.literal) return printPattern(c.chars);
    if (c.inlineSet) return c.text;
    return "$" + c.name;
}

// The partition as variable definitions: the classes, then the variables
// that are divided into several classes, with the literal characters and the
// inline sets in comments.
static std::string printPartition(const CharacterClassPartition & p) {
    std::string defs;
    std::string literals;
    for (const CharacterClass & c : p.classes) {
        if (c.literal) {
            literals += " " + printPattern(c.chars);
        } else if (!c.inlineSet) {
            const re::CC * cc = c.chars;
            const bool single = cc->size() == 1 && re::lo_codepoint(cc->front()) == re::hi_codepoint(cc->front());
            defs += "$" + c.name + " = " + (single ? printPattern(cc) : printUnicodeSet(cc)) + " ;\n";
        }
    }
    for (const auto & v : p.variables) {
        if (v.classes.size() == 1 && p.classes[v.classes[0]].name == v.name) continue;
        defs += "$" + v.name + " = [";
        for (size_t i = 0; i < v.classes.size(); i++) {
            defs += (i ? " " : "") + classElement(p.classes[v.classes[i]]);
        }
        defs += "] ;\n";
    }
    for (const auto & s : p.inlineSets) {
        if (s.classes.size() == 1 && p.classes[s.classes[0]].inlineSet) continue;   // remains as written
        defs += "# " + s.text + " = [";
        for (size_t i = 0; i < s.classes.size(); i++) {
            defs += (i ? " " : "") + classElement(p.classes[s.classes[i]]);
        }
        defs += "]\n";
    }
    return (literals.empty() ? "" : "# literal characters:" + literals + "\n") + defs;
}

// Verify that the classes are disjoint, each variable is the union of its
// classes and the literal classes are single characters.  Returns an error
// message or the empty string.
static std::string verifyPartition(const CharacterClassPartition & p) {
    UCD::UnicodeSet all;
    for (const CharacterClass & c : p.classes) {
        if (c.chars->intersects(all)) return "classes intersect: " + classElement(c);
        all = all + *c.chars;
        if (c.literal && !(c.chars->size() == 1 && re::lo_codepoint(c.chars->front()) == re::hi_codepoint(c.chars->front()))) {
            return "literal class of more than one character";
        }
        if (!c.literal && !c.inlineSet && c.name.empty()) return "unnamed class";
    }
    for (const auto & v : p.variables) {
        UCD::UnicodeSet u;
        for (size_t i : v.classes) u = u + *p.classes[i].chars;
        if (!(u == *v.chars)) {
            return "variable $" + v.name + " is not the union of its classes: missing "
                + printUnicodeSet(re::makeCC(*v.chars - u)) + ", extra " + printUnicodeSet(re::makeCC(u - *v.chars));
        }
    }
    for (const auto & s : p.inlineSets) {
        UCD::UnicodeSet u;
        for (size_t i : s.classes) u = u + *p.classes[i].chars;
        if (!(u == *s.chars)) return "inline set " + s.text + " is not the union of its classes";
    }
    return "";
}

struct PartitionCounts {
    size_t groups = 0;
    size_t variables = 0;      // set variables (in each group)
    size_t divided = 0;        // set variables divided into several classes
    size_t inlineSets = 0;     // distinct inline sets
    size_t inlineDivided = 0;  // inline sets divided into several classes
    size_t classes = 0;
    size_t literals = 0;
    size_t generated = 0;      // classes with new variable names
    size_t inlineClasses = 0;  // classes denoted by inline sets as written
};

// The partitions of all groups, each preceded by a comment giving its rules
// (numbered from 1).
static std::string printPartitions(const std::vector<CharacterClassPartition> & partitions) {
    std::string s;
    for (size_t g = 0; g < partitions.size(); g++) {
        s += "# group " + std::to_string(g + 1) + ": rules " + std::to_string(partitions[g].firstRule + 1)
            + "-" + std::to_string(partitions[g].lastRule + 1) + "\n" + printPartition(partitions[g]);
    }
    return s;
}

static std::string verifyPartitions(const std::vector<CharacterClassPartition> & partitions) {
    for (size_t g = 0; g < partitions.size(); g++) {
        const std::string error = verifyPartition(partitions[g]);
        if (!error.empty()) return "group " + std::to_string(g + 1) + ": " + error;
    }
    return "";
}

static bool reportPartition(const std::vector<std::string> & tRules, const std::string & label, bool quiet, PartitionCounts & total) {
    const std::vector<CharacterClassPartition> partitions = partitionCharacterClasses(parseTransformRules(tRules));
    PartitionCounts n;
    for (const CharacterClassPartition & p : partitions) {
        n.variables += p.variables.size();
        n.classes += p.classes.size();
        for (const CharacterClass & c : p.classes) {
            n.literals += c.literal;
            n.inlineClasses += c.inlineSet;
            n.generated += !c.literal && !c.existing && !c.inlineSet;
        }
        for (const auto & v : p.variables) n.divided += v.classes.size() > 1;
        n.inlineSets += p.inlineSets.size();
        for (const auto & s : p.inlineSets) n.inlineDivided += s.classes.size() > 1;
    }
    const std::string error = verifyPartitions(partitions);
    std::cout << label << ": " << partitions.size() << " groups, " << n.variables << " set variables (" << n.divided << " divided), "
              << n.inlineSets << " inline sets (" << n.inlineDivided << " divided), " << n.classes << " classes: "
              << n.literals << " literal characters, " << (n.classes - n.literals - n.generated - n.inlineClasses) << " existing variables, "
              << n.inlineClasses << " inline sets, " << n.generated << " new variables" << (error.empty() ? "" : "; ERROR: " + error) << "\n";
    total.groups += partitions.size();
    if (!quiet) std::cout << printPartitions(partitions);
    total.variables += n.variables;
    total.divided += n.divided;
    total.inlineSets += n.inlineSets;
    total.inlineDivided += n.inlineDivided;
    total.classes += n.classes;
    total.literals += n.literals;
    total.inlineClasses += n.inlineClasses;
    total.generated += n.generated;
    return error.empty();
}

struct OverlapCounts {
    size_t rules = 0;
    size_t pairs = 0;
    size_t overlaps = 0;
};

static void reportOverlaps(const std::vector<std::string> & tRules, const std::string & label, bool quiet, OverlapCounts & total) {
    const std::vector<Rule *> parsed = parseTransformRules(tRules);
    for (const Extract e : {Extract::Forward, Extract::Backward}) {
        const std::vector<Rule *> rules = extract(parsed, e);
        OverlapCounts n;
        size_t groupRules = 0;
        for (const Rule * r : rules) {
            if (llvm::isa<TransformRule>(r)) {
                n.pairs += groupRules * (groupRules - (groupRules > 0)) / 2;
                groupRules = 0;
            } else if (llvm::isa<ConversionRule>(r)) {
                n.rules++;
                groupRules++;
            }
        }
        n.pairs += groupRules * (groupRules - (groupRules > 0)) / 2;
        const std::vector<RuleOverlap> overlaps = findRuleOverlaps(rules);
        n.overlaps = overlaps.size();
        if (n.rules == 0) continue;
        std::cout << label << (e == Extract::Forward ? " forward: " : " backward: ") << n.rules << " rules, "
                  << n.pairs << " pairs, " << n.overlaps << " overlapping\n";
        if (!quiet) {
            for (const RuleOverlap & o : overlaps) {
                std::cout << "    " << printRule(rules[o.earlier]) << "   |   " << printRule(rules[o.later]) << "\n";
            }
        }
        total.rules += n.rules;
        total.pairs += n.pairs;
        total.overlaps += n.overlaps;
    }
}

static void countTrivialCaptures(const std::vector<std::string> & tRules, const std::string & label, TrivialCaptureStats & total) {
    const std::vector<Rule *> rules = parseTransformRules(tRules);
    for (const Extract e : {Extract::Forward, Extract::Backward}) {
        TrivialCaptureStats stats;
        TrivialCaptureElimination(extract(rules, e), &stats);
        if (stats.candidates == 0) continue;
        std::cout << label << (e == Extract::Forward ? " forward: " : " backward: ") << stats.candidates << " candidates, "
                  << stats.transformed.size() << " transformed (" << stats.endPositionConflicts << " with end position conflicts), "
                  << stats.blockedByLaterRule << " blocked by later rules, " << stats.blockedWithinCapture << " blocked within captures\n";
        for (const Rule * r : stats.transformed) std::cout << "    transformed: " << printRule(r) << "\n";
        for (const auto & b : stats.blocked) {
            std::cout << "    blocked:     " << printRule(b.rule) << "\n";
            for (const Rule * r : b.laterRules) std::cout << "        later rule may match at start: " << printRule(r) << "\n";
            for (const Rule * r : b.withinRules) std::cout << "        rule may match within capture: " << printRule(r) << "\n";
        }
        total.candidates += stats.candidates;
        total.blockedByLaterRule += stats.blockedByLaterRule;
        total.blockedWithinCapture += stats.blockedWithinCapture;
        total.endPositionConflicts += stats.endPositionConflicts;
        total.transformed.insert(total.transformed.end(), stats.transformed.begin(), stats.transformed.end());
    }
}

struct EliminationTestCase {
    const char * input;
    const char * expected;
};

static const EliminationTestCase eliminationTestCases[] = {
    {"(a*) b → $1 x ;", "(a+) b → $1 x ;\nb → x ;\n"},
    // A capture of a fixed string is replaced by the string.
    {"(a?) b → $1 ;", "ab → a ;\nb → ;\n"},
    {"$m = m ; ($m?) z → $1 $1 ;", "$m = m ;\n$m z → $m $m ;\nz → ;\n"},
    {"([ab]?) z → $1 ;", "([ab]) z → $1 ;\nz → ;\n"},
    // Multiple nullable captures; remaining captures are renumbered.
    {"(a*) (b?) c → $2 $1 ;", "(a+) bc → b $1 ;\n(a+) c → $1 ;\nbc → b ;\nc → ;\n"},
    {"(x) (a*) (y) → $3 $2 $1 ;", "(x) (a+) (y) → $3 $2 $1 ;\n(x) (y) → $2 $1 ;\n"},
    {"(x) (a?) (y) → $3 $2 $1 ;", "(x) a (y) → $2 a $1 ;\n(x) (y) → $2 $1 ;\n"},
    // References within function calls and cursors.
    {"$v = [ab] ; x ($v*) → &Any-Hex($1) | y ;",
     "$v = [ab] ;\nx ($v+) → &Any-Hex($1) | y ;\nx → &Any-Hex() | y ;\n"},
    // Deleting a capture deletes the captures within it.
    {"((a)*) b → $2 $1 ;", "((a)+) b → $2 $1 ;\nb → ;\n"},
    // Captures in contexts.
    {"(x*) { y → z $1 ;", "(x+) { y → z $1 ;\ny → z ;\n"},
    // Variables defined as optional items.
    {"$v = [ab]* ; ($v) c → $1 ;", "$v = [ab]* ;\n([ab]+) c → $1 ;\nc → ;\n"},
    // Sequences of optional items: all combinations, from all present to none present.
    {"(a* b*) c → $1 ;", "(a+ b+) c → $1 ;\n(a+) c → $1 ;\n(b+) c → $1 ;\nc → ;\n"},
    {"$m = m ; $d = d ; (x) ($m? $d?) y → $2 $1 ;",
     "$m = m ;\n$d = d ;\n(x) $m $d y → $m $d $1 ;\n(x) $m y → $m $1 ;\n(x) $d y → $d $1 ;\n(x) y → $1 ;\n"},
    {"$V = [aeiou] ; z ([jw]? $V?) → $1 ;",
     "$V = [aeiou] ;\nz ([jw] $V) → $1 ;\nz ([jw]) → $1 ;\nz ($V) → $1 ;\nz → ;\n"},
    {"((a)* b*) c → $2 $1 ;", "((a)+ b+) c → $2 $1 ;\n((a)+) c → $2 $1 ;\n(b+) c → $1 ;\nc → ;\n"},
    {"z { (a? b? c?) → $1 ;",
     "z { abc → abc ;\nz { ab → ab ;\nz { ac → ac ;\nz { a → a ;\n"
     "z { bc → bc ;\nz { b → b ;\nz { c → c ;\nz { → ;\n"},
    {"(x? y?) (z*) → $2 $1 ;",
     "xy (z+) → $1 xy ;\nxy → xy ;\nx (z+) → $1 x ;\nx → x ;\ny (z+) → $1 y ;\ny → y ;\n(z+) → $1 ;\n→ ;\n"},
    // Unchanged: captures within repetitions, non-nullable captures, non-forward rules.
    {"((a*) b)* → x ;", "((a*) b)* → x ;\n"},
    {"(a+) b → $1 ;", "(a+) b → $1 ;\n"},
    {"(a b?) c → $1 ;", "(a b?) c → $1 ;\n"},
    {"(a*) ↔ b ;", "(a*) ↔ b ;\n"},
};

static const EliminationTestCase trivialTestCases[] = {
    {"a { (b) } c → $1 x ;", "ab { } c → x ;\n"},
    {"(b) → $1 x | y ;", "b { → x | y ;\n"},
    {"(b) → $1 ;", "b { → ;\n"},
    {"(b+) → $1 x @| ;", "b+ { → x @| ;\n"},
    // The capture is retained if referenced elsewhere in the result.
    {"(b) → $1 x $1 ;", "(b) { → x $1 ;\n"},
    {"(b) → $1 x | &Any-Hex($1) ;", "(b) { → x | &Any-Hex($1) ;\n"},
    // Other captures are renumbered.
    {"(a) { (b) } (c) → $2 $3 $1 ;", "(a) b { } (c) → $2 $1 ;\n"},
    {"{ ((a)(b)) } → $1 $3 $2 ;", "(a) (b) { → $2 $1 ;\n"},
    {"{ ((a)(b)) } → $1 $1 $3 ;", "((a) (b)) { → $1 $3 ;\n"},
    {"{ ((a)(b)) } → $1 $3 ;", "(a) (b) { → $2 ;\n"},
    // Not transformed when a later rule of the group may match at the start
    // of X, or any other rule within X.  Properties are resolved; rules whose
    // text to replace may be empty may match anywhere.
    {"a { (b) } → $1 x ; b → y ;", "a { (b) → $1 x ;\nb → y ;\n"},
    {"b → y ; (b) → $1 x ;", "b → y ;\nb { → x ;\n"},
    {"(bc) → $1 x ; c → y ;", "(bc) → $1 x ;\nc → y ;\n"},
    {"c → y ; (bc) → $1 x ;", "c → y ;\n(bc) → $1 x ;\n"},
    {"(bc) → $1 x ; d → y ;", "bc { → x ;\nd → y ;\n"},
    {"(b) → $1 x ; :: Null ; b → y ;", "b { → x ;\n:: Null ;\nb → y ;\n"},
    {"(b) → $1 x ; [:L:] → y ;", "(b) → $1 x ;\n[:l:] → y ;\n"},
    {"(b) → $1 x ; [:Nd:] → y ;", "b { → x ;\n[:nd:] → y ;\n"},
    {"(b) → $1 x ; { } c → y ;", "b { → x ;\n} c → y ;\n"},
    {"(b) → $1 x ; { } b → y ;", "(b) → $1 x ;\n} b → y ;\n"},
    // Contexts are considered.
    {"x { (b) } → $1 z ; y { b → w ;", "xb { → z ;\ny { b → w ;\n"},
    {"[xy] { (b) } → $1 z ; y { b → w ;", "[xy] { (b) → $1 z ;\ny { b → w ;\n"},
    {"(b) } [$] → $1 z ; b } \\. → w ;", "b { } [$] → z ;\nb } \\. → w ;\n"},
    {"(bc) → $1 z ; x { c → w ;", "bc { → z ;\nx { c → w ;\n"},
    {"(bc) → $1 z ; b { c → w ;", "(bc) → $1 z ;\nb { c → w ;\n"},
    {"(b) → $1 x ; b ← y ;", "b { → x ;\nb ← y ;\n"},
    // Unchanged: reference not first, text not only the capture, not a forward rule.
    {"(b) → x $1 ;", "(b) → x $1 ;\n"},
    {"(b) c → $1 ;", "(b) c → $1 ;\n"},
    {"(b) → &Any-Hex($1) ;", "(b) → &Any-Hex($1) ;\n"},
    {"(b) ↔ $1 x ;", "(b) ↔ $1 x ;\n"},
    {"$1 x ← (b) ;", "$1 x ← (b) ;\n"},
};

struct OverlapTestCase {
    const char * rules;     // two rules
    bool overlap;
};

static const OverlapTestCase overlapTestCases[] = {
    {"a → x ; a → y ;", true},
    {"a → x ; b → y ;", false},
    {"ab → x ; a → y ;", true},
    {"abc → x ; ad → y ;", false},
    {"a } b → x ; a } c → y ;", false},
    {"a } [bc] → x ; a } c → y ;", true},
    {"a } [$] → x ; a } \\. → y ;", false},
    {"a } $ → x ; ab → y ;", false},
    {"a } $ → x ; a* → y ;", true},
    {"x { a → 1 ; y { a → 2 ;", false},
    {"[xy] { a → 1 ; y { a → 2 ;", true},
    {"^ { a → 1 ; b { a → 2 ;", false},
    {"b* { a → 1 ; ^ { a → 2 ;", true},
    {"[^b] { a → 1 ; b { a → 2 ;", false},
    {"[^b] { a → 1 ; ^ { a → 2 ;", true},
    {"^ a → 1 ; b { a → 2 ;", false},
    {"^ a → 1 ; [^b] { a → 2 ;", true},
    {"a } [^b] → 1 ; a } $ → 2 ;", true},
    {"a } [^b] → 1 ; ab → 2 ;", false},
    {"(a*) b → 1 ; b → 2 ;", true},
    {"[:Lu:] → 1 ; [:Ll:] → 2 ;", false},
    {"[:Greek:] → 1 ; α → 2 ;", true},
    {"{ } c → 1 ; b → 2 ;", false},
    {"{ } c → 1 ; c → 2 ;", true},
};

static const EliminationTestCase partitionTestCases[] = {
    // Only the variables used by the rules are partitioned ($front is not used).
    {"$vowel = [aeiou] ; $front = [ei] ; $m = m ; $vowel $m → x ; a → b ;",
     "# group 1: rules 1-5\n# literal characters: a b x\n$vowel_1 = [eiou] ;\n$m = m ;\n$vowel = [a $vowel_1] ;\n"},
    // Overlapping sets are divided into their common and distinct parts.
    {"$A = [a-d] ; $B = [c-f] ; $A $B → x ;",
     "# group 1: rules 1-3\n# literal characters: x\n$A_1 = [ab] ;\n$A_2 = [cd] ;\n$B_1 = [ef] ;\n$A = [$A_1 $A_2] ;\n$B = [$A_2 $B_1] ;\n"},
    // Variables used only within the definitions of set variables are not partitioned.
    {"$LO = [ace] ; $HI = [bdf] ; $W = f ; $C = [$LO $HI] ; $F = $C ; $C $W → y ;",
     "# group 1: rules 1-6\n# literal characters: y\n$C_1 = [a-e] ;\n$W = f ;\n$C = [$C_1 $W] ;\n"},
    // Characters of strings in sets are literal; properties are resolved.
    {"$v = [{ch}a-c] ; $v → x ;", "# group 1: rules 1-2\n# literal characters: c h x\n$v_1 = [ab] ;\n$v = [$v_1 c] ;\n"},
    {"$d = [:Nd:] ; $d → 1 ;", "# group 1: rules 1-2\n# literal characters: 1\n$d_1 = [02-9٠-٩۰-۹߀-߉"},
    // Inline sets, including sets within variable definitions that are not
    // sets; inline sets that are not divided remain as written.
    {"$v = [a-e] ; $v [cx] → 1 ; $sp = [fg]* ; $sp → 2 ;",
     "# group 1: rules 1-4\n# literal characters: 1 2\n$v_1 = [abde] ;\n$v_2 = c ;\n$set_1 = x ;\n$v = [$v_1 $v_2] ;\n"
     "# [cx] = [$v_2 $set_1]\n"},
    {"$v = [a-e] ; $v → 1 ; [cd] → 2 ;", "# group 1: rules 1-3\n# literal characters: 1 2\n$v_1 = [abe] ;\n$v = [$v_1 [cd]] ;\n"},
    // Variables whose sets include strings, or variables defined as strings.
    {"$d = [{ab}{cd}] ; $v = [x y $d] ; $v → 1 ;", "# group 1: rules 1-3\n# literal characters: 1 a b c d\n$v = [xy] ;\n"},
    {"$a = [ab] ; $s = [{xy}{zw}] ; $m = [$a $s c] ; $m → 1 ;",
     "# group 1: rules 1-4\n# literal characters: 1 w x y z\n$m = [a-c] ;\n"},
    // Groups are partitioned separately; filter and transform rules are excluded.
    {"[ab] → 1 ; :: [a-c] Upper ; [ab] → 2 ;",
     "# group 1: rules 1-1\n# literal characters: 1\n# group 2: rules 3-3\n# literal characters: 2\n"},
    // Variables defined in an earlier group and used in a later one, including
    // through a variable that is not a set.
    {"$v = [a-d] ; $w = [cd] ; $s = $v+ x ; :: NFD ; $s → 1 ; :: Null ; [b-e] → 2 ;",
     "# group 1: rules 1-3\n# group 2: rules 5-5\n# literal characters: 1 x\n$v = [a-d] ;\n"
     "# group 3: rules 7-7\n# literal characters: 2\n"},
};

struct DirectionTestCase {
    const char * input;
    const char * forward;
    const char * backward;
};

static const DirectionTestCase directionTestCases[] = {
    // The Inverse Summary example of UTS #35.
    {":: [:Uppercase Letter:] ; :: latin-greek ; :: greek-japanese ; x ↔ y ; z → w ; r ← m ;"
     " :: upper; a → b ; c ↔ d ; :: any-publishing ; :: ([:Number:]) ;",
     ":: [:uppercaseletter:] ;\n:: latin-greek ;\n:: greek-japanese ;\nx → y ;\nz → w ;\n"
     ":: upper ;\na → b ;\nc → d ;\n:: any-publishing ;\n",
     ":: [:number:] ;\n:: publishing-any ;\nd → c ;\n:: Lower ;\ny → x ;\nm → r ;\n"
     ":: japanese-greek ;\n:: greek-latin ;\n"},
    // Contexts and cursors are ignored on the sides where they do not belong.
    {"a { b | c } d ↔ e { f | g } h ;", "a { bc } d → f | g ;\n", "e { fg } h → b | c ;\n"},
    {"x → Ab@| ; |@ab ← [a-z] { y ;", "x → Ab @| ;\n", "[a-z] { y → |@ ab ;\n"},
    // Segments and references.
    {"$1 ← (x) ; (y) → &Any-Hex($1) ;", "(y) → &Any-Hex($1) ;\n", "(x) → $1 ;\n"},
    // Explicit inverses and filters of transform rules.
    {":: NFD (NFC) ; :: [a-z] Upper () ; :: ([A-Z] Lower) ;",
     ":: NFD ;\n:: [a-z] Upper ;\n", ":: [A-Z] Lower ;\n:: NFC ;\n"},
    // Transform rules not applying in a direction still separate groups.
    {"a ↔ b ; :: X () ; c ↔ d ; :: (Y) ; e ↔ f ;",
     "a → b ;\n:: X ;\nc → d ;\n:: Null ;\ne → f ;\n",
     "f → e ;\n:: Y ;\nd → c ;\n:: Null ;\nb → a ;\n"},
    {"a → b ; :: (Lower) ; c → d ;", "a → b ;\n:: Null ;\nc → d ;\n", ":: Lower ;\n"},
    // Only the variables used in each direction are retained, including
    // variables used in the definitions of used variables.
    {"$a = x ; $b = [$a y] ; $c = z ; $u = u ; $b → q ; r ← $c ;",
     "$a = x ;\n$b = [$a y] ;\n$b → q ;\n", "$c = z ;\n$c → r ;\n"},
    {"$t = [a-z] ; :: [$t] Upper () ; $n = n ; n ← $n ;",
     "$t = [a-z] ;\n:: [$t] Upper ;\n", "$n = n ;\n$n → n ;\n"},
    {"$h = h ; $g = [:L:] ; (x) → &Any-Hex($1 $h) ; $g { y ← z ;",
     "$h = h ;\n(x) → &Any-Hex($1 $h) ;\n", "z → y ;\n"},
    // Variable definitions precede the reversed rules.
    {"$v = [ab] ; $v ↔ x ; $w = y ; :: Null ; $w ↔ z ; :: ([:L:]) ;",
     "$v = [ab] ;\n$v → x ;\n$w = y ;\n:: Null ;\n$w → z ;\n",
     ":: [:l:] ;\n$v = [ab] ;\n$w = y ;\nz → $w ;\n:: Null ;\nx → $v ;\n"},
};

static bool checkOutput(const char * label, const char * input, const std::string & actual, const char * expected, bool exact = true) {
    bool ok = actual == expected;
    std::string reprinted;
    if (ok) {
        ok = reparses(actual, exact, reprinted);
    }
    if (!ok) {
        std::cerr << "FAIL (" << label << "): " << input << "\n  expected: " << expected
                  << "  actual:   " << actual << (reprinted.empty() ? "" : "  reprinted: " + reprinted);
    }
    return ok;
}

static int runSelfTest() {
    unsigned failures = 0;
    unsigned count = 0;
    for (const TestCase & t : testCases) {
        count++;
        std::string actual;
        bool error = false;
        try {
            actual = printRules(parseTransformRules({t.input}));
        } catch (const TransformRuleParseError & e) {
            error = true;
            actual = e.what();
        }
        bool ok;
        if (t.expected == nullptr) {
            ok = error;
        } else {
            ok = !error && actual == t.expected;
            if (ok) {
                // The canonical form must reparse to itself.
                try {
                    ok = printRules(parseTransformRules({actual})) == actual;
                } catch (const TransformRuleParseError & e) {
                    ok = false;
                    actual = std::string("reparse failed: ") + e.what();
                }
            }
        }
        if (!ok) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  expected: "
                      << (t.expected ? t.expected : "(parse error)\n")
                      << "  actual:   " << actual << (error ? "\n" : "");
        }
    }
    for (const DirectionTestCase & t : directionTestCases) {
        count += 2;
        try {
            const std::vector<Rule *> rules = parseTransformRules({t.input});
            failures += !checkOutput("forward", t.input, printRules(ExtractForwardRules(rules)), t.forward);
            failures += !checkOutput("backward", t.input, printRules(ExtractReverseBackwardRules(rules)), t.backward);
        } catch (const TransformRuleParseError & e) {
            failures += 2;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    for (const EliminationTestCase & t : partitionTestCases) {
        count++;
        try {
            const std::vector<CharacterClassPartition> p = partitionCharacterClasses(parseTransformRules({t.input}));
            const std::string actual = printPartitions(p);
            const std::string error = verifyPartitions(p);
            // A prefix of the output is expected when the classes are large.
            if (!error.empty() || actual.compare(0, std::strlen(t.expected), t.expected) != 0) {
                failures++;
                std::cerr << "FAIL (partition): " << t.input << "\n  expected: " << t.expected << "\n  actual:   " << actual
                          << (error.empty() ? "" : "  error: " + error + "\n");
            }
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    for (const OverlapTestCase & t : overlapTestCases) {
        count++;
        try {
            const std::vector<Rule *> rules = parseTransformRules({t.rules});
            RuleOverlapAnalysis analysis;
            const bool overlap = analysis.mayOverlap(llvm::cast<ConversionRule>(rules[0]), llvm::cast<ConversionRule>(rules[1]));
            const bool reverse = analysis.mayOverlap(llvm::cast<ConversionRule>(rules[1]), llvm::cast<ConversionRule>(rules[0]));
            if (overlap != t.overlap || reverse != t.overlap) {
                failures++;
                std::cerr << "FAIL (overlap): " << t.rules << "\n  expected: " << t.overlap << " actual: " << overlap << " " << reverse << "\n";
            }
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.rules << "\n  " << e.what() << "\n";
        }
    }
    for (const EliminationTestCase & t : trivialTestCases) {
        count++;
        try {
            const std::vector<Rule *> rules = TrivialCaptureElimination(parseTransformRules({t.input}));
            failures += !checkOutput("trivial", t.input, printRules(rules), t.expected);
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    // Trivial capture elimination followed by nullable capture elimination.
    {
        count++;
        const char * input = "w { ($v?) } y → z $1 ; x { ($v?) } y → $1 z ;";
        const char * expected = "w { $v } y → z $v ;\nw { } y → z ;\nx $v? { } y → z ;\n";
        try {
            TransformRuleParser parser;
            parser.parse("$v = v ;");
            std::vector<Rule *> rules = parser.parse(input);
            rules = NullableCaptureElimination(TrivialCaptureElimination(rules));
            const std::string actual = printRules(rules);
            if (actual != expected) {
                failures++;
                std::cerr << "FAIL (trivial+nullable): " << input << "\n  expected: " << expected << "  actual:   " << actual;
            }
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << input << "\n  " << e.what() << "\n";
        }
    }
    for (const EliminationTestCase & t : eliminationTestCases) {
        count++;
        try {
            const std::vector<Rule *> rules = NullableCaptureElimination(parseTransformRules({t.input}));
            failures += !checkOutput("elimination", t.input, printRules(rules), t.expected);
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    std::cout << (count - failures) << "/" << count << " tests passed\n";
    return failures == 0 ? 0 : 1;
}

int main(int argc, char * argv[]) {
    bool xml = false;
    bool quiet = false;
    Extract e = Extract::All;
    bool eliminate = false;
    bool eliminateTrivial = false;
    bool countTrivial = false;
    bool overlaps = false;
    bool partition = false;
    bool count = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--self-test") return runSelfTest();
        else if (arg == "--xml") xml = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--forward") e = Extract::Forward;
        else if (arg == "--backward") e = Extract::Backward;
        else if (arg == "--eliminate-nullable-captures") eliminate = true;
        else if (arg == "--eliminate-trivial-captures") eliminateTrivial = true;
        else if (arg == "--count-trivial-captures") countTrivial = true;
        else if (arg == "--overlaps") overlaps = true;
        else if (arg == "--partition") partition = true;
        else if (arg == "--count-nullable-captures") count = true;
        else files.push_back(arg);
    }
    if (files.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--xml] [--quiet] [--forward | --backward]\n"
                  << "           [--eliminate-trivial-captures] [--eliminate-nullable-captures] file ...\n"
                  << "       " << argv[0] << " [--xml] [--quiet] --overlaps file ...\n"
                  << "       " << argv[0] << " [--xml] [--quiet] --partition file ...\n"
                  << "       " << argv[0] << " [--xml] --count-trivial-captures file ...\n"
                  << "       " << argv[0] << " [--xml] --count-nullable-captures file ...\n"
                  << "       " << argv[0] << " --self-test\n";
        return 2;
    }
    bool ok = true;
    NullableCounts total;
    TrivialCaptureStats trivialTotal;
    OverlapCounts overlapTotal;
    PartitionCounts partitionTotal;
    for (const std::string & f : files) {
        try {
            const std::string text = readFile(f);
            const std::vector<std::string> tRules = xml ? extractTRules(text) : std::vector<std::string>{text};
            if (partition) {
                ok &= reportPartition(tRules, f, quiet, partitionTotal);
            } else if (overlaps) {
                reportOverlaps(tRules, f, quiet, overlapTotal);
            } else if (countTrivial) {
                countTrivialCaptures(tRules, f, trivialTotal);
            } else if (count) {
                countNullableCaptures(tRules, f, total);
            } else {
                ok &= processRules(tRules, f, quiet, e, eliminateTrivial, eliminate);
            }
        } catch (const std::exception & e) {
            std::cerr << f << ": " << e.what() << "\n";
            ok = false;
        }
    }
    if (partition) {
        std::cout << "Total: " << partitionTotal.groups << " groups, " << partitionTotal.variables << " set variables (" << partitionTotal.divided << " divided), "
                  << partitionTotal.inlineSets << " inline sets (" << partitionTotal.inlineDivided << " divided), "
                  << partitionTotal.classes << " classes: " << partitionTotal.literals << " literal characters, "
                  << (partitionTotal.classes - partitionTotal.literals - partitionTotal.generated - partitionTotal.inlineClasses) << " existing variables, "
                  << partitionTotal.inlineClasses << " inline sets, " << partitionTotal.generated << " new variables\n";
    }
    if (overlaps) {
        std::cout << "Total: " << overlapTotal.rules << " rules, " << overlapTotal.pairs << " pairs, "
                  << overlapTotal.overlaps << " overlapping\n";
    }
    if (countTrivial) {
        std::cout << "Total: " << trivialTotal.candidates << " candidates, " << trivialTotal.transformed.size()
                  << " transformed (" << trivialTotal.endPositionConflicts << " with end position conflicts), "
                  << trivialTotal.blockedByLaterRule << " blocked by later rules, "
                  << trivialTotal.blockedWithinCapture << " blocked within captures\n";
    }
    if (count) {
        std::cout << "Total: " << total.captures << " nullable captures in " << total.rules << " rules of "
                  << total.files << " files (+" << total.forwardAdded << " forward rules, +"
                  << total.backwardAdded << " backward rules); " << total.repeated << " within repetitions\n";
    }
    return ok ? 0 : 1;
}

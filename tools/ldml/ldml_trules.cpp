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
//      (forward) rules before printing.  With --mutually-exclusive, the
//      rules are then expressed in terms of the mutually exclusive
//      character classes of each group.  With --disambiguate-order, the
//      order dependencies of the rules are then resolved where possible.
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
//  ldml_trules [--xml] --classify-after-contexts file ...
//      Classify the after contexts of the conversion rules (forward and
//      backward, after DisambiguateOrder) by their support in the regular
//      expression engine as lookaheads: supported (+), unsupported with a
//      known limitation (b) or unsupported otherwise (c).  Report the rule
//      counts and the distinct after contexts of each class.  With the
//      SHOW_BODIES environment variable set, the prepared lookahead bodies of
//      unsupported after contexts are printed to stderr.
//  ldml_trules [--xml] [--quiet] --classify-rules file ...
//      Classify the conversion rules (forward and backward, as extracted
//      forward rules) by the length of the text to replace (0, 1, 2,
//      fixed > 2, variable), the number of captures, the order of the
//      references to the captures in the result, and the revisiting status
//      of the result (cursor).  Report the counts for each, and the combined
//      classes with an example of each (listing each rule unless --quiet).
//      Also report the simple insertion rules (a captured text, replaced by
//      fixed text, the reference to the capture and fixed text), by the kind
//      of insertion and the length of the captured text.
//  ldml_trules --self-test
//      Run the built-in test cases.

#include <ldml/transform_rules.h>
#include <ldml/transform_rules_parser.h>
#include <ldml/transform_rules_printer.h>
#include <re/adt/adt.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_transformer.h>
#include <re/unicode/regex_passes.h>
#include <re/printer/re_printer.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <fstream>
#include <iostream>
#include <sstream>
#include <tuple>

using namespace ldml;

static std::string readFile(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open " + path);
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
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

// Each error of the file on its own line, prefixed by the file name.
static void reportErrors(const std::string & label, const TransformRuleParseErrors & e) {
    for (const TransformRuleParseError & err : e.getErrors()) {
        std::cerr << label << ": " << err.what() << "\n";
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
                         bool eliminateTrivial, bool eliminateNullable, bool exclusive, bool disambiguate) {
    try {
        std::vector<Rule *> rules = extract(parseTransformRules(tRules), e);
        if (eliminateTrivial) rules = TrivialCaptureElimination(rules);
        if (eliminateNullable) rules = NullableCaptureElimination(rules);
        MutuallyExclusiveStats stats;
        if (exclusive) {
            rules = MutuallyExclusivePartitioning(rules, &stats);
            if (stats.mismatches) {
                std::cerr << label << ": " << stats.mismatches << " sets replaced by classes with different characters\n";
                return false;
            }
        }
        DisambiguationStats dstats;
        if (disambiguate) {
            rules = DisambiguateOrder(rules, &dstats);
            if (dstats.verificationFailures) {
                std::cerr << label << ": " << dstats.verificationFailures << " replacement rules overlap resolved rules\n";
                for (const auto & f : dstats.failedPairs) {
                    std::cerr << "    " << printRule(f.first) << "   |   " << printRule(f.second) << "\n";
                }
                return false;
            }
        }
        const std::string printed = printRules(rules);
        if (!quiet) std::cout << printed;
        std::string reprinted;
        if (!reparses(printed, !exclusive, reprinted)) {
            std::cerr << label << ": round trip mismatch\n--- printed\n" << printed << "--- reprinted\n" << reprinted;
            return false;
        }
        if (quiet) {
            std::cout << label << ": " << rules.size() << " rules OK";
            if (exclusive) {
                std::cout << " (" << stats.groups << " groups, " << stats.setsRewritten << " sets rewritten, "
                          << stats.classDefinitions << " class variables, " << stats.variableCopies << " variable copies)";
            }
            if (disambiguate) {
                std::cout << " (overlaps " << dstats.overlapsBefore << " -> " << dstats.overlapsAfter << ", "
                          << dstats.rulesSplit << " rules split into " << dstats.splitRules << ", "
                          << dstats.pairsResolved << " pairs resolved, " << dstats.pairsUnresolved << " unresolved, "
                          << dstats.rulesReplaced << " rules replaced by " << dstats.rulesAdded;
                if (dstats.deletionClosures) std::cout << ", " << dstats.deletionClosures << " deletion closures";
                std::cout << ")";
                for (const auto & r : dstats.unresolvedReasons) {
                    std::cout << "\n    unresolved: " << r.second << " " << r.first;
                }
            }
            std::cout << "\n";
        }
        return true;
    } catch (const TransformRuleParseErrors & e) {
        reportErrors(label, e);
        return false;
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
    {"'ab'+ → c ;", "'ab'+ → c ;\n"},
    {"([a-b]) > &hex($1);", "([ab]) → &hex($1) ;\n"},
    {"([a-b]) > &Any-Hex/Unicode($1);", "([ab]) → &Any-Hex/Unicode($1) ;\n"},
    {"$1 ← (x) ;", "$1 ← (x) ;\n"},
    {"(a)(b) ↔ $2 $1 ;", "(a) (b) ↔ $2 $1 ;\n"},
    {"a } $ → b ;", "a } $ → b ;\n"},
    // A set matches the text boundary in the text only as its last item (as in ICU).
    {"[$] b → c ;", "[] b → c ;\n"},
    {"[^a] → y ; a [^a] → z ;", "[^a$] → y ;\na [^a] → z ;\n"},
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
    {"[:separator:]* → ' ' ;", nullptr},    // matches the empty text without contexts
    {"$e = ; $e → x ;", nullptr},
    {"x ← (a?) ;", nullptr},
    {"(a*) ↔ b ;", nullptr},
    {"a ([bc])+ → $1 ;", nullptr},           // a segment within a repetition referenced
    {"x $1 ← ([bc])* y ;", nullptr},
    {"a ([bc])+ → x ;", "a ([bc])+ → x ;\n"},
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
    {"((a)?) b → $2 $1 ;", "((a)) b → $2 $1 ;\nb → ;\n"},
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
    {"((a)? b*) c → $2 $1 ;", "((a) b+) c → $2 $1 ;\n((a)) c → $2 $1 ;\n(b+) c → $1 ;\nc → ;\n"},
    {"z { (a? b? c?) → $1 ;",
     "z { abc → abc ;\nz { ab → ab ;\nz { ac → ac ;\nz { a → a ;\n"
     "z { bc → bc ;\nz { b → b ;\nz { c → c ;\nz { → ;\n"},
    {"(x? y?) (z*) w → $2 $1 ;",
     "xy (z+) w → $1 xy ;\nxyw → xy ;\nx (z+) w → $1 x ;\nxw → x ;\ny (z+) w → $1 y ;\nyw → y ;\n(z+) w → $1 ;\nw → ;\n"},
    // Unchanged: captures within repetitions, non-nullable captures, non-forward rules.
    {"((a*) b)+ → x ;", "((a*) b)+ → x ;\n"},
    {"(a+) b → $1 ;", "(a+) b → $1 ;\n"},
    {"(a b?) c → $1 ;", "(a b?) c → $1 ;\n"},
    {"(a*) ← b ;", "(a*) ← b ;\n"},
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
    {"a } $ → x ; a+ → y ;", true},
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
    {"$v = [a-e] ; $v [cx] → 1 ; $sp = [fg]+ ; $sp → 2 ;",
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

static const EliminationTestCase exclusiveTestCases[] = {
    // Divided variables are replaced by the unions of their classes.
    {"$A = [a-d] ; $B = [c-f] ; $A $B → x ;",
     "$A = [a-d] ;\n$B = [c-f] ;\n$A_1 = [ab] ;\n$A_2 = [cd] ;\n$B_1 = [ef] ;\n[$A_1$A_2] [$A_2$B_1] → x ;\n"},
    // Variables that are exactly one class, and inline sets that are not divided, remain.
    {"$v = [a-e] ; $w = [xy] ; $v $w → 1 ; [cd] → 2 ; [pq] → 3 ;",
     "$v = [a-e] ;\n$w = [xy] ;\n$v_1 = [abe] ;\n[$v_1 cd] $w → 1 ;\n[cd] → 2 ;\n[pq] → 3 ;\n"},
    // Literal characters are classes; strings and the text boundary are retained.
    {"$v = [{ch}a-c] ; $v → x ; [a-d$] { y → z ;",
     "$v = [{ch}a-c] ;\n$v_1 = [ab] ;\n$set_1 = d ;\n[$v_1{ch}c] → x ;\n[$v_1$set_1 c$] { y → z ;\n"},
    // Variables that are not sets are copied with their sets rewritten.
    {"$v = [a-d] ; $s = $v+ x ; $s → 1 ; [bc] → 2 ;",
     "$v = [a-d] ;\n$s = $v+ x ;\n$v_1 = [ad] ;\n$s_g1 = [$v_1 bc]+ x ;\n$s_g1 → 1 ;\n[bc] → 2 ;\n"},
    // Groups are rewritten separately; captures and references are retained.
    {"$v = [a-d] ; ($v) b → $1 ; :: Null ; $v → 2 ;",
     "$v = [a-d] ;\n$v_1 = [acd] ;\n([$v_1 b]) b → $1 ;\n:: Null ;\n$v → 2 ;\n"},
};

static const EliminationTestCase disambiguationTestCases[] = {
    // The Han-Latin case: an optional item then a required item.
    {"a } \\u0020? b → x ; [ac] → y ;",
     "a } \\u0020? b → x ;\nc → y ;\na } [^\\u0020b] → y ;\na } \\u0020 [^b] → y ;\n"},
    // A longer key: the longest match idiom.
    {"ab → x ; a → y ;", "ab → x ;\na } [^b] → y ;\n"},
    {"abc → x ; a → y ;", "abc → x ;\na } [^b] → y ;\na } b [^c] → y ;\n"},
    // Before contexts.
    {"b { a → x ; [ac] → y ;", "b { a → x ;\nc → y ;\n[^b] { a → y ;\n"},
    {"c b? { a → x ; a → y ;", "c b? { a → x ;\n[^bc] { a → y ;\n[^c] b { a → y ;\n"},
    // A rule without context masks its key.
    {"a → x ; [ab] → y ;", "a → x ;\nb → y ;\n"},
    // Several earlier rules with distinct keys.
    {"a } b → x ; c } d → z ; [ac] → y ;", "a } b → x ;\nc } d → z ;\na } [^b] → y ;\nc } [^d] → y ;\n"},
    // Sets and variables.
    {"$v = [bc] ; a } $v → x ; [ae] → y ;", "$v = [bc] ;\na } $v → x ;\ne → y ;\na } [^$v] → y ;\n"},
    // Earlier rules with items on both sides; not handled: optional items
    // overlapping following items.
    {"c { a } b → x ; a → y ;", "c { a } b → x ;\na } [^b] → y ;\n[^c] { a } b → y ;\n"},
    {"c { a } b → x ; a } b → y ;", "c { a } b → x ;\n[^c] { a } b → y ;\n"},
    {"c { a } b → x ; ab → z ; a → y ;", "c { a } b → x ;\n[^c] { ab → z ;\na } [^b] → y ;\n"},
    // Earlier rules with shared keys are explored together.
    {"a } b → x ; a } c → z ; a → y ;", "a } b → x ;\na } c → z ;\na } [^bc] → y ;\n"},
    {"ab → x ; ac → z ; a → y ;", "ab → x ;\nac → z ;\na } [^bc] → y ;\n"},
    // Later rules with longer keys.
    {"abc → x ; ab → y ;", "abc → x ;\nab } [^c] → y ;\n"},
    {"abcd → x ; ab → y ;", "abcd → x ;\nab } [^c] → y ;\nab } c [^d] → y ;\n"},
    // Segments of earlier rules are disregarded.
    {"(a) } b → $1 x ; a → y ;", "(a) } b → $1 x ;\na } [^b] → y ;\n"},
    // Earlier rules on both sides of the position.
    {"a } b → x ; c { a → z ; a → y ;", "a } b → x ;\nc { a } [^b] → z ;\n[^c] { a } [^b] → y ;\n"},
    // Later rules with contexts: the pieces are within the contexts of L.
    {"ab → x ; a } [bc] → y ;", "ab → x ;\na } c → y ;\n"},
    {"b { a → x ; [bc] { a → y ;", "b { a → x ;\nc { a → y ;\n"},
    {"a } b → x ; c { a → y ;", "a } b → x ;\nc { a } [^b] → y ;\n"},
    {"b { a → x ; d { a } e → y ;", "b { a → x ;\nd { a } e → y ;\n"},
    {"a } b → x ; a } b c → y ;", "a } b → x ;\n"},
    {"a } b c → x ; a } b → y ;", "a } bc → x ;\na } b [^c] → y ;\n"},
    // Sets with strings or the text boundary in L are split into their alternatives.
    {"ch → x ; [{ch}c] → y ;", "ch → x ;\nc } [^h] → y ;\n"},
    {"c → x ; [{ch}{qu}ckq] → k ;", "c → x ;\nqu → k ;\nk → k ;\nq } [^u] → k ;\n"},
    {"a { b → x ; [a$] { b → y ;", "a { b → x ;\n[$] { b → y ;\n"},
    {"a } b → x ; a } [b$] → y ;", "a } b → x ;\na } [$] → y ;\n"},
    // Insertion rules (empty texts to replace), earlier and later.
    {"a { } b → x ; b → y ;", "a { } b → x ;\n[^a] { b → y ;\n"},
    {"} b → x ; b → y ;", "} b → x ;\n"},
    {"a? { } b → x ; bc → y ;", "a? { } b → x ;\n"},
    {"ab → x ; a { } b → y ;", "ab → x ;\na { } b → y ;\n"},
    {"a } c → x ; { } b → y ;", "a } c → x ;\n} b → y ;\n"},
    {"b → x ; a { } → y ;", "b → x ;\na { } [^b] → y ;\n"},
    // ^ at the start of the text to replace of E is a condition preceding it.
    {"^ k → g ; k → y ;", "^ k → g ;\n[:any:] { k → y ;\n"},
    // The text boundary as an item of L.
    {"a } b → x ; ^ a → y ;", "a } b → x ;\n^ { a } [^b] → y ;\n"},
    {"a } [b$] → x ; a } $ → y ;", "a } [b$] → x ;\n"},
    // Repetitions in L: each item is replaced by the classes it matches.
    {"ab → x ; a+ → y ;", "ab → x ;\naa a* → y ;\na } [^ab] → y ;\n"},
    {"ab → x ; a c? → y ;", "ab → x ;\nac → y ;\na } [^bc] → y ;\n"},
    {"b { a → x ; c* { a → y ;", "b { a → x ;\nc* c { a → y ;\n[^bc] { a → y ;\n"},
    {"ab → x ; (a+) → $1 ;", "ab → x ;\n(aa a*) → $1 ;\n(a) } [^ab] → $1 ;\n"},
    {"ab → x ; a+ a → y ;", "ab → x ;\na+ a → y ;\n"},
    // A later rule matched possessively: [ab]? b matches only ab and bb.
    {"bc → x ; [ab]? b → y ;", "bc → x ;\nab → y ;\nbb → y ;\n"},
    // Segments in the key of L are retained, with references to them.
    {"ab → x ; (a) → $1 y ;", "ab → x ;\n(a) } [^b] → $1 y ;\n"},
    {"ab → x ; ([ac]) → z $1 ;", "ab → x ;\n(c) → z $1 ;\n(a) } [^b] → z $1 ;\n"},
    {"$v = xy ; xyz → z ; ($v) → w $1 ;", "$v = xy ;\nxyz → z ;\n(xy) } [^z] → w $1 ;\n"},
    {"abc → x ; (a) (b) → &Any-Hex($2) $1 ;", "abc → x ;\n(a) (b) } [^c] → &Any-Hex($2) $1 ;\n"},
    // A later rule masked by its earlier rules is removed.
    {"a → x ; a } b → z ; a → y ;", "a → x ;\n"},
    {"a } [bc]? c → x ; a → y ;", "a } [bc]? c → x ;\na } [^bc] → y ;\na } [bc] [^c] → y ;\n"},
    // Groups are independent.
    {"ab → x ; :: Null ; a → y ;", "ab → x ;\n:: Null ;\na → y ;\n"},
    // The text boundary: [$] and $ in contexts.
    {"a } [b$] → x ; a → y ;", "a } [b$] → x ;\na } [^b$] → y ;\n"},
    {"a } $ → x ; a → y ;", "a } $ → x ;\na } [:any:] → y ;\n"},
    {"[b$] { a → x ; a → y ;", "[b$] { a → x ;\n[^b$] { a → y ;\n"},
    {"a } b [$] → x ; a → y ;", "a } b [$] → x ;\na } [^b] → y ;\na } b [:any:] → y ;\n"},
    // Strings in sets.
    {"a } [{bc}d] → x ; a → y ;", "a } [{bc}d] → x ;\na } [^bd] → y ;\na } b [^c] → y ;\n"},
    // Repetitions.
    {"a } b* c → x ; a → y ;", "a } b* c → x ;\na } b* [^bc] → y ;\n"},
    {"a } b+ c → x ; a → y ;", "a } b+ c → x ;\na } [^b] → y ;\na } b b* [^bc] → y ;\n"},
    {"ab*c → x ; a → y ;", "a b* c → x ;\na } b* [^bc] → y ;\n"},
    {"c* d { a → x ; a → y ;", "c* d { a → x ;\n[^d] { a → y ;\n"},
    // Variables that are not sets are expanded, in the key of L as well.
    {"$h = h ; $u = u ; $e = ; $h $u → x ; $h $e → y ;",
     "$h = h ;\n$u = u ;\n$e = ;\n$h $u → x ;\n$h } [^$u] → y ;\n"},
    {"$s = hu ; $s → x ; h → y ;", "$s = hu ;\n$s → x ;\nh } [^u] → y ;\n"},
    {"$c = cd ; $c { a → x ; a → y ;", "$c = cd ;\n$c { a → x ;\n[^d] { a → y ;\n[^c] d { a → y ;\n"},
    // Not handled: repetitions through several classes, possessive differences.
    {"a } [{bc}{de}]* f → x ; a → y ;", "a } [{bc}{de}]* f → x ;\na → y ;\n"},
    {"a } b* b → x ; a → y ;", "a } b* b → x ;\na } b* [^b] → y ;\n"},
    {"a } [{b}{bc}] → x ; a → y ;", "a } [{bc}b] → x ;\na } [^b] → y ;\n"},
    {"a } [{bc}b] c → x ; a → y ;", "a } [{bc}b] c → x ;\na } [^b] → y ;\na } b [^c] → y ;\na } bc [^c] → y ;\n"},
    {"[{ab}ac] d? → x ; c → y ;", "[{ab}ac] d? → x ;\n"},
    {"yw } [{m̥}bm]* [$] → ɨu ; yw → əu ; y → ə ;", "yw } [{m̥}bm]* [$] → ɨu ;\nyw } [{m̥}bm]* [^bm$] → əu ;\ny } [^w] → ə ;\n"},
    {"b a { b → y ; ([a-c] a) { b → $1 ;", "ba { b → y ;\n([ac] a) { b → $1 ;\n"},
    {"a b { c → y ; (a+ [bd]) { c → $1 ;", "ab { c → y ;\n(a+ d) { c → $1 ;\n"},
    {"b } c → y ; ([a-c] (a)) { b → $1 $2 ;", "b } c → y ;\n([a-c] (a)) { b } [^c] → $1 $2 ;\n"},
    {"u → q ; [{o̞}ou] ː? → w ;", "u → q ;\no̞ ː? → w ;\noː → w ;\no } [^ː̞] → w ;\n"},
    {"c → q ; [{ab}ac] b → z ;", "c → q ;\nabb → z ;\n"},
    {"c → q ; [{ab}{abd}ac] b → z ;", "c → q ;\nabdb → z ;\nabb → z ;\n"},
    {"c e { d → q ; [by] [{ba}ca] e { d → z ;", "ce { d → q ;\n[by] bae { d → z ;\nyae { d → z ;\n"},
    {"a } [^b]+ → x ; a → y ;", "a } [^b]+ → x ;\na } [^[^b$]$] → y ;\n"},
    {"I → y ; I } [^[:ccc=Above:]]* [:ccc=Above:] → x ;", "I → y ;\n"},
    {"a ([bc])+ → x ; a → y ;", "a ([bc])+ → x ;\na } [^bc] → y ;\n"},
    {"a b → y ; a ([bc])+ → x ;", "ab → y ;\nac [bc]* → x ;\n"},
    {"a { . → y ; d [^b]* { c → ;", "a { . → y ;\nd [^b]* [^ab$] { c → ;\n"},
    {"([^0-9]) 0 $ → $1 o ; ([a-z]) 0 ([^0-9]) → $1 o $2 ;", "([^0-9$]) 0 $ → $1 o ;\n([a-z]) 0 ([^0-9$]) → $1 o $2 ;\n"},
    {"^ x+ → ; x+ $ → ;", "^ x+ → ;\n[:any:] { x x* } $ → ;\n"},
    {"a 0 → y ; ([a-z]) 0 ($) → $1 o $2 ;", "a0 → y ;\n([b-z]) 0 () } $ → $1 o $2 ;\n"},
    {"$c = [{m̥}bmw] ; yw } $c* [$] → u ; yw → o ;", "$c = [{m̥}bmw] ;\nyw } $c* [$] → u ;\nyw } $c* [^bmw$] → o ;\n"},
    {"$c = [{m̥}bmw] ; y } $c* [$] → i ; y → e ;", "$c = [{m̥}bmw] ;\ny } $c* [$] → i ;\ny } $c* [^bmw$] → e ;\n"},
    {"$c = [{m̥}bmw] ; yw → əu ; y } $c* [$] → ɨ ;", "$c = [{m̥}bmw] ;\nyw → əu ;\ny } [{m̥}bm] $c* [$] → ɨ ;\ny } [$] → ɨ ;\n"},
    {"$c = [{m̥}bm] ; a → y ; [$] $c* { a → x ;", "$c = [{m̥}bm] ;\na → y ;\n"},
    {"a } [{bc}{bcd}] e → x ; a → y ;", "a } [{bc}{bcd}] e → x ;\na } [^b] → y ;\na } b [^c] → y ;\na } bc [^de] → y ;\na } bcd [^e] → y ;\n"},
    // Deletion closure: the characters deleted by a rule are added to the
    // repeated set preceding it in its before context (el-Upper).
    {"x [cd]* { [ab] → ;", "x [a-d]* { [ab] → ;\n"},
    {"y [cd]* { [ab] → ; [ef] → g ;", "y [a-d]* { [ab] → ;\n[ef] → g ;\n"},
    // The item before the repeated set may overlap the set, but not the deleted characters.
    {"[xc] [cd]* { [ab] → ;", "[cx] [a-d]* { [ab] → ;\n"},
    {"x [^ab]* { [ab] → ;", "x [[^ab$]ab$]* { [ab] → ;\n"},
    // Not applied: another rule produces a deleted character, an after context,
    // an item before the repeated set that includes a deleted character.
    {"x [cd]* { [ab] → ; e → a ;", "x [cd]* { [ab] → ;\ne → a ;\n"},
    {"x [cd]* { [ab] } y → ;", "x [cd]* { [ab] } y → ;\n"},
    {"[xa] [cd]* { [ab] → ;", "[ax] [cd]* { [ab] → ;\n"},
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

//
// Classification of the after contexts of conversion rules by their support
// in the regular expression engine, as lookaheads (?=after) prepared as for
// icgrep (property resolution and the RE passes, including the lookahead
// standardizations) and then compiled as the LookAheadNamer would.
//

// Replace variables by their definitions (function calls are kept).
class InlineVariables final : public re::RE_Transformer {
public:
    InlineVariables() : RE_Transformer("InlineVariables") {}
protected:
    re::RE * transformName(re::Name * n) override {
        if (isFunctionCall(n) || (n->getDefinition() == nullptr)) return n;
        return transform(n->getDefinition());
    }
};

static void collectLookaheads(re::RE * r, std::vector<re::Assertion *> & found) {
    using namespace re;
    if (Assertion * a = llvm::dyn_cast<Assertion>(r)) {
        if (a->getKind() == Assertion::Kind::LookAhead) found.push_back(a);
    } else if (Seq * s = llvm::dyn_cast<Seq>(r)) {
        for (RE * e : *s) collectLookaheads(e, found);
    } else if (Alt * alt = llvm::dyn_cast<Alt>(r)) {
        for (RE * e : *alt) collectLookaheads(e, found);
    } else if (Rep * rep = llvm::dyn_cast<Rep>(r)) {
        collectLookaheads(rep->getRE(), found);
    } else if (Capture * c = llvm::dyn_cast<Capture>(r)) {
        collectLookaheads(c->getCapturedRE(), found);
    } else if (Group * g = llvm::dyn_cast<Group>(r)) {
        collectLookaheads(g->getRE(), found);
    }
}

static bool containsKind(re::RE * r, bool (*pred)(re::RE *)) {
    using namespace re;
    if (pred(r)) return true;
    if (Seq * s = llvm::dyn_cast<Seq>(r)) {
        for (RE * e : *s) if (containsKind(e, pred)) return true;
    } else if (Alt * alt = llvm::dyn_cast<Alt>(r)) {
        for (RE * e : *alt) if (containsKind(e, pred)) return true;
    } else if (Rep * rep = llvm::dyn_cast<Rep>(r)) {
        return containsKind(rep->getRE(), pred);
    } else if (Capture * c = llvm::dyn_cast<Capture>(r)) {
        return containsKind(c->getCapturedRE(), pred);
    } else if (Assertion * a = llvm::dyn_cast<Assertion>(r)) {
        return containsKind(a->getAsserted(), pred);
    } else if (Group * g = llvm::dyn_cast<Group>(r)) {
        return containsKind(g->getRE(), pred);
    }
    return false;
}

static void flattenBody(re::RE * r, std::vector<re::RE *> & elems) {
    using namespace re;
    if (Alt * alt = llvm::dyn_cast<Alt>(r)) {
        if (alt->size() == 1) { flattenBody(alt->front(), elems); return; }
    } else if (Seq * s = llvm::dyn_cast<Seq>(r)) {
        for (RE * e : *s) flattenBody(e, elems);
        return;
    }
    elems.push_back(r);
}

// The class of a lookahead body: "+..." supported, "b..." unsupported with a
// known limitation, "c..." unsupported, other.
static std::string classifyLookaheadBody(re::RE * body) {
    using namespace re;
    const auto range = getLengthRange(body, &cc::Unicode);
    if (range.second == 0) return "+zero-width";
    if (range.first == range.second) return range.first == 1 ? "+one character" : "+fixed length";
    if (hasUniquePrefix(body)) return "+unique prefix";
    std::vector<LookaheadSegment> segments;
    if (parseLookaheadChain(body, &cc::Unicode, segments)) return "+lookahead chain";
    if (containsKind(body, [](RE * e) { return llvm::isa<Reference>(e); })) return "creference";
    if (containsKind(body, [](RE * e) { return llvm::isa<Assertion>(e); })) return "cnested lookaround";
    if (containsKind(body, [](RE * e) { return llvm::isa<Start>(e); })) return "ctext start";
    if (containsKind(body, [](RE * e) { return llvm::isa<Capture>(e); })) return "bcapture";
    std::vector<RE *> elems;
    flattenBody(body, elems);
    bool nonClassStar = false, variableItem = false;
    for (RE * e : elems) {
        const auto r = getLengthRange(e, &cc::Unicode);
        if (const Rep * rep = llvm::dyn_cast<Rep>(e)) {
            if (rep->getUB() == Rep::UNBOUNDED_REP) {
                if (resolveCharClass(rep->getRE()) == nullptr) nonClassStar = true;
                continue;
            }
        }
        if (r.first != r.second) variableItem = true;
    }
    if (nonClassStar) return "bstar of a non-class";
    if (llvm::isa<End>(elems.back())) return "crun of a class to the end of the text";
    if (variableItem) return "bvariable-length item (bounded repetition or alternation)";
    if (llvm::isa<Rep>(elems.back())) return "cending with a star";
    return "boverlapping star class";
}

struct AfterContextCounts {
    std::map<std::string, unsigned> rules;                  // by class
    std::map<std::string, std::map<std::string, std::string>> examples;   // class -> after context -> file
};

static void classifyAfterContexts(const std::vector<std::string> & tRules, const std::string & label, AfterContextCounts & counts) {
    for (const Extract e : {Extract::Forward, Extract::Backward}) {
        std::vector<Rule *> rules = DisambiguateOrder(extract(parseTransformRules(tRules), e));
        for (Rule * r : rules) {
            ConversionRule * const cr = llvm::dyn_cast<ConversionRule>(r);
            if ((cr == nullptr) || !cr->getSourceSide(Direction::Forward)->hasAfterContext()) continue;
            re::RE * const after = cr->getSourceSide(Direction::Forward)->getAfterContext();
            re::RE * inlined = InlineVariables().transformRE(engineContext(after, true));
            re::RE * la = re::makeAssertion(inlined, re::Assertion::Kind::LookAhead, re::Assertion::Sense::Positive);
            la = re::resolveModesAndExternalSymbols(la);
            la = re::regular_expression_passes(la);
            std::vector<re::Assertion *> lookaheads;
            collectLookaheads(la, lookaheads);
            // The class of the context is the least supported class of its lookaheads.
            auto rank = [](const std::string & c) -> int {
                static const std::vector<std::string> supported{"+always or never holds", "+zero-width",
                    "+one character", "+fixed length", "+unique prefix", "+lookahead chain"};
                for (unsigned i = 0; i < supported.size(); ++i) if (c == supported[i]) return i;
                return (c[0] == 'b') ? 10 : 20;
            };
            std::string cls = "+always or never holds";
            std::string body;
            for (re::Assertion * a : lookaheads) {
                const std::string c = classifyLookaheadBody(a->getAsserted());
                if (rank(c) > rank(cls)) {
                    cls = c;
                    body = Printer_RE::PrintRE(a->getAsserted());
                }
            }
            if ((cls[0] != '+') && getenv("SHOW_BODIES")) {
                std::cerr << cls << "\t" << label << "\t" << printPattern(after) << "\n    " << body << "\n";
            }
            const std::string dir = (e == Extract::Forward) ? "forward" : "backward";
            counts.rules[dir + " " + cls]++;
            counts.examples[cls].emplace(printPattern(after), label);
        }
    }
}

//
// Classification of conversion rules (forward and backward, as extracted
// forward rules) by the length of the text to replace, the number of
// captures, the order of the references to the captures in the result, and
// the revisiting status of the result (its cursor).
//

// Variables inlined, with the text boundary marker as the empty string (it
// matches no character).
class InlineVariablesAndBoundary final : public re::RE_Transformer {
public:
    InlineVariablesAndBoundary() : RE_Transformer("InlineVariablesAndBoundary") {}
protected:
    re::RE * transformName(re::Name * n) override {
        if (isTextBoundary(n)) return re::makeSeq();
        if (isFunctionCall(n) || (n->getDefinition() == nullptr)) return n;
        return transform(n->getDefinition());
    }
    // Empty sets match nothing: [$] is Alt{CC{}, End}, of length 0.
    re::RE * transformAlt(re::Alt * alt) override {
        std::vector<re::RE *> elems;
        for (re::RE * e : *alt) {
            re::RE * const e1 = transform(e);
            const re::CC * const cc = llvm::dyn_cast<re::CC>(e1);
            if ((cc == nullptr) || !cc->empty()) elems.push_back(e1);
        }
        return re::makeAlt(elems.begin(), elems.end());
    }
};

static std::string classifyTextLength(re::RE * text) {
    const auto range = re::getLengthRange(InlineVariablesAndBoundary().transformRE(text), &cc::Unicode);
    if (range.first == range.second) {
        if (range.first <= 2) return std::to_string(range.first) + (range.first == 0 ? " (insertion)" : "");
        return "fixed >2";
    }
    return range.second == INT_MAX ? "variable (unbounded)" : "variable (bounded)";
}

static void collectCaptures(re::RE * r, std::vector<re::Capture *> & found) {
    using namespace re;
    if (r == nullptr) return;
    if (Capture * c = llvm::dyn_cast<Capture>(r)) {
        found.push_back(c);
        collectCaptures(c->getCapturedRE(), found);
    } else if (Seq * s = llvm::dyn_cast<Seq>(r)) {
        for (RE * e : *s) collectCaptures(e, found);
    } else if (Alt * alt = llvm::dyn_cast<Alt>(r)) {
        for (RE * e : *alt) collectCaptures(e, found);
    } else if (Rep * rep = llvm::dyn_cast<Rep>(r)) {
        collectCaptures(rep->getRE(), found);
    } else if (Group * g = llvm::dyn_cast<Group>(r)) {
        collectCaptures(g->getRE(), found);
    } else if (Assertion * a = llvm::dyn_cast<Assertion>(r)) {
        collectCaptures(a->getAsserted(), found);
    }
}

// The references of a result in order, noting those within function calls.
static void collectReferences(re::RE * r, std::vector<re::Reference *> & found, bool & inFunction, bool withinCall = false) {
    using namespace re;
    if (Reference * ref = llvm::dyn_cast<Reference>(r)) {
        found.push_back(ref);
        inFunction |= withinCall;
    } else if (Seq * s = llvm::dyn_cast<Seq>(r)) {
        for (RE * e : *s) collectReferences(e, found, inFunction, withinCall);
    } else if (Alt * alt = llvm::dyn_cast<Alt>(r)) {
        for (RE * e : *alt) collectReferences(e, found, inFunction, withinCall);
    } else if (Name * n = llvm::dyn_cast<Name>(r)) {
        if (isFunctionCall(n) && n->getDefinition()) collectReferences(n->getDefinition(), found, inFunction, true);
    }
}

static std::string classifyCaptureCount(size_t n) {
    return n >= 3 ? "3+" : std::to_string(n);
}

static std::string classifyCaptureOrder(const std::vector<re::Capture *> & captures, re::RE * result) {
    if (captures.empty()) return "no captures";
    std::vector<re::Reference *> refs;
    bool inFunction = false;
    collectReferences(result, refs, inFunction);
    if (refs.empty()) return "unreferenced";
    std::vector<unsigned> order;
    for (re::Reference * r : refs) order.push_back(std::stoul(r->getName()));
    bool increasing = true;
    for (size_t i = 1; i < order.size(); i++) increasing &= order[i - 1] < order[i];
    std::vector<unsigned> distinct(order);
    std::sort(distinct.begin(), distinct.end());
    const bool repeated = std::adjacent_find(distinct.begin(), distinct.end()) != distinct.end();
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    std::string c = repeated ? "repeated" : (increasing ? "in order" : "reordered");
    if (distinct.size() < captures.size()) c += ", partial";
    if (inFunction) c += ", in function call";
    return c;
}

static std::string classifyRevisit(const RuleSide * result) {
    const bool revisit = !llvm::cast<re::Seq>(result->getResultToRevisit())->empty();
    const bool completed = !llvm::cast<re::Seq>(result->getCompletedResult())->empty();
    const int offset = result->getCursorOffset();
    if (!result->hasCursor() || (offset == 0 && !revisit)) return "none";
    if (offset < 0) return "back up before result (|@)";
    if (offset > 0) return "skip past result (@|)";
    return completed ? "revisit part of result" : "revisit entire result";
}

// Fixed text: characters, or variables defined as fixed text.
static bool isFixedText(re::RE * r) {
    using namespace re;
    if (CC * cc = llvm::dyn_cast<CC>(r)) return cc->count() == 1;
    if (Seq * seq = llvm::dyn_cast<Seq>(r)) {
        for (RE * e : *seq) if (!isFixedText(e)) return false;
        return true;
    }
    if (Alt * alt = llvm::dyn_cast<Alt>(r)) return alt->size() == 1 && isFixedText(alt->front());
    if (Name * n = llvm::dyn_cast<Name>(r)) {
        return !isFunctionCall(n) && n->getDefinition() && isFixedText(n->getDefinition());
    }
    return false;
}

static void flattenItems(re::RE * r, std::vector<re::RE *> & items) {
    if (re::Seq * seq = llvm::dyn_cast<re::Seq>(r)) {
        for (re::RE * e : *seq) flattenItems(e, items);
    } else {
        items.push_back(r);
    }
}

// A simple insertion rule: the text to replace is a capture, and the result
// (without text to revisit) is fixed text, the reference to the capture, and
// fixed text, e.g. ($v) → $1 x ;  inserts x after the text.  Returns the kind
// of insertion (before, after, before and after, or none: a copy), or an
// empty string if the rule is not a simple insertion rule.
static std::string classifyInsertion(const ConversionRule * cr, re::RE ** captured = nullptr) {
    using namespace re;
    const RuleSide * const source = cr->getSourceSide(Direction::Forward);
    const RuleSide * const result = cr->getResultSide(Direction::Forward);
    std::vector<RE *> text;
    flattenItems(source->getText(), text);
    Capture * const capture = (text.size() == 1) ? llvm::dyn_cast<Capture>(text[0]) : nullptr;
    if (capture == nullptr || classifyRevisit(result) != "none") return "";
    std::vector<RE *> items;
    flattenItems(result->getText(), items);
    size_t refIndex = items.size();
    for (size_t i = 0; i < items.size(); i++) {
        if (Reference * ref = llvm::dyn_cast<Reference>(items[i])) {
            if (refIndex != items.size() || ref->getName() != capture->getName()) return "";
            refIndex = i;
        } else if (!isFixedText(items[i])) {
            return "";
        }
    }
    if (refIndex == items.size()) return "";
    if (captured) *captured = capture->getCapturedRE();
    const bool before = refIndex > 0;
    const bool after = refIndex + 1 < items.size();
    if (before && after) return "insert before and after";
    if (before) return "insert before";
    if (after) return "insert after";
    return "copy (no insertion)";
}

struct RuleClassCounts {
    // dimension -> direction -> class -> count
    std::map<std::string, std::map<std::string, std::map<std::string, unsigned>>> byDimension;
    // signature -> (count, example rule, file)
    std::map<std::string, std::tuple<unsigned, std::string, std::string>> signatures;
    unsigned rules = 0;
};

// The classes of a forward conversion rule: text to replace, captures,
// capture order and revisiting.
struct RuleClass {
    std::string length;
    std::string captures;
    std::string order;
    std::string revisit;
    std::string signature() const {return length + " | " + captures + " | " + order + " | " + revisit;}
};

static RuleClass classifyRule(const ConversionRule * cr) {
    const RuleSide * const source = cr->getSourceSide(Direction::Forward);
    const RuleSide * const result = cr->getResultSide(Direction::Forward);
    std::vector<re::Capture *> captures;
    collectCaptures(source->getBeforeContext(), captures);
    collectCaptures(source->getAfterContext(), captures);
    const size_t contextCaptures = captures.size();
    collectCaptures(source->getText(), captures);
    RuleClass c;
    c.length = classifyTextLength(source->getText());
    c.captures = classifyCaptureCount(captures.size());
    if (contextCaptures) c.captures += " (" + std::to_string(contextCaptures) + " in contexts)";
    c.order = classifyCaptureOrder(captures, result->getText());
    c.revisit = classifyRevisit(result);
    return c;
}

static void classifyRules(const std::vector<std::string> & tRules, const std::string & label, bool quiet, RuleClassCounts & counts) {
    const std::vector<Rule *> parsed = parseTransformRules(tRules);
    for (const Extract e : {Extract::Forward, Extract::Backward}) {
        const std::string dir = (e == Extract::Forward) ? "forward" : "backward";
        for (const Rule * r : extract(parsed, e)) {
            const ConversionRule * const cr = llvm::dyn_cast<ConversionRule>(r);
            if (cr == nullptr) continue;
            const RuleClass c = classifyRule(cr);
            counts.rules++;
            counts.byDimension["text to replace"][dir][c.length]++;
            counts.byDimension["captures"][dir][c.captures]++;
            counts.byDimension["capture order"][dir][c.order]++;
            counts.byDimension["revisiting"][dir][c.revisit]++;
            re::RE * captured = nullptr;
            const std::string insertion = classifyInsertion(cr, &captured);
            counts.byDimension["simple insertion"][dir][insertion.empty() ? "not a simple insertion rule" : insertion]++;
            if (!insertion.empty()) {
                counts.byDimension["simple insertion: captured text"][dir][classifyTextLength(captured)]++;
            }
            const std::string sig = c.signature();
            auto & s = counts.signatures[sig];
            if (std::get<0>(s)++ == 0) {
                std::get<1>(s) = printRule(r);
                std::get<2>(s) = label;
            }
            if (!quiet) {
                std::cout << label << " " << dir << "\t" << sig << (insertion.empty() ? "" : " | simple insertion: " + insertion)
                          << "\t" << printRule(r) << "\n";
            }
        }
    }
}

static const EliminationTestCase ruleClassTestCases[] = {
    {"a → x ;", "1 | 0 | no captures | none"},
    {"ab → x ;", "2 | 0 | no captures | none"},
    {"$v = abc ; $v → x ;", "fixed >2 | 0 | no captures | none"},
    {"[{ch}c] → x ;", "variable (bounded) | 0 | no captures | none"},
    {"a+ → x ;", "variable (unbounded) | 0 | no captures | none"},
    {"a { } b → x ;", "0 (insertion) | 0 | no captures | none"},
    {"a } [$] → x ;", "1 | 0 | no captures | none"},
    {"a [$] → x ;", "1 | 0 | no captures | none"},
    {"(a) (b) → $1 $2 ;", "2 | 2 | in order | none"},
    {"(a) (b) → $2 $1 ;", "2 | 2 | reordered | none"},
    {"(a) (b) → $1 $1 ;", "2 | 2 | repeated, partial | none"},
    {"(a) (b) (c) → $3 ;", "fixed >2 | 3+ | in order, partial | none"},
    {"(a) → &Any-Hex($1) ;", "1 | 1 | in order, in function call | none"},
    {"(a) { b → x ;", "1 | 1 (1 in contexts) | unreferenced | none"},
    {"a → x | y ;", "1 | 0 | no captures | revisit part of result"},
    {"a → | y ;", "1 | 0 | no captures | revisit entire result"},
    {"a → y | ;", "1 | 0 | no captures | none"},
    {"a → |@ y ;", "1 | 0 | no captures | back up before result (|@)"},
    {"a → y @| ;", "1 | 0 | no captures | skip past result (@|)"},
};

static const EliminationTestCase insertionClassTestCases[] = {
    {"(a) → $1 x ;", "insert after"},
    {"(a) → x $1 ;", "insert before"},
    {"(a) → x $1 yz ;", "insert before and after"},
    {"$v = [aeiou] ; ($v) → $1 ;", "copy (no insertion)"},
    {"$s = xy ; ([ab]+) → $s $1 ;", "insert before"},
    {"b { (a) } c → $1 x ;", "insert after"},
    {"(a) b → $1 x ;", ""},
    {"(a) → $1 $1 ;", ""},
    {"(a) (b) → $1 x $2 ;", ""},
    {"(a) → &Any-Hex($1) ;", ""},
    {"(a) → x | $1 ;", ""},
    {"a → x ;", ""},
};

static void reportRuleClasses(const RuleClassCounts & counts) {
    std::cout << "Total: " << counts.rules << " conversion rules (forward and backward)\n";
    for (const char * dim : {"text to replace", "captures", "capture order", "revisiting",
                             "simple insertion", "simple insertion: captured text"}) {
        const auto it = counts.byDimension.find(dim);
        if (it == counts.byDimension.end()) continue;
        std::map<std::string, std::pair<unsigned, unsigned>> rows;
        for (const auto & d : it->second) {
            for (const auto & c : d.second) {
                (d.first == "forward" ? rows[c.first].first : rows[c.first].second) += c.second;
            }
        }
        std::cout << "\n== " << dim << "\tforward\tbackward\ttotal\n";
        for (const auto & row : rows) {
            std::cout << "  " << row.first << "\t" << row.second.first << "\t" << row.second.second
                      << "\t" << (row.second.first + row.second.second) << "\n";
        }
    }
    std::vector<std::pair<std::string, std::tuple<unsigned, std::string, std::string>>> sigs(counts.signatures.begin(), counts.signatures.end());
    std::stable_sort(sigs.begin(), sigs.end(), [](const auto & a, const auto & b) { return std::get<0>(a.second) > std::get<0>(b.second); });
    std::cout << "\n== combined classes (text to replace | captures | capture order | revisiting): count, example\n";
    for (const auto & s : sigs) {
        std::cout << "  " << std::get<0>(s.second) << "\t" << s.first << "\n      e.g. " << std::get<1>(s.second)
                  << "\t" << std::get<2>(s.second) << "\n";
    }
}

// The contexts of the first rule, for the regular expression engine
// (engineContext), as printed by Printer_RE ("" for no context).
struct EngineContextTestCase {
    const char * input;
    const char * before;
    const char * after;
};

static const EngineContextTestCase engineContextTestCases[] = {
    // A negated set as the outermost item: a negative assertion.
    {"[^ab] { x } [^cd] → y ;",
     "NegativeLookBehindAssertion(CC \"Unicode_61_62\" )", "NegativeLookAheadAssertion(CC \"Unicode_63_64\" )"},
    // A negated set followed (or preceded) by a character: no boundary (and
    // the outermost item of the before context is a negative assertion).
    {"[^ab] c { x } c [^de]* e → y ;",
     "(Seq[NegativeLookBehindAssertion(CC \"Unicode_61_62\" ),CC \"Unicode_63\" ])",
     "(Seq[CC \"Unicode_63\" ,Rep(Diff (Any(Unicode) , CC \"Unicode_64_65\" ),0,Unbounded),CC \"Unicode_65\" ])"},
    // A negated set followed only by an optional item: the boundary is kept.
    {"x } [^cd] e? → y ;", "",
     "(Seq[(Alt[Diff (Any(Unicode) , CC \"Unicode_63_64\" ),End]),Rep(CC \"Unicode_65\" ,0,1)])"},
    // The boundary set [$] (End or Start) is kept.
    {"[$] { x } [$] → y ;", "(Alt[Start,CC \"Unicode\" ])", "(Alt[End,CC \"Unicode\" ])"},
};

static std::string printEngineContext(re::RE * context, bool after) {
    return context ? Printer_RE::PrintRE(engineContext(context, after)) : "";
}

// All the errors of a rule text are reported, one per line, after recovery
// at the end of each rule.
struct MultipleErrorTestCase {
    const char * input;
    const char * expected;
};

static const MultipleErrorTestCase multipleErrorTestCases[] = {
    {"a b ; c → d ; e f ;",
     "Expected a conversion operator (→, ←, ↔) but found ';' (line 1, column 5)\n"
     "Expected a conversion operator (→, ←, ↔) but found ';' (line 1, column 19)"},
    // Recovery skips quoted, escaped and bracketed ';' and comments.
    {"a ';' b ; [;] c → d ; x\\; y ; # ; \n e $ f → g ;",
     "Expected a conversion operator (→, ←, ↔) but found ';' (line 1, column 9)\n"
     "Expected a conversion operator (→, ←, ↔) but found ';' (line 1, column 29)\n"
     "The '$' anchor must be at the end of the pattern (line 2, column 6)"},
    // An unterminated set: recovery at the next ';' after the error (here,
    // the variable $v used within the set while being defined).
    {"$v = [a-z ; $v → x ; y → $w ;",
     "Undefined variable $v (line 1, column 15)\n"
     "Undefined variable $w (line 1, column 28)"},
    // Uses of a variable whose definition has an error.
    {"$v = a ] ; $v → x ; y → $w ;",
     "Unquoted syntax character ']' (line 1, column 8)\n"
     "Variable $v is undefined, as its definition has an error (line 1, column 14)\n"
     "Undefined variable $w (line 1, column 27)"},
    // Validation errors are all reported, by check.
    {"a → b ; :: [a] ; x* → y ; (a)+ → $1 ; z* ↔ w* ;",
     "Filter rule :: [a] ; is not the first rule\n"
     "Rule x* → y ; matches the empty text without contexts, indefinitely\n"
     "Rule z* ↔ w* ; matches the empty text without contexts, indefinitely\n"
     "Rule (a)+ → $1 ; references a segment within a repetition, which captures only its last repetition"},
    // Syntax errors, then validation errors of the rules without syntax errors.
    {"a b ; x* → y ;",
     "Expected a conversion operator (→, ←, ↔) but found ';' (line 1, column 5)\n"
     "Rule x* → y ; matches the empty text without contexts, indefinitely"},
};

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
    for (const EliminationTestCase & t : exclusiveTestCases) {
        count++;
        try {
            MutuallyExclusiveStats stats;
            const std::vector<Rule *> rules = MutuallyExclusivePartitioning(parseTransformRules({t.input}), &stats);
            failures += !checkOutput("exclusive", t.input, printRules(rules), t.expected, false);
            if (stats.mismatches) {
                failures++;
                std::cerr << "FAIL (exclusive mismatches): " << t.input << "\n";
            }
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    for (const EliminationTestCase & t : disambiguationTestCases) {
        count++;
        try {
            DisambiguationStats stats;
            const std::vector<Rule *> rules = DisambiguateOrder(parseTransformRules({t.input}), &stats);
            failures += !checkOutput("disambiguate", t.input, printRules(rules), t.expected);
            if (stats.verificationFailures) {
                failures++;
                std::cerr << "FAIL (disambiguate verification): " << t.input << "\n";
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
    for (const EliminationTestCase & t : insertionClassTestCases) {
        count++;
        try {
            const std::vector<Rule *> rules = parseTransformRules({t.input});
            const std::string actual = classifyInsertion(llvm::cast<ConversionRule>(rules.back()));
            if (actual != t.expected) {
                failures++;
                std::cerr << "FAIL (insertion class): " << t.input << "\n  expected: " << t.expected << "\n  actual:   " << actual << "\n";
            }
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    for (const EliminationTestCase & t : ruleClassTestCases) {
        count++;
        try {
            const std::vector<Rule *> rules = parseTransformRules({t.input});
            const std::string actual = classifyRule(llvm::cast<ConversionRule>(rules.back())).signature();
            if (actual != t.expected) {
                failures++;
                std::cerr << "FAIL (rule class): " << t.input << "\n  expected: " << t.expected << "\n  actual:   " << actual << "\n";
            }
        } catch (const TransformRuleParseError & e) {
            failures++;
            std::cerr << "FAIL: " << t.input << "\n  " << e.what() << "\n";
        }
    }
    for (const MultipleErrorTestCase & t : multipleErrorTestCases) {
        count++;
        std::string actual = "(no error)";
        try {
            parseTransformRules({t.input});
        } catch (const TransformRuleParseError & e) {
            actual = e.what();
        }
        if (actual != t.expected) {
            failures++;
            std::cerr << "FAIL (multiple errors): " << t.input << "\n  expected: " << t.expected << "\n  actual:   " << actual << "\n";
        }
    }
    for (const EngineContextTestCase & t : engineContextTestCases) {
        count++;
        try {
            const std::vector<Rule *> rules = parseTransformRules({t.input});
            const RuleSide * const side = llvm::cast<ConversionRule>(rules[0])->getSourceSide(Direction::Forward);
            const std::string before = printEngineContext(side->getBeforeContext(), false);
            const std::string after = printEngineContext(side->getAfterContext(), true);
            if ((before != t.before) || (after != t.after)) {
                failures++;
                std::cerr << "FAIL (engine context): " << t.input << "\n  expected: " << t.before << " | " << t.after
                          << "\n  actual:   " << before << " | " << after << "\n";
            }
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
    bool exclusive = false;
    bool disambiguate = false;
    bool countTrivial = false;
    bool overlaps = false;
    bool partition = false;
    bool count = false;
    bool classifyAfter = false;
    bool classifyRuleKinds = false;
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
        else if (arg == "--mutually-exclusive") exclusive = true;
        else if (arg == "--disambiguate-order") disambiguate = true;
        else if (arg == "--count-trivial-captures") countTrivial = true;
        else if (arg == "--overlaps") overlaps = true;
        else if (arg == "--partition") partition = true;
        else if (arg == "--count-nullable-captures") count = true;
        else if (arg == "--classify-after-contexts") classifyAfter = true;
        else if (arg == "--classify-rules") classifyRuleKinds = true;
        else files.push_back(arg);
    }
    if (files.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--xml] [--quiet] [--forward | --backward]\n"
                  << "           [--eliminate-trivial-captures] [--eliminate-nullable-captures] [--mutually-exclusive]\n"
                  << "           [--disambiguate-order] file ...\n"
                  << "       " << argv[0] << " [--xml] [--quiet] --overlaps file ...\n"
                  << "       " << argv[0] << " [--xml] [--quiet] --partition file ...\n"
                  << "       " << argv[0] << " [--xml] --count-trivial-captures file ...\n"
                  << "       " << argv[0] << " [--xml] --count-nullable-captures file ...\n"
                  << "       " << argv[0] << " [--xml] --classify-after-contexts file ...\n"
                  << "       " << argv[0] << " [--xml] [--quiet] --classify-rules file ...\n"
                  << "       " << argv[0] << " --self-test\n";
        return 2;
    }
    bool ok = true;
    NullableCounts total;
    TrivialCaptureStats trivialTotal;
    OverlapCounts overlapTotal;
    PartitionCounts partitionTotal;
    AfterContextCounts afterCounts;
    RuleClassCounts ruleClassCounts;
    for (const std::string & f : files) {
        try {
            const std::string text = readFile(f);
            const std::vector<std::string> tRules = xml ? extractTRules(text) : std::vector<std::string>{text};
            if (classifyRuleKinds) {
                classifyRules(tRules, f, quiet, ruleClassCounts);
            } else if (classifyAfter) {
                classifyAfterContexts(tRules, f, afterCounts);
            } else if (partition) {
                ok &= reportPartition(tRules, f, quiet, partitionTotal);
            } else if (overlaps) {
                reportOverlaps(tRules, f, quiet, overlapTotal);
            } else if (countTrivial) {
                countTrivialCaptures(tRules, f, trivialTotal);
            } else if (count) {
                countNullableCaptures(tRules, f, total);
            } else {
                ok &= processRules(tRules, f, quiet, e, eliminateTrivial, eliminate, exclusive, disambiguate);
            }
        } catch (const TransformRuleParseErrors & e) {
            reportErrors(f, e);
            ok = false;
        } catch (const std::exception & e) {
            std::cerr << f << ": " << e.what() << "\n";
            ok = false;
        }
    }
    if (classifyAfter) {
        // Classes: + supported, b unsupported (known limitation), c unsupported (other).
        for (const auto & c : afterCounts.rules) {
            std::cout << c.second << "\t" << c.first << "\n";
        }
        for (const auto & c : afterCounts.examples) {
            std::cout << "\n== " << c.first << " (" << c.second.size() << " distinct after contexts)\n";
            for (const auto & x : c.second) {
                std::cout << "  " << x.first << "\t" << x.second << "\n";
            }
        }
    }
    if (classifyRuleKinds) {
        reportRuleClasses(ruleClassCounts);
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

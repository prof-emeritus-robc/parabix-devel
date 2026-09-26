/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

//  ldml_trules: parse LDML transform rules and print them in canonical form.
//
//  ldml_trules [--xml] [--quiet] [--forward | --backward]
//              [--eliminate-nullable-captures] file ...
//      Parse the rules of each file (plain rule text, or with --xml, the
//      <tRule> elements of an LDML transform file), print the canonical
//      form of the rules and check that the canonical form reparses to
//      the same canonical form.  With --forward or --backward, the rules
//      extracted for that direction (as forward rules) are printed.
//      With --eliminate-nullable-captures, nullable capture elimination
//      is applied to the (forward) rules before printing.
//  ldml_trules [--xml] --count-nullable-captures file ...
//      Report the nullable captures in the source sides of conversion rules.
//  ldml_trules --self-test
//      Run the built-in test cases.

#include <ldml/transform_rules.h>
#include <ldml/transform_rules_parser.h>
#include <ldml/transform_rules_printer.h>
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

enum class Extract {All, Forward, Backward};

static std::vector<Rule *> extract(const std::vector<Rule *> & rules, const Extract e) {
    switch (e) {
        case Extract::Forward: return ExtractForwardRules(rules);
        case Extract::Backward: return ExtractReverseBackwardRules(rules);
        default: return rules;
    }
}

// Parse, print, and check that the printed form reparses to the same form.
static bool processRules(const std::vector<std::string> & tRules, const std::string & label, bool quiet, const Extract e, bool eliminate) {
    try {
        std::vector<Rule *> rules = extract(parseTransformRules(tRules), e);
        if (eliminate) rules = NullableCaptureElimination(rules);
        const std::string printed = printRules(rules);
        if (!quiet) std::cout << printed;
        const std::string reprinted = printRules(parseTransformRules({printed}));
        if (reprinted != printed) {
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

struct EliminationTestCase {
    const char * input;
    const char * expected;
};

static const EliminationTestCase eliminationTestCases[] = {
    {"(a*) b → $1 x ;", "(a+) b → $1 x ;\nb → x ;\n"},
    // A capture of a single character is replaced by the character.
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
    // Unchanged: captures within repetitions, non-nullable captures, non-forward rules.
    {"((a*) b)* → x ;", "((a*) b)* → x ;\n"},
    {"(a+) b → $1 ;", "(a+) b → $1 ;\n"},
    {"(a*) ↔ b ;", "(a*) ↔ b ;\n"},
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

static bool checkOutput(const char * label, const char * input, const std::string & actual, const char * expected) {
    bool ok = actual == expected;
    std::string reprinted;
    if (ok) {
        // The extracted rules must reparse to themselves.
        try {
            reprinted = printRules(parseTransformRules({actual}));
        } catch (const TransformRuleParseError & e) {
            reprinted = std::string("reparse failed: ") + e.what() + "\n";
        }
        ok = reprinted == actual;
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
        else if (arg == "--count-nullable-captures") count = true;
        else files.push_back(arg);
    }
    if (files.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--xml] [--quiet] [--forward | --backward] [--eliminate-nullable-captures] file ...\n"
                  << "       " << argv[0] << " [--xml] --count-nullable-captures file ...\n"
                  << "       " << argv[0] << " --self-test\n";
        return 2;
    }
    bool ok = true;
    NullableCounts total;
    for (const std::string & f : files) {
        try {
            const std::string text = readFile(f);
            const std::vector<std::string> tRules = xml ? extractTRules(text) : std::vector<std::string>{text};
            if (count) {
                countNullableCaptures(tRules, f, total);
            } else {
                ok &= processRules(tRules, f, quiet, e, eliminate);
            }
        } catch (const std::exception & e) {
            std::cerr << f << ": " << e.what() << "\n";
            ok = false;
        }
    }
    if (count) {
        std::cout << "Total: " << total.captures << " nullable captures in " << total.rules << " rules of "
                  << total.files << " files (+" << total.forwardAdded << " forward rules, +"
                  << total.backwardAdded << " backward rules); " << total.repeated << " within repetitions\n";
    }
    return ok ? 0 : 1;
}

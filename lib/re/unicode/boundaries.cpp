#include <re/unicode/boundaries.h>

#include <re/adt/adt.h>
#include <re/adt/re_name.h>
#include <re/parse/parser.h>
#include <re/printer/re_printer.h>
#include <re/analysis/validation.h>
#include <re/transforms/re_transformer.h>
#include <re/unicode/re_name_resolve.h>
#include <re/unicode/resolve_properties.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>

#include <vector>                  // for vector, allocator
#include <llvm/Support/Casting.h>  // for dyn_cast, isa
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>


/*
 Unicode Technical Standard #18 defines grapheme cluster mode, signified by the (?g) switch.
 The mode is defined in terms of the assertion of grapheme cluster boundary assertions \b{g}
 after every atomic literal.
 
 resolveGraphemeMode transforms a regular expression to perform the required insertion of
 grapheme cluster boundaries, and the elimination of grapheme cluster mode groups.

*/

using namespace llvm;

namespace re {

struct GraphemeBoundaryAbsentValidator final : public RE_Validator {
    
    GraphemeBoundaryAbsentValidator()
    : RE_Validator() {}
    
    bool validatePropertyExpression(const PropertyExpression * e) override {
        return e->getPropertyCode() != UCD::g;
    }

    bool validateName(const Name * n) override {
        return n->getFullName() != "\\b{g}";
    }
};

bool hasGraphemeClusterBoundary(const RE * re) {
    GraphemeBoundaryAbsentValidator v;
    return !(v.validateRE(re));
}

    
class GraphemeModeTransformer : public RE_Transformer {
public:
    GraphemeModeTransformer(bool inGraphemeMode = true) : RE_Transformer("ResolveGraphemeMode"),
    mGraphemeMode(inGraphemeMode),
    mGCB(makeBoundaryExpression("g"))
    {}
    
    RE * transformName(Name * n) override {
        if (mGraphemeMode && (n->getName() == ".")) {
            RE * nonGCB = makeDiff(makeSeq({}), mGCB);
            return makeSeq({makeAny(), makeRep(makeSeq({nonGCB, makeAny()}), 0, Rep::UNBOUNDED_REP), mGCB});
        }
        return n;
    }
    
    RE * transformCC(CC * cc) override {
        if (mGraphemeMode) return makeSeq({cc, mGCB});
        return cc;
    }
    
    RE * transformRange(Range * rg) override {
        if (mGraphemeMode) return makeSeq({rg, mGCB});
        return rg;
    }
    
    RE * transformGroup(Group * g) override {
        if (g->getMode() == Group::Mode::GraphemeMode) {
            RE * r = g->getRE();
            bool modeSave = mGraphemeMode;
            mGraphemeMode = g->getSense() == Group::Sense::On;
            RE * t = transform(r);
            mGraphemeMode = modeSave;
            return t;
        } else {
            return RE_Transformer::transformGroup(g);
        }
    }
    
    RE * transformSeq(Seq * seq) override {
        std::vector<RE*> list;
        bool afterSingleChar = false;
        bool changed = false;
        for (auto i = seq->begin(); i != seq->end(); ++i) {
            bool atSingleChar = isa<CC>(*i) && (cast<CC>(*i)->count() == 1);
            if (afterSingleChar && mGraphemeMode && !atSingleChar) {
                list.push_back(mGCB);
                changed = true;
            }
            if (isa<CC>(*i)) {
                list.push_back(*i);
            } else {
                RE * t = transform(*i);
                if (*i != t) changed = true;
                list.push_back(t);
            }
            afterSingleChar = atSingleChar;
        }
        if (afterSingleChar && mGraphemeMode) {
            list.push_back(mGCB);
            changed = true;
        }
        if (!changed) return seq;
        return makeSeq(list.begin(), list.end());
    }

private:
    bool mGraphemeMode;
    RE * mGCB;
};

RE * resolveGraphemeMode(RE * re, bool inGraphemeMode) {
    return GraphemeModeTransformer(inGraphemeMode).transformRE(re);
}

#define Behind(x) makeLookBehindAssertion(x)
#define notBehind(x) makeNegativeLookBehindAssertion(x)
#define Ahead(x) makeLookAheadAssertion(x)
#define notAhead(x) makeNegativeLookAheadAssertion(x)

RE * generateGraphemeClusterBoundaryRule(bool extendedGraphemeClusters) {
    // 3.1.1 Grapheme Cluster Boundary Rules
    // Grapheme cluster boundary rules define a number of contexts where
    // breaks are not permitted.  In the following definitions, we identify
    // the points at which breaks are not permitted are identified by the
    // definitions marked GCX.
    
    // Rules GB1, GB2, GB4 and GB5 define rules where breaks occur overriding
    // later rules (specifically GB9, GB9a, GB9b).
    // Rules GB9 and GB9a are overridden by GB1 and GB4, to allow breaks
    // at start of text or after any control|CR|LF.  This is equivalent
    // to stating that the lookbehind context for GB9 and GB9b is any
    // non-control character (any actual character not in control|CR|LF).
    // Similarly, the overriding of GB9b simplifies to a lookahead assertion
    // on a noncontrol.
    //
    RE * GCB_CR = makePropertyExpression("gcb", "cr");
    RE * GCB_LF = makePropertyExpression("gcb", "lf");
    RE * GCB_Control = makePropertyExpression("gcb", "control");
    RE * GCB_Control_CR_LF = makeAlt({GCB_Control, GCB_CR, GCB_LF});

    // Break at the start and end of text.
    RE * GCB_1 = makeSOT();
    RE * GCB_2 = makeEOT();
    // Do not break between a CR and LF.
    RE * GCB_3 = makeSeq({Behind(GCB_CR), Ahead(GCB_LF)});
    // Otherwise, break before and after controls.
    RE * GCB_4 = Behind(GCB_Control_CR_LF);
    RE * GCB_5 = Ahead(GCB_Control_CR_LF);
    RE * GCB_1_5 = makeAlt({GCB_1, GCB_2, makeDiff(makeAlt({GCB_4, GCB_5}), GCB_3)});

    // Do not break Hangul syllable sequences.
    RE * GCB_L = makePropertyExpression("gcb", "l");
    RE * GCB_V = makePropertyExpression("gcb", "v");
    RE * GCB_LV = makePropertyExpression("gcb", "lv");
    RE * GCB_LVT = makePropertyExpression("gcb", "lvt");
    RE * GCB_T = makePropertyExpression("gcb", "t");
    RE * GCX_6 = makeSeq({Behind(GCB_L), Ahead(makeAlt({GCB_L, GCB_V, GCB_LV, GCB_LVT}))});
    RE * GCX_7 = makeSeq({Behind(makeAlt({GCB_LV, GCB_V})), Ahead(makeAlt({GCB_V, GCB_T}))});
    RE * GCX_8 = makeSeq({Behind(makeAlt({GCB_LVT, GCB_T})), Ahead(GCB_T)});

    // Do not break before extendiers or zero-width joiners.
    RE * GCB_EX = makePropertyExpression("gcb", "ex");
    RE * GCB_ZWJ = makePropertyExpression("gcb", "zwj");
    RE * GCX_9 = makeSeq({notBehind(GCB_Control_CR_LF), Ahead(makeAlt({GCB_EX, GCB_ZWJ}))});

    if (extendedGraphemeClusters) {
        RE * GCB_SpacingMark = makePropertyExpression("gcb", "sm");
        RE * GCB_Prepend = makePropertyExpression("gcb", "pp");
        RE * GCX_9a = makeSeq({notBehind(GCB_Control_CR_LF), Ahead(GCB_SpacingMark)});
        RE * GCX_9b = makeSeq({Behind(GCB_Prepend), notAhead(GCB_Control_CR_LF)});
        GCX_9 = makeAlt({GCX_9, GCX_9a, GCX_9b});
    }

    RE * ExtendedPictographic = makePropertyExpression("Extended_Pictographic");
    RE * EmojiSeq = makeSeq({ExtendedPictographic, makeRep(GCB_EX, 0, Rep::UNBOUNDED_REP), GCB_ZWJ});
    RE * GCX_11 = makeSeq({Behind(EmojiSeq), Ahead(ExtendedPictographic)});

    RE * GCB_RI = makePropertyExpression("gcb", "ri");
    // Note: notBehind(RI) == sot | [^RI]
    RE * odd_RI_seq = makeSeq({notBehind(GCB_RI), makeRep(makeSeq({GCB_RI, GCB_RI}), 0, Rep::UNBOUNDED_REP), GCB_RI});
    RE * GCX_12_13 = makeSeq({Behind(odd_RI_seq), Ahead(GCB_RI)});

    //Name * gcb = makePropertyExpression("gcb");
    RE * GCX = makeAlt({GCX_6, GCX_7, GCX_8, GCX_9, GCX_11, GCX_12_13});

    // Otherwise, break everywhere.
    RE * GCB_999 = makeSeq({Behind(makeAny()), Ahead(makeAny())});

    RE * gcb = makeAlt({GCB_1_5, makeDiff(GCB_999, GCX)});

    gcb = UCD::linkAndResolve(gcb);

    return gcb;
}

// Unicode word boundary rules
RE * generateWordBoundaryRule() {
    // Unicode Word Boundary Rules (UAX #29) - Basic Implementation
    // WB1: Break at the start of text, sot ÷ Any
    RE * WB_1 = makeSOT();

    // WB2: Break at the end of text, Any ÷ eot
    RE * WB_2 = makeEOT();

    // WB3: Do not break within CRLF.
    // CR × LF
    RE * WB_CR = makePropertyExpression("wb", "cr");
    RE * WB_LF = makePropertyExpression("wb", "lf");
    RE * WB_Newline = makePropertyExpression("wb", "newline");

    // Combine CR, LF, and Newline for breaking rules
    RE * WB_CRLFNewline = makeAlt({WB_CR, WB_LF, WB_Newline});

    // Do not break between a CR and LF.
    RE * WBX_3 = makeSeq({Behind(WB_CR), Ahead(WB_LF)});

    // break
    // WB3a: Break before Newlines (including CR and LF)
    RE * WB_3a = Behind(WB_CRLFNewline);

    // WB3b: Break after Newlines (including CR and LF)
    RE * WB_3b = Ahead(WB_CRLFNewline);

    // WB3c: Do not break within emoji zwj sequences.
    RE * WBX_ZWJ = makePropertyExpression("wb", "zwj");
    RE * ExtendedPictographic = makePropertyExpression("Extended_Pictographic");
    RE * WBX_3c = makeSeq({Behind(WBX_ZWJ), Ahead(ExtendedPictographic)});

    // WB3d: Keep horizontal whitespace together.
    // WSegSpace × WSegSpace
    RE * WB_WSegSpace = makePropertyExpression("wb", "wsegspace");
    RE * WBX_3d = makeSeq({Behind(WB_WSegSpace), Ahead(WB_WSegSpace)});

    // WB4: Ignore Format and Extend characters
    RE * WB_Extend = makePropertyExpression("wb", "extend");
    RE * WB_Format = makePropertyExpression("wb", "format");

    RE * WB_EFZ_Star = makeRep(makeAlt({WB_Extend, WB_Format, WBX_ZWJ}), 0, Rep::UNBOUNDED_REP);

    //RE * WB_4 = makeAlt({Behind(WB_Extend), Ahead(WB_Format)});
    RE * WBX_4 = makeSeq({Behind(makeAny()), Ahead(makeAlt({WB_Extend, WB_Format, WBX_ZWJ}))});

    // WB5: Do not break between most letters
    // AHLetter × AHLetter
    RE * WB_ALetter = makePropertyExpression("wb", "aletter");
    RE * WB_HebrewLetter = makePropertyExpression("wb", "hebrew_letter");

    // Underscore as part of AHLetter
    // RE * WB_Underscore = makeByte('_');

    RE * WB_AHLetter = makeAlt({WB_ALetter, WB_HebrewLetter});
    RE * WB_AHLetter_EFZ_Star = makeSeq({WB_AHLetter, WB_EFZ_Star});
    RE * WBX_5 = makeSeq({Behind(WB_AHLetter_EFZ_Star), Ahead(WB_AHLetter)});

    // WB6: Do not break letters across certain punctuation
    // AHLetter × (MidLetter | MidNumLetQ) × AHLetter
    //  punctuation inside words
    RE * WB_MidLetter = makePropertyExpression("wb", "midletter");
    RE * WB_MidNumLet = makePropertyExpression("wb", "midnumlet");
    RE * WB_SingleQuote = makePropertyExpression("wb", "single_quote");
    RE * WB_MidNumLetQ = makeAlt({WB_MidNumLet, WB_SingleQuote});
    RE * MidLetter_MidNumLetQ = makeAlt({WB_MidLetter, WB_MidNumLetQ});
    RE * WBX_6 = makeSeq({Behind(WB_AHLetter_EFZ_Star), Ahead(makeSeq({MidLetter_MidNumLetQ, WB_EFZ_Star, WB_AHLetter}))});

    // WB7: AHLetter (MidLetter | MidNumLetQ) × AHLetter
    RE * WBX_7 = makeSeq({
        Behind(makeSeq({WB_AHLetter_EFZ_Star, MidLetter_MidNumLetQ, WB_EFZ_Star})),
           Ahead(WB_AHLetter)
       });
    // WB7a: Hebrew_Letter    ×    Single_Quote
    RE * WBX_7a = makeSeq({Behind(makeSeq({WB_HebrewLetter, WB_EFZ_Star})), Ahead(WB_SingleQuote)});

    // WB7b: Hebrew_Letter    ×    Double_Quote Hebrew_Letter
    RE * WB_DoubleQuote = makePropertyExpression("wb", "double_quote");
    RE * WBX_7b = makeSeq({Behind(makeSeq({WB_HebrewLetter, WB_EFZ_Star})), Ahead(makeSeq({WB_DoubleQuote, WB_EFZ_Star, WB_HebrewLetter}))});

    // WB7c: Hebrew_Letter Double_Quote    ×    Hebrew_Letter
    RE * WBX_7c = makeSeq({Behind(makeSeq({WB_HebrewLetter, WB_EFZ_Star, WB_DoubleQuote, WB_EFZ_Star})), Ahead(WB_HebrewLetter)});

    // WB8: Do not break within sequences of digits, or digits adjacent to letters
    // Numeric × Numeric
    RE * WB_Numeric = makePropertyExpression("wb", "numeric");
    RE * WB_Numeric_EFZ_Star = makeSeq({WB_Numeric, WB_EFZ_Star});

    RE * WBX_8 = makeSeq({Behind(WB_Numeric_EFZ_Star), Ahead(WB_Numeric)});

    // WB9: AHLetter × Numeric
    RE * WBX_9 = makeSeq({Behind(WB_AHLetter_EFZ_Star), Ahead(WB_Numeric)});

    // WB10: Numeric × AHLetter
    RE * WBX_10 = makeSeq({Behind(WB_Numeric_EFZ_Star), Ahead(WB_AHLetter)});

    // am not using WB_midNumLetQ ?
    // WB11: Do not break within NUMERIC sequences
    // Numeric (MidNum | MidNumLetQ)    ×    Numeric
    RE * WB_MidNum = makePropertyExpression("wb", "midnum");
    RE * MidNum_MidNumLetQ = makeAlt({WB_MidNum, WB_MidNumLetQ});
    RE * WBX_11 = makeSeq({Behind(makeSeq({WB_Numeric_EFZ_Star, MidNum_MidNumLetQ, WB_EFZ_Star})), Ahead(WB_Numeric)});

    // WB12: Numeric    ×    (MidNum | MidNumLetQ) Numeric --?
    RE * WBX_12 = makeSeq({Behind(WB_Numeric_EFZ_Star), Ahead(makeSeq({MidNum_MidNumLetQ, WB_EFZ_Star, WB_Numeric}))});

    // WB13:Do not break between Katakana
    RE * WB_Katakana = makePropertyExpression("wb", "katakana");
    RE * WBX_13 = makeSeq({Behind(makeSeq({WB_Katakana, WB_EFZ_Star})), Ahead(WB_Katakana)});

    // WB13a: (AHLetter | Numeric | Katakana | ExtendNumLet) × ExtendNumLet , Do not break from extenders.
    RE * WB_ExtendNumLet = makePropertyExpression("wb", "extendnumlet");
    RE * WB_ALetNumKat = makeAlt({WB_AHLetter, WB_Numeric, WB_Katakana, WB_ExtendNumLet});
    RE * WBX_13a = makeSeq({Behind(makeSeq({WB_ALetNumKat, WB_EFZ_Star})), Ahead(WB_ExtendNumLet)});

    // WB13b:Do not break from extenders.
    RE * WB_ALetNumKat_1 = makeAlt({WB_AHLetter, WB_Numeric, WB_Katakana});
    RE * WBX_13b = makeSeq({Behind(makeSeq({WB_ExtendNumLet, WB_EFZ_Star})), Ahead(WB_ALetNumKat_1)});

    // WB15/16: Do not break within emoji flag sequences, do not break between regional indicator (RI)
    RE * WB_RI = makePropertyExpression("wb", "ri");
    // Note: notBehind(RI) == sot | [^RI]
    RE * odd_RI_seq = makeSeq({notBehind(WB_RI), makeRep(makeSeq({WB_RI, WB_EFZ_Star, WB_RI, WB_EFZ_Star}), 0, Rep::UNBOUNDED_REP), WB_RI, WB_EFZ_Star});
    RE * WBX_15_16 = makeSeq({Behind(odd_RI_seq), Ahead(WB_RI)});

    RE * WB_all = makeAlt({WB_1, WB_2, WB_3a, WB_3b});

    RE * WBX_all = makeAlt({WBX_3, WBX_3c, WBX_3d, WBX_4, WBX_5, WBX_6, WBX_7, WBX_7a, WBX_7b, WBX_7c, WBX_8, WBX_9, WBX_10, WBX_11, WBX_12, WBX_13, WBX_13a, WBX_13b, WBX_15_16});

    // WB999: Break everywhere else.
    RE * WB_999 = makeSeq({Behind(makeAny()), Ahead(makeAny())});

    // Final word boundary rule: break at start/end of text, or break everywhere except where WBX rules apply
    RE * wb = makeAlt({WB_all, makeDiff(WB_999, WBX_all)});

    wb = UCD::linkAndResolve(wb);

    return wb;
}


RE * EnumeratedPropertyBoundary(UCD::EnumeratedPropertyObject * enumObj) {
    unsigned enum_count = enumObj->GetEnumCount();
    std::vector<RE *> assertions;
    auto prop = enumObj->getPropertyCode();
    std::vector<RE *> alts;
    for (unsigned j = 0; j < enum_count; j++) {
        std::string enumVal = enumObj->GetValueEnumName(j);
        RE * expr = makePropertyExpression(UCD::getPropertyFullName(prop), enumVal);
        expr = UCD::linkAndResolve(expr);
        expr = UCD::externalizeProperties(expr);
        alts.push_back(makeSeq({notBehind(expr), Ahead(expr)}));
        alts.push_back(makeSeq({Behind(expr), notAhead(expr)}));
    }
    return makeAlt(alts.begin(), alts.end());
}

const static std::map<RE_TokenizerKind, std::string> PreTokenizerPatterns =

    {{GPT2R50K, R"GPT('(?:[sdmt]|ll|ve|re)| ?\p{L}++| ?\p{N}++| ?[^\s\p{L}\p{N}]++|\s++$|\s+(?!\S)|\s)GPT"},
        {WhitespaceBoundary, R"(\w++|[^\w\s]++)"},
        {WhitespaceSplitBoundary, R"(\S+(?!\S)|\s+(?!\s))"},
        {PunctuationBoundary, R"([[:punct:]]|[^[:punct:]]++)"},
        {DigitBoundary, R"(\d++|[^\d\s]++|\s++)"},
        {ByteLevelBoundary, R"GPT('(?:[sdmt]|ll|ve|re)| ?\p{L}++| ?\p{N}++| ?[^\s\p{L}\p{N}]++|\s+(?!\S)|\s+)GPT"},
        {BertPreTokenizer, R"(\w++|[^\w\s]++)"},
        {SequenceWhitespacePunctuation, R"(\w++|[^\w\s]++)"}};

RE * generateRE_TokenizerRule(RE_TokenizerKind k) {
    auto f = PreTokenizerPatterns.find(k);
    auto & pat = f->second;
    RE * r = re::RE_Parser::parse(pat, re::DEFAULT_MODE, re::RE_Syntax::PCRE, false);
    if (!r) {
        llvm::errs() << "Error: failed to parse Sequence[Whitespace,Punctuation] pattern.\n";
        return nullptr;
    }
    r = UCD::linkAndResolve(r);
    return r;
}

}

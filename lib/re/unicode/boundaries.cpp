#include <re/unicode/boundaries.h>

#include <re/adt/adt.h>
#include <re/adt/re_name.h>
#include <re/printer/re_printer.h>
#include <re/analysis/validation.h>
#include <re/transforms/re_transformer.h>
#include <re/unicode/re_name_resolve.h>
#include <re/unicode/resolve_properties.h>
#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>

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

    
struct WordBoundaryAbsentValidator final : public RE_Validator {
    
    WordBoundaryAbsentValidator()
    : RE_Validator() {}
    
    bool validateName(const Name * n) override {
        return n->getName() != "\\b";
    }
};

bool hasWordBoundary(const RE * re) {
    WordBoundaryAbsentValidator v;
    return !(v.validateRE(re));
}

class NonUnicodeValidator : public RE_Validator {
public:
    NonUnicodeValidator() : RE_Validator("NonUnicodeValidator") {}

    bool validateCC(const CC * cc) override {return cc->getAlphabet() != &cc::Unicode;}

    bool validatePropertyExpression(const PropertyExpression * pe) override {return false;}
};

struct UnicodeLookaheadAbsentValidator final : public RE_Validator {
    UnicodeLookaheadAbsentValidator() : RE_Validator() {}

    bool validateAssertion(const Assertion * a) override {
        if (a->getKind() == Assertion::Kind::LookBehind) return true;
        return NonUnicodeValidator().validateRE(a->getAsserted());
    }

    bool validatePropertyExpression(const PropertyExpression * e) override {
        return e->getKind() != PropertyExpression::Kind::Boundary;
    }

    bool validateName(const Name * n) override {
        RE * defn = n->getDefinition();
        if (defn) {
            return validateRE(defn);
        }
        return true;
    }
};

bool hasUnicodeLookahead(const RE * re) {
    UnicodeLookaheadAbsentValidator v;
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
    // let's start with the three fundamental rules: WB1, WB2, WB3
    
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
    RE * WB_CRLFNewlinw = makeAlt({WB_CR, WB_LF, WB_Newline});
    
    // Do not break between a CR and LF.
    RE * WB_3 = makeSeq({Behind(WB_CR), Ahead(WB_LF)});
    
    // break
    // WB3a: Break before Newlines (including CR and LF)
    RE * WB_3a = Behind(WB_CRLFNewlinw);
    
    // WB3b: Break after Newlines (including CR and LF)
    RE * WB_3b = Ahead(WB_CRLFNewlinw);
    
    // Combine breaking rules (except WB_3 which prevents CR×LF break)
    RE * WB_1_3 = makeAlt({WB_1, WB_2, makeDiff(makeAlt({WB_3a, WB_3b}), WB_3)});
    
    // Combine the "do not break" rules (just WB_3 for now)
    RE * WBX = WB_3;
    
    // WB999: Break everywhere else.
    RE * WB_999 = makeSeq({Behind(makeAny()), Ahead(makeAny())});
    
    // Final word boundary rule: break at start/end of text, or break everywhere except where WBX rules apply
    RE * wb = makeAlt({WB_1_3, makeDiff(WB_999, WBX)});
    
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

class BoundaryPropertyResolver : public RE_Transformer {
public:
    BoundaryPropertyResolver() : RE_Transformer("ResolveBoundaryProperties") {}
    
    RE * transformPropertyExpression(PropertyExpression * propExpr) {
        if (propExpr->getKind() == PropertyExpression::Kind::Codepoint) {
            return propExpr;
        }
        int prop_code = propExpr->getPropertyCode();
        if (propExpr->getPropertyIdentifier() == "g") {
            Name * gcb_name = makeZeroWidth("\\b{g}");
            gcb_name->setDefinition(generateGraphemeClusterBoundaryRule());
            return gcb_name;
        }
        if (propExpr->getPropertyIdentifier() == "w") {
            Name * wb_name = makeZeroWidth("\\b{w}");
            wb_name->setDefinition(generateWordBoundaryRule());
            return wb_name;
        }
        if (prop_code >= 0) {
            auto obj = UCD::getPropertyObject(static_cast<UCD::property_t>(prop_code));
            if ((propExpr->getValueString() == "") && isa<UCD::EnumeratedPropertyObject>(obj)) {
                return EnumeratedPropertyBoundary(cast<UCD::EnumeratedPropertyObject>(obj));
            }
            auto pe = makePropertyExpression(propExpr->getPropertyIdentifier(), propExpr->getValueString());
            RE * a = makeLookAheadAssertion(pe);
            RE * na = makeNegativeLookAheadAssertion(pe);
            RE * b = makeLookBehindAssertion(pe);
            RE * nb = makeNegativeLookBehindAssertion(pe);
            RE * resolved = nullptr;
            if (propExpr->getOperator() == PropertyExpression::Operator::NEq) {
                resolved = makeAlt({makeSeq({b, a}), makeSeq({nb, na})});
            } else {
                resolved = makeAlt({makeSeq({b, na}), makeSeq({nb, a})});
            }
            return resolved;
        }
        re::UnsupportedRE(Printer_RE::PrintRE(propExpr));
    }

};

RE * resolveBoundaryProperties(RE * r) {
    return UCD::linkProperties(BoundaryPropertyResolver().transformRE(r));
}

}

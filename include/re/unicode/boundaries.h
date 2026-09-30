#pragma once

namespace UCD { class EnumeratedPropertyObject;}

namespace re {
    
class RE;
class Name;

class EnumeratedPropertyObject;

bool hasGraphemeClusterBoundary(const RE * re);

RE * resolveGraphemeMode(RE * re, bool inGraphemeMode);

RE * generateGraphemeClusterBoundaryRule(bool extendedGraphemeClusters = true);

RE * generateWordBoundaryRule();

RE * EnumeratedPropertyBoundary(UCD::EnumeratedPropertyObject * enumObj);

// Word Boundary Rules         
RE * generateWordBoundaryRule();

enum RE_TokenizerKind {
    GPT2R50K, WhitespaceBoundary, WhitespaceSplitBoundary, PunctuationBoundary, DigitBoundary, 
    ByteLevelBoundary, BertPreTokenizer, SequenceWhitespacePunctuation, KindCount
};

RE * generateRE_TokenizerRule(RE_TokenizerKind k);
}
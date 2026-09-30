#pragma once

namespace UCD { class EnumeratedPropertyObject;}

namespace re {
    
class RE;
class Name;

class EnumeratedPropertyObject;

// Does the RE contain a boundary expression (e.g., \b{g}, \b{w}, \b{gc})?
bool hasBoundaryExpression(const RE * re);

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
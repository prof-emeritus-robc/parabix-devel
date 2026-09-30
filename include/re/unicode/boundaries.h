#pragma once

namespace UCD { class EnumeratedPropertyObject;}

namespace re {
    
class RE;
class Name;

class EnumeratedPropertyObject;

bool hasGraphemeClusterBoundary(const RE * re);

bool hasSimpleWordBoundary(const RE * re);

bool hasLevel2WordBoundary(const RE * re);

bool hasUnicodeLookahead(const RE * re);

RE * resolveGraphemeMode(RE * re, bool inGraphemeMode);

RE * generateGraphemeClusterBoundaryRule(bool extendedGraphemeClusters = true);

RE * generateWordBoundaryRule();

RE * EnumeratedPropertyBoundary(UCD::EnumeratedPropertyObject * enumObj);

// Word Boundary Rules         
RE * generateWordBoundaryRule();

// GPT-2 r50k pretokenizer regex (PCRE). Returns a parsed RE representing
// the GPT-2 pretokenizer pattern used by Hugging Face's tokenizer.
RE * generateGPT2R50KRule();

// Whitespace Pre-Tokenizer Rule
RE * generateWhitespaceBoundaryRule();

// WhitespaceSplit Pre-Tokenizer Rule
RE * generateWhitespaceSplitBoundaryRule();

// Punctuation Pre-Tokenizer Rule
RE * generatePunctuationBoundaryRule();

// Digit Pre-Tokenizer Rule
RE * generateDigitBoundaryRule();

// ByteLevel Pre-Tokenizer Rule
RE * generateByteLevelBoundaryRule();

// BERT Pre-Tokenizer Rule
RE * generateBertPreTokenizerRule();

// Sequence Pre-Tokenizer: Whitespace then Punctuation
RE * generateSequenceWhitespacePunctuationRule();

enum RE_TokenizerKind {
    GPT2R50K, WhitespaceBoundary, WhitespaceSplitBoundary, PunctuationBoundary, DigitBoundary, 
    ByteLevelBoundary, BertPreTokenizer, SequenceWhitespacePunctuation, KindCount
};

RE * generateRE_TokenizerRule(RE_TokenizerKind k);
}
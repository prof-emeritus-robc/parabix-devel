#pragma once

namespace UCD { class EnumeratedPropertyObject;}

namespace re {
    
class RE;
class Name;

class EnumeratedPropertyObject;

bool hasGraphemeClusterBoundary(const RE * re);

bool hasWordBoundary(const RE * re);

bool hasUnicodeLookahead(const RE * re);

RE * resolveGraphemeMode(RE * re, bool inGraphemeMode);

RE * generateGraphemeClusterBoundaryRule(bool extendedGraphemeClusters = true);

RE * EnumeratedPropertyBoundary(UCD::EnumeratedPropertyObject * enumObj);

RE * resolveBoundaryProperties(RE * r);

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
}


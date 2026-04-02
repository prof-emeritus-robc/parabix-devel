#!/usr/bin/env python3
"""
HuggingFace Pre-tokenizer Reference Tests
"""

import sys
import os
import argparse
from tokenizers.pre_tokenizers import (
    Whitespace, WhitespaceSplit, ByteLevel, BertPreTokenizer,
    Punctuation, Digits, CharDelimiterSplit, Sequence, Split,
)
from tokenizers import Regex

DEFAULT_INPUT = "/Users/munizahashim/parabix-devel/build19/test.txt"

parser = argparse.ArgumentParser()
parser.add_argument("input_file", nargs="?", default=DEFAULT_INPUT, help="Path to input text file")
args = parser.parse_args()

with open(args.input_file, "r", encoding="utf-8") as f:
    TEXT = f.read().strip()

PRETOKENIZERS = {
    "whitespace":                    WhitespaceSplit(),  # behaves like "split on whitespace"
    "whitespacesplit":               Whitespace(),  # behaves like "split on whitespace + punct" 
    "bytelevel":                     ByteLevel(add_prefix_space=False),  # default in GPT-2
    # "bert":                          BertPreTokenizer(),
    "punctuation":                   Punctuation(),
    "digits":                        Digits(individual_digits=False),
    "chardelimiter":                 CharDelimiterSplit(","),
    "sequence_whitespace_punct":     Sequence([WhitespaceSplit(), Punctuation()]),
    "simplewordboundaries":          Split(pattern=Regex(r"\w+"), behavior="isolated"), # both the words and the separators to survive as tokens
    "uax29":                         None,  # No direct HF equivalent
}

OUTPUT_FILE = os.path.join(os.path.dirname(__file__), "hf_pretokenizers_output.txt")

def run(out):
    out.write(f"Input: {repr(TEXT)}\n\n")
    for name, pt in PRETOKENIZERS.items():
        out.write(f"[{name}]\n")
        if pt is None:
            out.write("  No HuggingFace equivalent for uax29 (needs ICU/PyICU)\n\n")
            continue
        result = pt.pre_tokenize_str(TEXT)
        tokens = [tok for tok, _ in result]
        out.write(f"  Total tokens: {len(tokens)}\n")
        for i, tok in enumerate(tokens):
            out.write(f"  {i+1:3}. {repr(tok)}\n")
        out.write("\n")

run(sys.stdout)

with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
    run(f)

print(f"Output written to: {OUTPUT_FILE}")

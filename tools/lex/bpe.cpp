/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "bpe.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>

bool BPETokenizer::isLoaded() const {
    return !vocab_.empty() && !merges_.empty();
}

// Reads vocab.json from disk and populates two tables:
// loadVocab - parses the file 
bool BPETokenizer::loadVocab(const std::string & path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open vocab file: " << path << "\n";
        return false;
    }
    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception & e) {
        std::cerr << "BPE: failed to parse vocab file: " << e.what() << "\n";
        return false;
    }
    for (auto & [key, val] : j.items()) {
        int value = val.get<int>();
        vocab_[key] = value;
        if (value >= 0) {
            if ((size_t)value >= idToToken_.size())
                idToToken_.resize((size_t)value + 1);
            idToToken_[(size_t)value] = key;
        }
    }
    std::cerr << "BPE: loaded vocab with " << vocab_.size() << " tokens\n";
    return !vocab_.empty();
}

// Reads merges.txt and populates merges_[{left,right}] = rank.
// The rank is the line number (0 = first merge = highest priority).
// Lines starting with '#' are comments (e.g. "#version: 0.2") and are skipped.
bool BPETokenizer::loadMerges(const std::string & path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open merges file: " << path << "\n";
        return false;
    }

    std::string line;
    int rank = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue; // skip blank lines and comments

        // Split on the single space that separates left and right symbol.
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue; // malformed line, skip it
        std::string left  = line.substr(0, sp);
        std::string right = line.substr(sp + 1);

        // Strip trailing '\r' in case the file uses Windows CRLF line endings.
        if (!right.empty() && right.back() == '\r') right.pop_back();

        // Store the merge rule. Lower rank = applied first = higher priority.
        merges_[{left, right}] = rank++;
    }

    std::cerr << "BPE: loaded " << merges_.size() << " merge rules\n";
    return !merges_.empty();
}

//  BPE encode
// Splits an already-byte-encoded UTF-8 string into individual codepoint strings.
std::vector<std::string> BPETokenizer::splitByCodepoint(const std::string & s) {
    std::vector<std::string> symbols;
    symbols.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {   // parallelize
        // Determine how many bytes this UTF-8 codepoint occupies.
        unsigned char c = (unsigned char)s[i];
        size_t len = (c < 0x80) ? 1 :   // 0xxxxxxx  — 1-byte codepoint
                     (c < 0xE0) ? 2 :   // 110xxxxx  — 2-byte codepoint (e.g. Ġ)
                     (c < 0xF0) ? 3 : 4; // 1110xxxx / 11110xxx — 3 or 4 bytes
        symbols.push_back(s.substr(i, len)); // one codepoint as a UTF-8 string
        i += len;
    }
    return symbols;
}

// Returns a list of every adjacent (left, right) symbol pair in the sequence.
std::vector<std::pair<std::string,std::string>> BPETokenizer::getPairs(
        const std::vector<std::string> & symbols) {
    std::vector<std::pair<std::string,std::string>> pairs;
    pairs.reserve(symbols.size());   
    for (size_t i = 0; i + 1 < symbols.size(); i++)
        pairs.push_back({symbols[i], symbols[i+1]});
    return pairs;
}
// Core BPE merge loop. Takes a symbol sequence and iteratively merges
std::vector<std::string> BPETokenizer::applyBPE(
        const std::vector<std::string> & input) const {
    if (input.size() <= 1) return input;

    // Build a cache key by joining symbols with '\0' separators.
    std::string cacheKey;
    cacheKey.reserve(64);
    for (const auto & s : input) { cacheKey += s; cacheKey += '\0'; }
    auto it = cache_.find(cacheKey);
    if (it != cache_.end()) return it->second; // cache hit — skip the loop

    std::vector<std::string> word = input; // working copy we will modify

    while (word.size() > 1) {
        // Get every adjacent pair in the current sequence.
        auto pairs = getPairs(word);

        // Scan all pairs to find the one with the lowest rank in merges_.
        // Lower rank = earlier in merges.txt = higher priority = applied first.
        int bestRank = std::numeric_limits<int>::max(); // start with worst possible rank // parallelize
        std::pair<std::string,std::string> bestPair;
        bool found = false;
        for (const auto & p : pairs) {
            auto mit = merges_.find(p); // O(log n) lookup in std::map
            if (mit != merges_.end() && mit->second < bestRank) {
                bestRank = mit->second;
                bestPair = p;
                found    = true;
            }
        }
        if (!found) break; // no pair in the sequence is a known merge rule — done

        // Rebuild the sequence, replacing every occurrence of bestPair
        std::vector<std::string> newWord;
        newWord.reserve(word.size());
        size_t i = 0;
        while (i < word.size()) {
            if (i + 1 < word.size()
                    && word[i]   == bestPair.first
                    && word[i+1] == bestPair.second) {
                newWord.push_back(bestPair.first + bestPair.second); // fuse the pair
                i += 2; // skip both symbols
            } else {
                newWord.push_back(word[i]); // keep symbol unchanged
                i++;
            }
        }
        word = std::move(newWord); // replace sequence with merged version
    }

    // Store the result in the cache so repeated words skip the merge loop.
    cache_[cacheKey] = word;
    return word;
}

// encoding function: takes a list of already-byte-encoded pre-tokens and returns a flat list of integer token IDs.
// encodePreTokens - the lookup-and-write
std::vector<int> BPETokenizer::encodePreTokens(
        const std::vector<std::string> & preTokens) const {
    std::vector<int> ids;
    for (const auto & token : preTokens) {  
        // split by Unicode codepoint — each codepoint is one BPE symbol.
        auto symbols = splitByCodepoint(token);

        // iteratively merge symbol pairs using the loaded merge rules.
        auto merged  = applyBPE(symbols);

        // look up each merged symbol in the vocab and emit its ID.
        for (const auto & sym : merged) { // parallelize 
            auto vit = vocab_.find(sym);
            ids.push_back(vit != vocab_.end() ? vit->second : -1);
        }
    }
    return ids;
}

//  Decode
// Converts a single token ID back to its string representation.
std::string BPETokenizer::decodeToken(int id) const {
    if (id < 0 || (size_t)id >= idToToken_.size()) return "";
    return idToToken_[(size_t)id];
}

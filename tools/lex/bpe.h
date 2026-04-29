/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <utility>

class BPETokenizer {
public:
    // Load token->id mapping from HuggingFace vocab.json.
    bool loadVocab(const std::string & path);

    // Load merge rules from HuggingFace merges.txt.
    bool loadMerges(const std::string & path);

    bool isLoaded() const;
    size_t vocabSize() const { return vocab_.size(); }

    // Splits each pre-token by Unicode codepoint 
    std::vector<int> encodePreTokens(const std::vector<std::string> & preTokens) const;

    // Return the token string for a given ID, or "" if out of range.
    std::string decodeToken(int id) const;

private:
    // Split an already-byte-encoded UTF-8 string into individual codepoint strings.
    static std::vector<std::string> splitByCodepoint(const std::string & s);

    // Iteratively apply the highest-priority merge rule until none apply.
    std::vector<std::string> applyBPE(const std::vector<std::string> & symbols) const;

    // Return all adjacent (left, right) symbol pairs.
    static std::vector<std::pair<std::string,std::string>> getPairs(
        const std::vector<std::string> & symbols);

    std::unordered_map<std::string, int>                         vocab_;            // token string -> ID
    std::vector<std::string>                                     idToToken_;        // ID -> token string
    std::map<std::pair<std::string,std::string>, int>            merges_;           // pair -> rank
    mutable std::unordered_map<std::string, std::vector<std::string>> cache_;
};

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <re/parse/replacement_parser.h>

#include <map>
#include <vector>
#include <re/adt/adt.h>
#include <re/analysis/re_inspector.h>
#include <re/analysis/replacement_analysis.h>

namespace re {

namespace {

// The number of references to each capture (by name) within an RE: the
// instances of the references of a replacement follow them.
struct ReferenceCounter final : public RE_Inspector {
    ReferenceCounter(std::map<std::string, unsigned> & counts)
    : RE_Inspector(NameProcessingMode::None, InspectionMode::IgnoreNonUnique), mCounts(counts) {}
    void inspectReference(Reference * r) override {
        unsigned & count = mCounts[r->getName()];
        count = std::max(count, r->getInstance() + 1);
    }
    std::map<std::string, unsigned> & mCounts;
};

bool isDigit(char c) {
    return (c >= '0') && (c <= '9');
}

bool isNameChar(char c) {
    return isDigit(c) || ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_');
}

int hexValue(char c) {
    if (isDigit(c)) return c - '0';
    if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
    return -1;
}

}

RE * parseReplacement(const std::string & text, RE * search, std::string & error) {
    const std::map<std::string, Capture *> captures = capturesByName(search);
    std::map<std::string, unsigned> instances;
    ReferenceCounter(instances).inspectRE(search);
    std::vector<RE *> items;
    size_t i = 0;
    auto fail = [&](const std::string & msg) -> RE * {
        error = msg + " at position " + std::to_string(i) + " of the replacement";
        return nullptr;
    };
    // A reference to the capture identified by id: a number (the capture named
    // "\n" or "n") or a name.
    auto reference = [&](std::string id) -> bool {
        Capture * capture = nullptr;
        if (isDigit(id[0])) {
            if (id.size() > 9) return false;
            id = std::to_string(std::stoul(id));
            auto f = captures.find("\\" + id);
            if (f == captures.end()) f = captures.find(id);
            if (f != captures.end()) capture = f->second;
        } else {
            auto f = captures.find(id);
            if (f != captures.end()) capture = f->second;
        }
        if (capture == nullptr) return false;
        const std::string name = capture->getName();
        items.push_back(makeReference(name, capture, instances[name]++));
        return true;
    };
    auto literal = [&](codepoint_t cp) {
        items.push_back(makeCC(cp, &cc::Unicode));
    };
    const size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        if (c == '$') {
            if (i + 1 == n) return fail("$ at the end");
            const char d = text[i + 1];
            if (d == '$') {
                literal('$');
                i += 2;
            } else if (isDigit(d)) {
                size_t j = i + 1;
                while ((j < n) && isDigit(text[j])) j++;
                const std::string id = text.substr(i + 1, j - i - 1);
                if (!reference(id)) return fail("No capture " + id);
                i = j;
            } else if (d == '{') {
                const size_t j = text.find('}', i + 2);
                if (j == std::string::npos) return fail("Unterminated ${");
                const std::string id = text.substr(i + 2, j - i - 2);
                if (id.empty()) return fail("Empty ${}");
                for (const char x : id) {
                    if (!isNameChar(x)) return fail("Invalid capture name " + id);
                }
                if (!reference(id)) return fail("No capture " + id);
                i = j + 1;
            } else {
                return fail("$ must be followed by a capture number, {number}, {name} or $");
            }
        } else if (c == '\\') {
            if (i + 1 == n) return fail("\\ at the end");
            const char d = text[i + 1];
            if ((d >= '1') && (d <= '9')) {
                if (!reference(std::string(1, d))) return fail("No capture " + std::string(1, d));
                i += 2;
                continue;
            }
            static const std::map<char, codepoint_t> escapes = {
                {'\\', '\\'}, {'$', '$'}, {'t', '\t'}, {'n', '\n'}, {'r', '\r'}, {'f', '\f'}, {'v', '\v'},
                {'e', 0x1B}, {'a', 0x07}};
            const auto e = escapes.find(d);
            if (e != escapes.end()) {
                literal(e->second);
                i += 2;
            } else if (d == 'x') {
                size_t j = i + 2;
                codepoint_t cp = 0;
                if ((j < n) && (text[j] == '{')) {
                    j++;
                    const size_t first = j;
                    while ((j < n) && (hexValue(text[j]) >= 0) && (j - first < 6)) {
                        cp = cp * 16 + hexValue(text[j]);
                        j++;
                    }
                    if ((j == first) || (j == n) || (text[j] != '}')) return fail("Malformed \\x{...}");
                    j++;
                } else {
                    for (unsigned k = 0; k < 2; k++, j++) {
                        if ((j == n) || (hexValue(text[j]) < 0)) return fail("Malformed \\xHH");
                        cp = cp * 16 + hexValue(text[j]);
                    }
                }
                if ((cp > 0x10FFFF) || ((cp >= 0xD800) && (cp <= 0xDFFF))) return fail("Invalid codepoint");
                literal(cp);
                i = j;
            } else {
                return fail("Unknown escape \\" + std::string(1, d));
            }
        } else {
            // A UTF-8 encoded character.
            const unsigned char b = static_cast<unsigned char>(c);
            unsigned length = 0;
            codepoint_t cp = 0;
            if (b < 0x80) {
                length = 1; cp = b;
            } else if ((b & 0xE0) == 0xC0) {
                length = 2; cp = b & 0x1F;
            } else if ((b & 0xF0) == 0xE0) {
                length = 3; cp = b & 0x0F;
            } else if ((b & 0xF8) == 0xF0) {
                length = 4; cp = b & 0x07;
            } else {
                return fail("Invalid UTF-8");
            }
            if (i + length > n) return fail("Invalid UTF-8");
            for (unsigned k = 1; k < length; k++) {
                const unsigned char x = static_cast<unsigned char>(text[i + k]);
                if ((x & 0xC0) != 0x80) return fail("Invalid UTF-8");
                cp = (cp << 6) | (x & 0x3F);
            }
            static const codepoint_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
            if ((cp < minimum[length]) || (cp > 0x10FFFF) || ((cp >= 0xD800) && (cp <= 0xDFFF))) {
                return fail("Invalid UTF-8");
            }
            literal(cp);
            i += length;
        }
    }
    return makeSeq(items.begin(), items.end());
}

}

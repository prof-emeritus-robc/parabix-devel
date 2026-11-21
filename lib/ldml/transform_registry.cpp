/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <ldml/transform_registry.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <boost/filesystem.hpp>
#include <llvm/Support/Casting.h>
#include <ucd/data/PropertyAliases.h>
#include <ucd/data/PropertyObjects.h>
#include <ucd/data/PropertyObjectTable.h>

namespace ldml {

static std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {return std::tolower(c);});
    return s;
}

static std::vector<std::string> splitSpaces(const std::string & s) {
    std::vector<std::string> words;
    std::istringstream in(s);
    std::string w;
    while (in >> w) words.push_back(w);
    return words;
}

//  The name with the source and target in lower case, each replaced by its
//  Unicode script code if it names a script.  Empty if the name is not of
//  the form Source-Target[/Variant].
static std::string scriptNormalized(const std::string & name) {
    const size_t slash = name.find('/');
    const std::string id = name.substr(0, slash);
    const size_t hyphen = id.find('-');
    if (hyphen == std::string::npos || id.find('-', hyphen + 1) != std::string::npos) return "";
    auto * sc = llvm::cast<UCD::EnumeratedPropertyObject>(UCD::getPropertyObject(UCD::sc));
    auto norm = [sc](const std::string & part) {
        const int code = sc->GetPropertyValueEnumCode(part);
        return lowerCase(code < 0 ? part : sc->GetValueEnumName(code));
    };
    const std::string variant = slash == std::string::npos ? "" : lowerCase(name.substr(slash));
    return norm(id.substr(0, hyphen)) + "-" + norm(id.substr(hyphen + 1)) + variant;
}

//  Built-in transforms of UTS #35 Part 2, with ICU's names for the Hex
//  variants.  Any-Hex without a variant is the Java form; Hex-Any without one
//  decodes all the forms (as ICU's).
struct BuiltIn {
    const char * name;
    std::vector<const char *> aliases;
};

static const std::vector<BuiltIn> builtInTransforms = {
    {"Any-NFC", {}}, {"Any-NFD", {}}, {"Any-NFKC", {}}, {"Any-NFKD", {}},
    {"Any-FCD", {}}, {"Any-FCC", {}},
    {"Any-Lower", {}}, {"Any-Upper", {}}, {"Any-Title", {}}, {"Any-CaseFold", {}},
    {"Any-Null", {}}, {"Any-Remove", {}},
    {"Any-Name", {}}, {"Name-Any", {}},
    {"Any-Hex/Java", {"Any-Hex"}}, {"Any-Hex/C", {}}, {"Any-Hex/Perl", {}},
    {"Any-Hex/Unicode", {}}, {"Any-Hex/XML", {}}, {"Any-Hex/XML10", {}},
    {"Hex-Any", {}}, {"Hex-Any/Java", {}}, {"Hex-Any/C", {}}, {"Hex-Any/Perl", {}},
    {"Hex-Any/Unicode", {}}, {"Hex-Any/XML", {}}, {"Hex-Any/XML10", {}},
    {"Any-BreakInternal", {}},
};

TransformRegistry::TransformRegistry() {
    for (const BuiltIn & b : builtInTransforms) {
        TransformEntry e;
        e.canonicalName = b.name;
        for (const char * a : b.aliases) e.aliases.push_back(a);
        addEntry(std::move(e), nullptr);
    }
}

void TransformRegistry::addName(size_t entry, const std::string & name, std::vector<std::string> * warnings) {
    const auto f = mIndex.emplace(lowerCase(name), entry);
    if (!f.second && f.first->second != entry && warnings) {
        const TransformEntry & prior = mEntries[f.first->second];
        warnings->push_back(mEntries[entry].file + ": name " + name + " already registered for " +
                            prior.canonicalName + (prior.isBuiltIn() ? " (built-in)" : " in " + prior.file));
    }
    const std::string normalized = scriptNormalized(name);
    if (!normalized.empty()) mScriptIndex.emplace(normalized, entry);
}

size_t TransformRegistry::addEntry(TransformEntry && e, std::vector<std::string> * warnings) {
    const size_t idx = mEntries.size();
    mEntries.push_back(std::move(e));
    const TransformEntry & entry = mEntries[idx];
    addName(idx, entry.canonicalName, warnings);
    for (const std::string & a : entry.aliases) addName(idx, a, warnings);
    return idx;
}

//  Parses the attributes of the start tag text following the element name.
static std::map<std::string, std::string> parseAttributes(const std::string & tag) {
    std::map<std::string, std::string> attrs;
    size_t pos = 0;
    for (;;) {
        const size_t eq = tag.find('=', pos);
        if (eq == std::string::npos) break;
        size_t nameEnd = eq;
        while (nameEnd > pos && std::isspace(static_cast<unsigned char>(tag[nameEnd - 1]))) nameEnd--;
        size_t nameStart = nameEnd;
        while (nameStart > pos && !std::isspace(static_cast<unsigned char>(tag[nameStart - 1]))) nameStart--;
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) q++;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) break;
        const size_t close = tag.find(tag[q], q + 1);
        if (close == std::string::npos) break;
        attrs[tag.substr(nameStart, nameEnd - nameStart)] = tag.substr(q + 1, close - q - 1);
        pos = close + 1;
    }
    return attrs;
}

void TransformRegistry::loadFile(const std::string & path, std::vector<std::string> & warnings) {
    std::ifstream in(path);
    if (!in) {
        warnings.push_back(path + ": cannot be read");
        return;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string xml = buf.str();
    bool found = false;
    for (size_t pos = xml.find("<transform"); pos != std::string::npos; pos = xml.find("<transform", pos + 1)) {
        const size_t nameEnd = pos + std::string("<transform").size();
        if (nameEnd >= xml.size() || !std::isspace(static_cast<unsigned char>(xml[nameEnd]))) continue;
        const size_t tagEnd = xml.find('>', nameEnd);
        if (tagEnd == std::string::npos) break;
        found = true;
        auto attrs = parseAttributes(xml.substr(nameEnd, tagEnd - nameEnd));
        const std::string & source = attrs["source"];
        const std::string & target = attrs["target"];
        if (source.empty() || target.empty()) {
            warnings.push_back(path + ": <transform> without source and target");
            continue;
        }
        const std::string variant = attrs["variant"].empty() ? "" : "/" + attrs["variant"];
        const std::string direction = attrs.count("direction") ? attrs["direction"] : "both";
        const bool internal = attrs["visibility"] == "internal";
        if (direction == "forward" || direction == "both") {
            TransformEntry e;
            e.canonicalName = source + "-" + target + variant;
            e.file = path;
            e.internal = internal;
            e.aliases = splitSpaces(attrs["alias"]);
            addEntry(std::move(e), &warnings);
        }
        if (direction == "backward" || direction == "both") {
            TransformEntry e;
            e.canonicalName = target + "-" + source + variant;
            e.file = path;
            e.direction = TransformDirection::Backward;
            e.internal = internal;
            e.aliases = splitSpaces(attrs["backwardAlias"]);
            addEntry(std::move(e), &warnings);
        }
        if (direction != "forward" && direction != "backward" && direction != "both") {
            warnings.push_back(path + ": unknown direction \"" + direction + "\"");
        }
    }
    if (!found) warnings.push_back(path + ": no <transform> element");
}

bool TransformRegistry::loadDirectory(const std::string & dir, std::vector<std::string> & warnings) {
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    std::vector<std::string> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".xml") files.push_back(it->path().string());
    }
    if (ec) return false;
    // Sorted for a deterministic resolution of conflicting names.
    std::sort(files.begin(), files.end());
    for (const std::string & f : files) loadFile(f, warnings);
    return true;
}

//  A BCP 47 transform ID (RFC 6497: the tag of the result, -t-, the tag of
//  the source, then fields: a key of a letter and a digit, and a value of one
//  or more subtags), lower-cased, with its field values in canonical form: the
//  registered aliases of values replaced by their registered names, as CLDR's
//  registry of the t extension gives them (common/bcp47/transform.xml for m0,
//  transform-destination.xml for d0 and s0, CLDR 41 and later; not in every
//  checkout).  A name that begins with a d0 or s0 field and then -t- and a
//  language tag (d0-morse-t-am-ethi, as a CLDR test file is named) is not
//  well-formed BCP 47; it is read as that tag with the field after -t-
//  (am-ethi-t-d0-morse).  Returns "" if the name is not such an ID, or is
//  already canonical.
static std::string canonicalTransformTag(const std::string & name) {
    struct Alias {
        const char * key;
        std::vector<std::string> alias;
        const char * value;
    };
    static const std::vector<Alias> aliases = {
        {"m0", {"beta", "metsehaf"}, "betamets"},
        {"m0", {"ies", "jes"}, "iesjes"},
        {"m0", {"tekie", "alibekit"}, "tekieali"},
        {"m0", {"names"}, "prprname"},
        {"m0", {"c"}, "c11"},
        {"d0", {"publishing"}, "publish"},
        {"d0", {"fullwidth"}, "fwidth"},
        {"d0", {"halfwidth"}, "hwidth"},
        {"d0", {"name"}, "charname"},
        {"d0", {"numericpinyin"}, "npinyin"},
        {"s0", {"publishing"}, "publish"},
        {"s0", {"numericpinyin"}, "npinyin"},
    };
    auto isKey = [](const std::string & t) {
        return t.size() == 2 && std::isalpha(static_cast<unsigned char>(t[0])) && std::isdigit(static_cast<unsigned char>(t[1]));
    };
    std::vector<std::string> tags;
    const std::string lower = lowerCase(name);
    for (size_t pos = 0;;) {
        const size_t dash = lower.find('-', pos);
        tags.push_back(lower.substr(pos, dash == std::string::npos ? std::string::npos : dash - pos));
        if (dash == std::string::npos) break;
        pos = dash + 1;
    }
    auto t = std::find(tags.begin(), tags.end(), "t");
    if (t == tags.end() || t == tags.begin()) return "";
    std::vector<std::string> result(tags.begin(), t), source(t + 1, tags.end());
    //  (The fields written before -t-: moved after it.)
    if (isKey(result.front())) {
        if ((result.front() != "d0" && result.front() != "s0") || source.empty() || isKey(source.front())) return "";
        std::vector<std::string> fields = result;
        result.clear();
        size_t i = 0;
        while (i < source.size() && !isKey(source[i])) result.push_back(source[i++]);
        std::vector<std::string> rest(source.begin() + static_cast<std::ptrdiff_t>(i), source.end());
        source.clear();
        source.insert(source.end(), fields.begin(), fields.end());
        source.insert(source.end(), rest.begin(), rest.end());
    }
    //  The field values, each alias replaced where it occurs as a run of subtags.
    std::vector<std::string> out;
    std::string key;
    for (size_t i = 0; i < source.size();) {
        if (isKey(source[i])) {
            key = source[i];
            out.push_back(source[i++]);
            continue;
        }
        bool replaced = false;
        for (const Alias & a : aliases) {
            if (key != a.key || i + a.alias.size() > source.size()) continue;
            if (!std::equal(a.alias.begin(), a.alias.end(), source.begin() + static_cast<std::ptrdiff_t>(i))) continue;
            out.push_back(a.value);
            i += a.alias.size();
            replaced = true;
            break;
        }
        if (!replaced) out.push_back(source[i++]);
    }
    std::string canonical;
    for (const std::string & r : result) canonical += r + "-";
    canonical += "t";
    for (const std::string & o : out) canonical += "-" + o;
    return canonical == lower ? "" : canonical;
}

const TransformEntry * TransformRegistry::lookup(const std::string & name) const {
    const std::string key = lowerCase(name);
    auto f = mIndex.find(key);
    if (f == mIndex.end()) {
        // An ID without a source is an Any-source ID.
        const std::string id = key.substr(0, key.find('/'));
        if (id.find('-') == std::string::npos) f = mIndex.find("any-" + key);
    }
    if (f != mIndex.end()) return &mEntries[f->second];
    //  A BCP 47 transform ID, its field values with registered aliases.
    const std::string canonical = canonicalTransformTag(name);
    if (!canonical.empty()) {
        f = mIndex.find(canonical);
        if (f != mIndex.end()) return &mEntries[f->second];
    }
    const std::string normalized = scriptNormalized(name);
    if (normalized.empty()) return nullptr;
    f = mScriptIndex.find(normalized);
    return f == mScriptIndex.end() ? nullptr : &mEntries[f->second];
}

std::string defaultTransformDirectory() {
    const char * home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/cldr/common/transforms";
}

}

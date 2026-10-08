/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <map>
#include <string>
#include <vector>

//  Names of LDML transforms (UTS #35 Part 2).
//
//  A transform ID has the form Source-Target or Source-Target/Variant.  The
//  canonical name of a transform defined by a CLDR file is built from the
//  source, target and variant attributes of its <transform> element:
//  Source-Target[/Variant] for the forward direction, Target-Source[/Variant]
//  for the backward direction.  The direction attribute ("forward",
//  "backward" or "both") says which of these exist.  The alias and
//  backwardAlias attributes are space-separated lists of further names for
//  the forward and backward transforms.
//
//  Built-in transforms (Any-NFD, Any-Lower, Any-Hex/Java, ...) are defined
//  algorithmically rather than by a file.
//
//  Lookup ignores case, and an ID without a source ("NFD", "Lower") means
//  the Any source ("Any-NFD", "Any-Lower"), as in ICU.  Also as in ICU, a
//  Unicode script name or code may stand for any other name or code of the
//  same script ("Hira-Latin" finds Hira-Latn).

namespace ldml {

enum class TransformDirection {Forward, Backward};

struct TransformEntry {
    std::string canonicalName;
    // The defining CLDR file; empty for a built-in transform.
    std::string file;
    TransformDirection direction = TransformDirection::Forward;
    // visibility="internal": used by other transforms, not for direct use.
    bool internal = false;
    std::vector<std::string> aliases;
    bool isBuiltIn() const {return file.empty();}
};

class TransformRegistry {
public:
    // Registers the built-in transforms.
    TransformRegistry();
    // Registers the transforms of every *.xml file in the directory.
    // Problems (unreadable files, files without a <transform> element,
    // names already registered for a different transform) are appended
    // to warnings.  Returns false if the directory cannot be read.
    bool loadDirectory(const std::string & dir, std::vector<std::string> & warnings);
    // Registers the transform(s) defined by one CLDR transform file.
    void loadFile(const std::string & path, std::vector<std::string> & warnings);
    // Returns nullptr for an unknown name.
    const TransformEntry * lookup(const std::string & name) const;
    const std::vector<TransformEntry> & entries() const {return mEntries;}
private:
    size_t addEntry(TransformEntry && e, std::vector<std::string> * warnings);
    void addName(size_t entry, const std::string & name, std::vector<std::string> * warnings);
    std::vector<TransformEntry> mEntries;
    std::map<std::string, size_t> mIndex;   // lower-cased name -> entry
    std::map<std::string, size_t> mScriptIndex;   // name with scripts as lower-cased codes -> entry
};

// The default location of the CLDR transform files: $HOME/cldr/common/transforms.
std::string defaultTransformDirectory();

}

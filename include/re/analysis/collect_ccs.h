#pragma once

#include <vector>
#include <set>
#include <re/analysis/re_inspector.h>

namespace UCD { class UnicodeSet; }

namespace cc { class Alphabet; }
namespace re {

class RE;
class CC;

CC * unionCC(RE * re,
             re::NameProcessingMode m = re::NameProcessingMode::ProcessDefinition);

using CC_Set = std::vector<CC *>;

CC_Set collectCCs(RE * const re, const cc::Alphabet & a,
                  re::NameProcessingMode m = re::NameProcessingMode::None);

// The codepoints that a match of an RE may consume, including those that its
// assertions may consume (unlike unionCC), and those of the definitions of its
// names: no match of the RE, and no assertion within it, can reach past a
// character outside them.  False if they are not known (an unresolved
// property or name, CCs of another alphabet).
bool matchableCodepoints(RE * re, UCD::UnicodeSet & chars);


}

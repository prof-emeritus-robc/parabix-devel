#pragma once

#include <vector>
#include <set>
#include <re/analysis/re_inspector.h>

namespace cc { class Alphabet; }
namespace re {

class RE;
class CC;

CC * unionCC(RE * re,
             re::NameProcessingMode m = re::NameProcessingMode::ProcessDefinition);

using CC_Set = std::vector<CC *>;

CC_Set collectCCs(RE * const re, const cc::Alphabet & a,
                  re::NameProcessingMode m = re::NameProcessingMode::None);


}

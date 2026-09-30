#pragma once

#include <set>
#include <memory>
#include <re/transforms/re_transformer.h>
#include <re/adt/adt.h>

namespace cc { class MultiplexedAlphabet; }

namespace re {
    class RE;

RE * transformCCs(const cc::MultiplexedAlphabet * const mpx, RE * r,
                  re::NameTransformationMode mode = re::NameTransformationMode::None);

inline RE * transformCCs(const std::shared_ptr<cc::MultiplexedAlphabet> & mpx, RE * r,
                         re::NameTransformationMode mode =  re::NameTransformationMode::None) {
    return transformCCs(mpx.get(), r, mode);
}
}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

//  The ICU implementation of transforms, for tconv --use-ICU (built with ICU
//  if CMake finds it: TCONV_WITH_ICU).  A transform defined by a CLDR file is
//  made by ICU from the rules of that file (createFromRules), as tconv reads
//  them, after the transforms its rules invoke that are defined by CLDR files,
//  registered under the names by which they are invoked; a built-in transform
//  is ICU's own (createInstance).

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tconv {

class ICUTransform {
public:
    //  Whether tconv was built with ICU.
    static bool available();
    //  Registers a transform made from rules (the tRule text of a CLDR file),
    //  in a direction, as the name given (for the rules that invoke it), or
    //  gives the reason why not.
    static bool registerRules(const std::string & name, const std::string & rules, bool reverse, std::string & error);
    //  The transform of a name (registered, or ICU's own), or of rules, with a
    //  filter (ranges of code points; none: all), or nullptr and the reason.
    static std::unique_ptr<ICUTransform> fromName(const std::string & name, const std::vector<std::pair<uint32_t, uint32_t>> * filter,
                                                  std::string & error);
    static std::unique_ptr<ICUTransform> fromRules(const std::string & name, const std::string & rules, bool reverse,
                                                   const std::vector<std::pair<uint32_t, uint32_t>> * filter, std::string & error);
    ~ICUTransform();
    //  The text (UTF-8) transformed.
    std::string apply(const std::string & text) const;
    //  The text transformed between the texts before and after it, which the
    //  transform sees as contexts (as ICU transforms a run of a filtered text,
    //  in the text of the run's surroundings) but leaves.
    std::string apply(const std::string & before, const std::string & text, const std::string & after) const;
private:
    struct Impl;
    explicit ICUTransform(Impl * impl);
    std::unique_ptr<Impl> mImpl;
};

}

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include "icu_transform.h"

#ifdef TCONV_WITH_ICU
#include <unicode/translit.h>
#include <unicode/uniset.h>
#include <unicode/unistr.h>
#include <unicode/parseerr.h>
#include <regex>
#endif

namespace tconv {

#ifdef TCONV_WITH_ICU

struct ICUTransform::Impl {
    std::unique_ptr<icu::Transliterator> t;
};

ICUTransform::ICUTransform(Impl * impl) : mImpl(impl) {}

bool ICUTransform::available() {
    return true;
}

//  ICU does not invert ::Null ; (U_INVALID_ID in reverse); ::Null (Null) ; is
//  the same transform in both directions.
static std::string invertibleNull(const std::string & rules) {
    static const std::regex null(R"((^|\n)(\s*)::\s*[Nn]ull\s*;)");
    return std::regex_replace(rules, null, "$1$2::Null (Null);");
}

static std::unique_ptr<icu::Transliterator> make(const std::string & name, const std::string & rules, bool reverse, std::string & error) {
    UErrorCode status = U_ZERO_ERROR;
    UParseError pe;
    const icu::UnicodeString id = icu::UnicodeString::fromUTF8(name);
    std::unique_ptr<icu::Transliterator> t(icu::Transliterator::createFromRules(
        id, icu::UnicodeString::fromUTF8(invertibleNull(rules)), reverse ? UTRANS_REVERSE : UTRANS_FORWARD, pe, status));
    if (U_FAILURE(status) || !t) {
        error = std::string("ICU cannot make ") + name + " from its rules: " + u_errorName(status) + " (line " + std::to_string(pe.line) + ")";
        return nullptr;
    }
    return t;
}

static void addFilter(icu::Transliterator & t, const std::vector<std::pair<uint32_t, uint32_t>> * filter) {
    if (filter == nullptr) return;
    icu::UnicodeSet * set = new icu::UnicodeSet();
    for (const auto & r : *filter) set->add(static_cast<UChar32>(r.first), static_cast<UChar32>(r.second));
    set->freeze();
    t.adoptFilter(set);
}

bool ICUTransform::registerRules(const std::string & name, const std::string & rules, bool reverse, std::string & error) {
    std::unique_ptr<icu::Transliterator> t = make(name, rules, reverse, error);
    if (!t) return false;
    icu::Transliterator::registerInstance(t.release());
    return true;
}

std::unique_ptr<ICUTransform> ICUTransform::fromName(const std::string & name, const std::vector<std::pair<uint32_t, uint32_t>> * filter,
                                                     std::string & error) {
    UErrorCode status = U_ZERO_ERROR;
    UParseError pe;
    std::unique_ptr<icu::Transliterator> t(icu::Transliterator::createInstance(icu::UnicodeString::fromUTF8(name), UTRANS_FORWARD, pe, status));
    if (U_FAILURE(status) || !t) {
        error = std::string("ICU has no transform ") + name + " (" + u_errorName(status) + ")";
        return nullptr;
    }
    addFilter(*t, filter);
    return std::unique_ptr<ICUTransform>(new ICUTransform(new Impl{std::move(t)}));
}

std::unique_ptr<ICUTransform> ICUTransform::fromRules(const std::string & name, const std::string & rules, bool reverse,
                                                      const std::vector<std::pair<uint32_t, uint32_t>> * filter, std::string & error) {
    std::unique_ptr<icu::Transliterator> t = make(name, rules, reverse, error);
    if (!t) return nullptr;
    addFilter(*t, filter);
    return std::unique_ptr<ICUTransform>(new ICUTransform(new Impl{std::move(t)}));
}

ICUTransform::~ICUTransform() = default;

std::string ICUTransform::apply(const std::string & text) const {
    icu::UnicodeString u = icu::UnicodeString::fromUTF8(text);
    mImpl->t->transliterate(u);
    std::string out;
    u.toUTF8String(out);
    return out;
}

std::string ICUTransform::apply(const std::string & before, const std::string & text, const std::string & after) const {
    icu::UnicodeString u = icu::UnicodeString::fromUTF8(before + text + after);
    const int32_t start = icu::UnicodeString::fromUTF8(before).length();
    const int32_t following = icu::UnicodeString::fromUTF8(after).length();
    UTransPosition position{0, u.length(), start, u.length() - following};
    mImpl->t->finishTransliteration(u, position);
    std::string out;
    u.tempSubString(start, u.length() - following - start).toUTF8String(out);
    return out;
}

#else

struct ICUTransform::Impl {};

ICUTransform::ICUTransform(Impl * impl) : mImpl(impl) {}

bool ICUTransform::available() {
    return false;
}

bool ICUTransform::registerRules(const std::string &, const std::string &, bool, std::string & error) {
    error = "tconv was built without ICU";
    return false;
}

std::unique_ptr<ICUTransform> ICUTransform::fromName(const std::string &, const std::vector<std::pair<uint32_t, uint32_t>> *,
                                                     std::string & error) {
    error = "tconv was built without ICU";
    return nullptr;
}

std::unique_ptr<ICUTransform> ICUTransform::fromRules(const std::string &, const std::string &, bool,
                                                      const std::vector<std::pair<uint32_t, uint32_t>> *, std::string & error) {
    error = "tconv was built without ICU";
    return nullptr;
}

ICUTransform::~ICUTransform() = default;

std::string ICUTransform::apply(const std::string & text) const {
    return text;
}

std::string ICUTransform::apply(const std::string &, const std::string & text, const std::string &) const {
    return text;
}

#endif

}

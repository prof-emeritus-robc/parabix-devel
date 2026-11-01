#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: ldml

parabix_add_executable(
NAME
    ldml_trules
SRC
    ldml_trules.cpp
DEPS
    ldml.transforms
    re.analysis
    re.transforms
    re.unicode
)

parabix_add_executable(
NAME
    tconv
SRC
    tconv.cpp
    icu_transform.cpp
DEPS
    ldml.transforms
    pablo.compiler
    pablo.bixnum
    kernel.basis
    kernel.bitwise
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.unicode
    kernel.unicode.normalization
    kernel.util
    re.cc
    re.unicode
    kernel.re
    toolchain
)

# The ICU implementation of transforms, for tconv --use-ICU (if ICU is found;
# -DICU_ROOT=<prefix> to give its location; Homebrew's icu4c is looked for).
if(NOT ICU_ROOT)
    file(GLOB TCONV_ICU_CANDIDATES /opt/homebrew/opt/icu4c* /usr/local/opt/icu4c*)
    if(TCONV_ICU_CANDIDATES)
        list(SORT TCONV_ICU_CANDIDATES)
        list(REVERSE TCONV_ICU_CANDIDATES)
        list(GET TCONV_ICU_CANDIDATES 0 TCONV_ICU_ROOT)
        set(ICU_ROOT ${TCONV_ICU_ROOT})
    endif()
endif()
find_package(ICU COMPONENTS uc i18n data)
if(ICU_FOUND)
    message(STATUS "tconv: ICU ${ICU_VERSION} (${ICU_INCLUDE_DIRS}) for --use-ICU")
    target_include_directories(tconv PRIVATE ${ICU_INCLUDE_DIRS})
    target_link_libraries(tconv PRIVATE ICU::i18n ICU::uc ICU::data)
    target_compile_definitions(tconv PRIVATE TCONV_WITH_ICU=1)
else()
    message(STATUS "tconv: ICU not found; --use-ICU is not available")
endif()

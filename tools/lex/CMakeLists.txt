#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: lex
# Find ICU components for locale-aware word boundaries
# tells CMake to locate ICU on your system
# COMPONENTS uc i18n - specifies we need:
# uc = Unicode common library (basic Unicode functions)
# i18n = Internationalization library (includes BreakIterator)
find_package(ICU REQUIRED COMPONENTS uc i18n)

parabix_add_executable(
NAME
    tokenizer
SRC
    tokenizer.cpp
    normalize.cpp
    pretokenizer.cpp
    ICU_Boundaries.cpp
    bpe.cpp
DEPS
    pablo.compiler
    kernel.basis
    kernel.io
    kernel.unicode
    kernel.pipeline
    kernel.streamutils
    kernel.scan
    kernel.bitwise
    kernel.util
    toolchain
    grep
    re.cc
    re.parse
)

# Link ICU libraries to tokenizer
# Link the required ICU components to the tokenizer executable
target_link_libraries(tokenizer PRIVATE ICU::uc ICU::i18n)
target_include_directories(tokenizer PRIVATE ${ICU_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
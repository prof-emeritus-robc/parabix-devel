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
    re.cc
    re.unicode
    kernel.re
    toolchain
)

#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: kernel.unicode

parabix_add_module(
NAME
    kernel.unicode
SRC
    charclasses.cpp
    char_replacement.cpp
    boundary_kernels.cpp
    UCD_property_kernel.cpp
    utf8gen.cpp
    utf8_decoder.cpp
    utf8_support.cpp
    utf16gen.cpp
DEPS
    kernel.bitwise
    kernel.pipeline
    kernel.streamutils
    re.unicode
    pablo.compiler
    pablo.bixnum
)

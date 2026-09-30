#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: re.cc

parabix_add_module(
NAME
    re.cc
SRC
    cc_compiler_target.cpp
    cc_compiler.cpp
    cc_kernel.cpp
    GB_18030_data.cpp
DEPS
    pablo.compiler
    re.adt
    unicode.core
)

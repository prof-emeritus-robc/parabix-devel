#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: re.adt

parabix_add_module(
NAME
    re.adt
SRC
    re_assertion.cpp
    re_cc.cpp
    re_diff.cpp
    re_empty_set.cpp
    re_intersect.cpp
    re_range.cpp
    re_re.cpp
    re_rep.cpp
    memoization.cpp
    re_utility.cpp
    alphabet/alphabet.cpp
    alphabet/multiplex_CCs.cpp
DEPS
    re.toolchain
    unicode.core
    slaballocator
)

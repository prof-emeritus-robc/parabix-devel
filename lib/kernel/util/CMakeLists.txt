#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: kernel.util

parabix_add_module(
NAME
    kernel.util
SRC
    bixhash.cpp
    error_monitor_kernel.cpp
    hex_convert.cpp
    linebreak_kernel.cpp
    nesting.cpp
    radix64.cpp
    random_stream.cpp
DEPS
    pablo.compiler
    pablo.bixnum
    re.cc
    kernel.pipeline
    kernel.basis
    kernel.streamutils
)

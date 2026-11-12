#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: testing

parabix_add_module(
NAME
    testing
EXCLUDE_FROM_SINGLE
SRC
    assert.cpp
    runtime.cpp
DEPS
    kernel.io
    kernel.core
    kernel.pipeline
)

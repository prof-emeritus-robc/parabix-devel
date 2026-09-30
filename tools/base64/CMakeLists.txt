#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: base64

parabix_add_executable(
NAME
    base64
SRC
    base64.cpp
DEPS
    kernel.io
    kernel.pipeline
    kernel.util
)

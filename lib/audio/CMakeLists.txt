#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: audio

parabix_add_module(
NAME
    audio
SRC
    audio.cpp
    stream_manipulation.cpp
DEPS
    pablo.compiler
    kernel.io
    kernel.pipeline
    kernel.streamutils
    util
    toolchain
)
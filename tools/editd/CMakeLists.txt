#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: editd

parabix_add_executable(
NAME
    editd
SRC
    editd_cpu_kernel.cpp
    editd_gpu_kernel.cpp
    editd.cpp
    editdscan_kernel.cpp
    pattern_compiler.cpp
DEPS
    pablo.compiler
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    re.cc
)

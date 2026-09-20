#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: idisa_exerciser

set(CMAKE_C_STANDARD 20)
set(CMAKE_CXX_STANDARD 20)

parabix_add_executable(
NAME
    idisa_exerciser
SRC
    idisa_exerciser.cpp
    idisa_operations.cpp
    safe_ops.cpp
DEPS
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.util
)

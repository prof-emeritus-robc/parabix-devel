#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: objcache

add_custom_target(recompile-object-cache ALL touch  ${CMAKE_CURRENT_SOURCE_DIR}/object_cache.cpp)

parabix_add_module(
NAME
    objcache
SRC
    object_cache.cpp
DEPS
    kernel.core
    toolchain
)

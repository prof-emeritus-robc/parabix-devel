#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: objcache

parabix_add_module(
NAME
    objcache
SRC
    build_identity.cpp
    object_cache.cpp
DEPS
    kernel.core
    toolchain
)

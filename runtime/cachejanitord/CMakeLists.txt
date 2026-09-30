#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# runtime: cachejanitord
# Background daemon that prunes the on-disk object cache maintained by the
# objcache library (lib/objcache). Not a user-facing tool, so it lives under
# runtime/ rather than tools/, and is built unconditionally alongside the
# framework libraries rather than gated behind GENERATE_TOOLS.

add_executable(cachejanitord
    object_cache_daemon.cpp)

target_link_libraries(cachejanitord ${Boost_LIBRARIES})

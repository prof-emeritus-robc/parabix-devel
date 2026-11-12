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

# Where the object cache finds it, as in an installation (see the top-level
# CMakeLists.txt): <build>/libexec/parabix by default.
set_target_properties(cachejanitord PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PARABIX_BUILD_LIBEXECDIR})

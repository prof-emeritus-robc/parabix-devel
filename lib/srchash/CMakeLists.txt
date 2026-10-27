#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: srchash
#
# Every other target links this module (see cmake/ParabixSourceHash.cmake),
# so it must depend on nothing else in Parabix.

parabix_add_module(
NAME
    srchash
SRC
    source_hash.cpp
NO_SOURCE_HASH
)

if(NOT ARTIFACT_MODE STREQUAL "Single")
    target_link_libraries(srchash PRIVATE ${CMAKE_DL_LIBS})
endif()

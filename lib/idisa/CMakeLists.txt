#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: idisa

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64") 
parabix_add_module(
NAME
    idisa
SRC
    CBuilder.cpp
    idisa_builder.cpp
    idisa_i64_builder.cpp
    idisa_arm_builder.cpp
DEPS
    toolchain
    ${Boost_LIBRARIES})
else()
parabix_add_module(
NAME
    idisa
SRC
    CBuilder.cpp
    idisa_avx_builder.cpp
    idisa_builder.cpp
    idisa_i64_builder.cpp
    # idisa_nvptx_builder.cpp
    idisa_sse_builder.cpp
DEPS
    toolchain
    ${Boost_LIBRARIES})
endif()

if(ARTIFACT_MODE STREQUAL "Single")
    add_dependencies(${PARABIX_MONO_TARGET} recompile-object-cache)
else()
    add_dependencies(idisa recompile-object-cache)
endif()

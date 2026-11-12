#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# Configure-time check (try_compile, from the top-level CMakeLists.txt):
# can the static Boost libraries be linked into a shared library?  Not when
# they were built without -fPIC, as the x86-64 archives of Debian and Ubuntu
# are ("relocation R_X86_64_PC32 ... can not be used when making a shared
# object").  All of Boost is built alike, so filesystem stands for the rest.

cmake_minimum_required(VERSION 3.12)
project(boost_static_pic_check CXX)
set(CMAKE_CXX_STANDARD 17)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS filesystem)
add_library(check SHARED check.cpp)
target_include_directories(check PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(check PRIVATE ${Boost_LIBRARIES})

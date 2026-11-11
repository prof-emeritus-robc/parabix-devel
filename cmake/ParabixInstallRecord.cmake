#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# Records the files an installation component installs, for uninstalling
# (cmake/ParabixUninstall.cmake.in).  Included by the install scripts
# (install(CODE), see cmake/ParabixInstall.cmake) before and after the
# rules of each component, with
#   PARABIX_RECORD_PHASE      START or END
#   PARABIX_RECORD_COMPONENT  the component (runtime, tool-<tool>)
#   PARABIX_RECORD_DIR        the record directory, relative to the prefix
#
# At the end, the files of the component (those added to the install
# manifest since the start) are written, relative to the prefix, to
# <prefix>/<PARABIX_RECORD_DIR>/<component>.txt.  The files of an earlier
# installation of the component that are no longer installed (e.g., a
# library of an earlier version) are removed.

# The install scripts set no policies (e.g., IN_LIST needs CMP0057); this
# file has its own policy scope.
cmake_policy(VERSION 3.14)

string(MAKE_C_IDENTIFIER "${PARABIX_RECORD_COMPONENT}" _PX_ID)

if(PARABIX_RECORD_PHASE STREQUAL "START")
    list(LENGTH CMAKE_INSTALL_MANIFEST_FILES _PX_START_${_PX_ID})
    return()
endif()

set(_PX_RECORD "${CMAKE_INSTALL_PREFIX}/${PARABIX_RECORD_DIR}/${PARABIX_RECORD_COMPONENT}.txt")
list(LENGTH CMAKE_INSTALL_MANIFEST_FILES _PX_END)
math(EXPR _PX_COUNT "${_PX_END} - ${_PX_START_${_PX_ID}}")
list(SUBLIST CMAKE_INSTALL_MANIFEST_FILES ${_PX_START_${_PX_ID}} ${_PX_COUNT} _PX_FILES)
list(APPEND _PX_FILES "${_PX_RECORD}")

set(_PX_RELATIVE "")
foreach(_PX_FILE ${_PX_FILES})
    file(RELATIVE_PATH _PX_REL "${CMAKE_INSTALL_PREFIX}" "${_PX_FILE}")
    list(APPEND _PX_RELATIVE "${_PX_REL}")
endforeach()

# What an earlier installation installed and this one did not
if(EXISTS "$ENV{DESTDIR}${_PX_RECORD}")
    file(STRINGS "$ENV{DESTDIR}${_PX_RECORD}" _PX_OLD)
    foreach(_PX_REL ${_PX_OLD})
        if(NOT _PX_REL IN_LIST _PX_RELATIVE)
            set(_PX_STALE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${_PX_REL}")
            if(EXISTS "${_PX_STALE}" OR IS_SYMLINK "${_PX_STALE}")
                message(STATUS "Removing (no longer installed): ${_PX_STALE}")
                file(REMOVE "${_PX_STALE}")
            endif()
        endif()
    endforeach()
endif()

string(REPLACE ";" "\n" _PX_CONTENT "${_PX_RELATIVE}")
message(STATUS "Installing: $ENV{DESTDIR}${_PX_RECORD}")
file(WRITE "$ENV{DESTDIR}${_PX_RECORD}" "${_PX_CONTENT}\n")
list(APPEND CMAKE_INSTALL_MANIFEST_FILES "${_PX_RECORD}")

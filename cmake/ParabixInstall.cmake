#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# Installation of the Parabix tools, from an ARTIFACT_MODE=Single build.
#
# Configure:
#   cmake -DCMAKE_BUILD_TYPE=Release -DARTIFACT_MODE=Single \
#         [-DPARABIX_INSTALL_SCOPE=User|System] [-DCMAKE_INSTALL_PREFIX=<prefix>] \
#         [-DPARABIX_INSTALL_TOOLS="default;dev;<tool>..."] [-DPARABIX_BUILD_TESTS=OFF] ..
#   PARABIX_INSTALL_SCOPE User (default prefix ~/.local) or System (/usr/local).
#   PARABIX_INSTALL_TOOLS the tools `make install` installs: the sets default,
#       dev and all (both), and tool names (see the lists below).
#
# Install:
#   make install                 builds everything, installs the selected tools
#   make install-tools           builds and installs the selected tools only
#   make install-<tool>          builds and installs one tool (of any set)
#   DESTDIR=<staging dir> applies to all of them.
#
# Layout (GNUInstallDirs, below the prefix):
#   bin/icgrep, bin/px-<tool>        the tools (all but icgrep prefixed, so as
#                                    not to shadow system commands)
#   lib/libparabix.so.0.9            the Parabix library (.0.9.dylib on macOS)
#   libexec/parabix/cachejanitord    the object cache cleanup daemon
#   libexec/parabix/bin/<tool>       unprefixed links to some tools, for
#                                    those who put this directory on their PATH
#   share/parabix/pablo/             pablo sources read by json, xml and ztf1
#   share/doc/parabix/               READMEs and licenses
#
# System-wide object cache (PARABIX_INSTALL_SCOPE=System): the tools read
# PARABIX_SYSTEM_OBJECT_CACHE_DIR (by default /var/cache/parabix, or
# /Library/Caches/parabix on macOS) after the user's own cache, and never
# write it.  It is filled when a tool is installed, by running it
# (share/parabix/prewarm.cmake), unless DESTDIR is set: the objects are for
# the processor of the machine installing.  Uninstalling the runtime
# removes it.
#
# Components: runtime (library, daemon, documents) and tool-<tool> for each
# tool; `cmake -DCOMPONENT=tool-wc -P cmake_install.cmake` installs one.
# Each records the files it installs in share/parabix/manifests (see
# cmake/ParabixInstallRecord.cmake).
#
# Uninstall:
#   make uninstall               everything installed to the prefix
#   make uninstall-<tool>        one tool, and the runtime with the last one
#   cmake [-DTOOL=<tool>] -P <prefix>/share/parabix/uninstall.cmake
#                                the same, without the build tree

# The default set: tools for general use.
set(PARABIX_TOOLS_DEFAULT
    icgrep wc base64 csvcut csvgrep csv2json tconv ldml_trules hexlines
    gb18030 nfc nfd nfkd u32u16 u32u8 u8u16 x8u16)
# The dev set: developer tools and demonstrations.
set(PARABIX_TOOLS_DEV
    ucount tokenizer xch editd idisa_exerciser idisa_test hashdemo matchprior
    ufilter audioamplifier discontinuity stereo2mono ztf-runs ztf-hash)
# Tools reading pablo sources at run time: installed for a user only (not
# for all users, who would all depend on files that one may change), and
# only by name, in no set.
set(PARABIX_TOOLS_USER_ONLY json xml ztf1)
# Tools that also get an unprefixed link in libexec/parabix/bin.
set(PARABIX_TOOLS_LINKED wc base64 csvgrep csvcut csv2json tconv hexlines)

if(NOT ARTIFACT_MODE STREQUAL "Single")
    install(CODE "message(FATAL_ERROR \"Parabix is installed from a build with -DARTIFACT_MODE=Single (see cmake/ParabixInstall.cmake)\")")
    return()
endif()

# Every tool is in a list, so that each new one is placed deliberately.
get_property(BUILT_TOOLS GLOBAL PROPERTY PARABIX_TOOLS)
foreach(TOOL ${BUILT_TOOLS})
    if(NOT TOOL IN_LIST PARABIX_TOOLS_DEFAULT AND NOT TOOL IN_LIST PARABIX_TOOLS_DEV
       AND NOT TOOL IN_LIST PARABIX_TOOLS_USER_ONLY)
        message(WARNING "Tool ${TOOL} is in no list of cmake/ParabixInstall.cmake, so it cannot be installed")
    endif()
endforeach()

if(PARABIX_INSTALL_SCOPE STREQUAL "System")
    string(FIND "${CMAKE_INSTALL_PREFIX}/" "$ENV{HOME}/" PREFIX_IN_HOME)
    if(PREFIX_IN_HOME EQUAL 0)
        message(WARNING "PARABIX_INSTALL_SCOPE=System with a prefix in your home directory: ${CMAKE_INSTALL_PREFIX}")
    endif()
endif()

# The tools selected by PARABIX_INSTALL_TOOLS
set(SELECTED_TOOLS "")
foreach(ITEM ${PARABIX_INSTALL_TOOLS})
    if(ITEM STREQUAL "default")
        list(APPEND SELECTED_TOOLS ${PARABIX_TOOLS_DEFAULT})
    elseif(ITEM STREQUAL "dev")
        list(APPEND SELECTED_TOOLS ${PARABIX_TOOLS_DEV})
    elseif(ITEM STREQUAL "all")
        list(APPEND SELECTED_TOOLS ${PARABIX_TOOLS_DEFAULT} ${PARABIX_TOOLS_DEV})
    elseif(ITEM IN_LIST PARABIX_TOOLS_DEFAULT OR ITEM IN_LIST PARABIX_TOOLS_DEV)
        list(APPEND SELECTED_TOOLS ${ITEM})
    elseif(ITEM IN_LIST PARABIX_TOOLS_USER_ONLY)
        if(PARABIX_INSTALL_SCOPE STREQUAL "System")
            message(FATAL_ERROR "PARABIX_INSTALL_TOOLS: ${ITEM} reads pablo sources at run time, so it is installed only with PARABIX_INSTALL_SCOPE=User")
        endif()
        list(APPEND SELECTED_TOOLS ${ITEM})
    else()
        message(FATAL_ERROR "PARABIX_INSTALL_TOOLS: ${ITEM} is not a tool or a set of tools (default, dev, all)")
    endif()
endforeach()
list(REMOVE_DUPLICATES SELECTED_TOOLS)

# The tool name as installed
function(parabix_install_name OUT TOOL)
    if(TOOL STREQUAL "icgrep")
        set(${OUT} ${TOOL} PARENT_SCOPE)
    else()
        set(${OUT} px-${TOOL} PARENT_SCOPE)
    endif()
endfunction(parabix_install_name)

# The records of installed files, and the uninstall script
set(PARABIX_RECORD_DIR ${CMAKE_INSTALL_DATADIR}/parabix/manifests)
set(PARABIX_UNINSTALL_SCRIPT ${CMAKE_INSTALL_DATADIR}/parabix/uninstall.cmake)
file(RELATIVE_PATH PARABIX_UNINSTALL_SCRIPT_TO_PREFIX ${CMAKE_INSTALL_FULL_DATADIR}/parabix ${CMAKE_INSTALL_PREFIX})
# The directories of Parabix, removed once empty (most specific first)
set(PARABIX_OWNED_DIRS
    ${CMAKE_INSTALL_LIBEXECDIR}/parabix/bin
    ${CMAKE_INSTALL_LIBEXECDIR}/parabix
    ${CMAKE_INSTALL_DATADIR}/parabix/pablo
    ${PARABIX_RECORD_DIR}
    ${CMAKE_INSTALL_DATADIR}/parabix
    ${CMAKE_INSTALL_DOCDIR})
set(PARABIX_PREWARM_SCRIPT ${CMAKE_INSTALL_DATADIR}/parabix/prewarm.cmake)
configure_file(${CMAKE_CURRENT_LIST_DIR}/ParabixUninstall.cmake.in ${PROJECT_BINARY_DIR}/ParabixUninstall.cmake @ONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/ParabixPrewarm.cmake.in ${PROJECT_BINARY_DIR}/ParabixPrewarm.cmake @ONLY)

# Records the files of a component as it is installed: PHASE START before
# its rules, END after them.
function(parabix_install_record COMPONENT PHASE)
    install(CODE "
        set(PARABIX_RECORD_PHASE ${PHASE})
        set(PARABIX_RECORD_COMPONENT ${COMPONENT})
        set(PARABIX_RECORD_DIR \"${PARABIX_RECORD_DIR}\")
        include(\"${PROJECT_SOURCE_DIR}/cmake/ParabixInstallRecord.cmake\")"
        COMPONENT ${COMPONENT} ${ARGN})
endfunction(parabix_install_record)

###   Component runtime   ###

parabix_install_record(runtime START)

if(TARGET ${PARABIX_MONO_TARGET} AND NOT ARTIFACT_TYPE STREQUAL "Static")
    install(TARGETS ${PARABIX_MONO_TARGET}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP
        COMPONENT runtime)
endif()
install(TARGETS cachejanitord
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/parabix
    COMPONENT runtime)
install(FILES
        ${PROJECT_SOURCE_DIR}/README.md
        ${PROJECT_SOURCE_DIR}/README-icgrep.md
        ${PROJECT_SOURCE_DIR}/OSL3.0.txt
        ${PROJECT_SOURCE_DIR}/LLVM-LICENSE.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT runtime)
install(FILES ${PROJECT_BINARY_DIR}/ParabixUninstall.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/parabix
    RENAME uninstall.cmake
    COMPONENT runtime)
if(PARABIX_SYSTEM_CACHE)
    install(FILES ${PROJECT_BINARY_DIR}/ParabixPrewarm.cmake
        DESTINATION ${CMAKE_INSTALL_DATADIR}/parabix
        RENAME prewarm.cmake
        COMPONENT runtime)
endif()
parabix_install_record(runtime END)

# make uninstall: everything installed to the prefix
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -DPREFIX=${CMAKE_INSTALL_PREFIX} -P ${PROJECT_BINARY_DIR}/ParabixUninstall.cmake
    COMMENT "Uninstalling Parabix from ${CMAKE_INSTALL_PREFIX}"
    VERBATIM)

###   Components tool-<tool>   ###

# From libexec/parabix/bin to bin, for the unprefixed links
file(RELATIVE_PATH LINKS_TO_BINDIR ${CMAKE_INSTALL_FULL_LIBEXECDIR}/parabix/bin ${CMAKE_INSTALL_FULL_BINDIR})

set(INSTALLED_TOOLS "")
foreach(TOOL ${PARABIX_TOOLS_DEFAULT} ${PARABIX_TOOLS_DEV} ${PARABIX_TOOLS_USER_ONLY})
    if(NOT TARGET ${TOOL})
        if(TOOL IN_LIST SELECTED_TOOLS)
            message(STATUS "Tool ${TOOL} is selected for installation but is not built")
        endif()
        continue()
    endif()
    if(TOOL IN_LIST PARABIX_TOOLS_USER_ONLY AND PARABIX_INSTALL_SCOPE STREQUAL "System")
        continue()
    endif()
    # Only make install-<tool> installs the tools not selected.
    if(TOOL IN_LIST SELECTED_TOOLS)
        set(EXCLUDE "")
        list(APPEND INSTALLED_TOOLS ${TOOL})
    else()
        set(EXCLUDE EXCLUDE_FROM_ALL)
    endif()
    parabix_install_name(INSTALL_NAME ${TOOL})
    parabix_install_record(tool-${TOOL} START ${EXCLUDE})
    # Copied as built: the build tree has the installed run paths.
    install(PROGRAMS $<TARGET_FILE:${TOOL}>
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME ${INSTALL_NAME}
        COMPONENT tool-${TOOL} ${EXCLUDE})
    get_property(PABLO_SOURCES GLOBAL PROPERTY PARABIX_PABLO_SOURCES_${TOOL})
    if(PABLO_SOURCES)
        install(FILES ${PABLO_SOURCES}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/parabix/pablo
            COMPONENT tool-${TOOL} ${EXCLUDE})
    endif()
    if(TOOL IN_LIST PARABIX_TOOLS_LINKED)
        # Relative, so that the installation can be moved.  Listed in the
        # install manifest, for uninstalling.
        install(CODE "
            set(INSTALLED \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBEXECDIR}/parabix/bin/${TOOL}\")
            set(LINK \"\$ENV{DESTDIR}\${INSTALLED}\")
            message(STATUS \"Installing: \${LINK} -> ${LINKS_TO_BINDIR}/${INSTALL_NAME}\")
            get_filename_component(LINK_DIR \"\${LINK}\" DIRECTORY)
            file(MAKE_DIRECTORY \"\${LINK_DIR}\")
            file(CREATE_LINK \"${LINKS_TO_BINDIR}/${INSTALL_NAME}\" \"\${LINK}\" SYMBOLIC)
            # As the manifest lists installed files: without DESTDIR
            list(APPEND CMAKE_INSTALL_MANIFEST_FILES \"\${INSTALLED}\")"
            COMPONENT tool-${TOOL} ${EXCLUDE})
    endif()
    parabix_install_record(tool-${TOOL} END ${EXCLUDE})
    if(PARABIX_SYSTEM_CACHE)
        # Prewarm the system-wide object cache with the tool (see above)
        install(CODE "
            if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
                execute_process(COMMAND \${CMAKE_COMMAND} -DTOOL=${TOOL}
                    -P \"\${CMAKE_INSTALL_PREFIX}/${PARABIX_PREWARM_SCRIPT}\")
            else()
                message(STATUS \"Not prewarming the system-wide object cache (DESTDIR is set): ${TOOL}\")
            endif()"
            COMPONENT tool-${TOOL} ${EXCLUDE})
    endif()
    # make install-<tool>: build and install the tool (with the runtime)
    add_custom_target(install-${TOOL}
        COMMAND ${CMAKE_COMMAND} -DCOMPONENT=runtime -P ${PROJECT_BINARY_DIR}/cmake_install.cmake
        COMMAND ${CMAKE_COMMAND} -DCOMPONENT=tool-${TOOL} -P ${PROJECT_BINARY_DIR}/cmake_install.cmake
        COMMENT "Installing ${INSTALL_NAME}"
        VERBATIM)
    add_dependencies(install-${TOOL} ${TOOL} ${PARABIX_MONO_TARGET} cachejanitord)
    # make uninstall-<tool>
    add_custom_target(uninstall-${TOOL}
        COMMAND ${CMAKE_COMMAND} -DPREFIX=${CMAKE_INSTALL_PREFIX} -DTOOL=${TOOL} -P ${PROJECT_BINARY_DIR}/ParabixUninstall.cmake
        COMMENT "Uninstalling ${INSTALL_NAME}"
        VERBATIM)
endforeach()

# make install-tools: build and install the selected tools only
set(INSTALL_COMMANDS COMMAND ${CMAKE_COMMAND} -DCOMPONENT=runtime -P ${PROJECT_BINARY_DIR}/cmake_install.cmake)
foreach(TOOL ${INSTALLED_TOOLS})
    list(APPEND INSTALL_COMMANDS COMMAND ${CMAKE_COMMAND} -DCOMPONENT=tool-${TOOL} -P ${PROJECT_BINARY_DIR}/cmake_install.cmake)
endforeach()
add_custom_target(install-tools ${INSTALL_COMMANDS}
    COMMENT "Installing the selected tools: ${INSTALLED_TOOLS}"
    VERBATIM)
add_dependencies(install-tools ${INSTALLED_TOOLS} ${PARABIX_MONO_TARGET} cachejanitord)

string(REPLACE ";" " " INSTALLED_LIST "${INSTALLED_TOOLS}")
message(STATUS "Install (${PARABIX_INSTALL_SCOPE}) to ${CMAKE_INSTALL_PREFIX}: ${INSTALLED_LIST}")
if(PARABIX_SYSTEM_CACHE)
    message(STATUS "System-wide object cache: ${PARABIX_SYSTEM_CACHE}")
endif()

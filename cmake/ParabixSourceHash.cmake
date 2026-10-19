#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# Source hashes of Parabix modules and executables.
#
# Every library module and executable gets a generated source file,
# <target>_srchash.cpp, holding a SHA-256 over the sources and headers of that
# target and of every Parabix module it depends on (transitively), plus a
# configuration salt (LLVM, compiler, build type and flags).  The hash changes
# exactly when the source of some module the target depends on changes, so it
# can key cached kernels at module granularity.
#
# Usage:
#   parabix_srchash_add_inputs(<name> SRC <files...> DEPS <deps...> [INCLUDE_DIR <dir>])
#       Records the inputs and direct dependencies of a module or executable.
#       Called for every module, including in ARTIFACT_MODE=Single, where the
#       module is not a target of its own.
#   parabix_srchash_attach(<target>)
#       Adds the generated <target>_srchash.cpp to <target>, which registers the
#       hash with lib/srchash when the image holding <target> is loaded.  Must
#       be called in the directory that defines <target>.
#   parabix_srchash_finalize()
#       Computes the dependency closures.  Called once, at the end of the
#       top-level CMakeLists.txt, after every target has been defined.

set(PARABIX_SRCHASH_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/ComputeSourceHash.cmake)
set(PARABIX_SRCHASH_DIR ${CMAKE_BINARY_DIR}/srchash)
set(PARABIX_SRCHASH_HEADER_GLOBS *.h *.hh *.hpp *.hxx *.inc *.def *.tcc)

# Lists the headers below DIR, leaving out subdirectories that hold a module of
# their own (a CMakeLists.txt in the corresponding directory of SRC_DIR).
function(parabix_srchash_glob_headers OUT DIR SRC_DIR)
    set(GLOBS "")
    foreach(PATTERN ${PARABIX_SRCHASH_HEADER_GLOBS})
        list(APPEND GLOBS "${DIR}/${PATTERN}")
    endforeach()
    file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${GLOBS})
    file(GLOB_RECURSE NESTED LIST_DIRECTORIES false "${SRC_DIR}/*/CMakeLists.txt")
    foreach(NESTED_LIST ${NESTED})
        get_filename_component(NESTED_DIR ${NESTED_LIST} DIRECTORY)
        file(RELATIVE_PATH NESTED_REL ${SRC_DIR} ${NESTED_DIR})
        string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" NESTED_RE "${DIR}/${NESTED_REL}/")
        list(FILTER HEADERS EXCLUDE REGEX "^${NESTED_RE}")
    endforeach()
    set(${OUT} ${HEADERS} PARENT_SCOPE)
endfunction(parabix_srchash_glob_headers)

function(parabix_srchash_add_inputs NAME)
    cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SRC;DEPS" ${ARGN})
    set(INPUTS "")
    foreach(SRC ${ARG_SRC})
        get_filename_component(ABS ${SRC} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND INPUTS ${ABS})
    endforeach()
    parabix_srchash_glob_headers(PRIVATE_HEADERS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    list(APPEND INPUTS ${PRIVATE_HEADERS})
    if(ARG_INCLUDE_DIR AND IS_DIRECTORY ${ARG_INCLUDE_DIR})
        parabix_srchash_glob_headers(PUBLIC_HEADERS ${ARG_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND INPUTS ${PUBLIC_HEADERS})
    endif()
    list(REMOVE_DUPLICATES INPUTS)
    set_property(GLOBAL APPEND PROPERTY PARABIX_SRCHASH_NAMES ${NAME})
    set_property(GLOBAL PROPERTY PARABIX_SRCHASH_INPUTS_${NAME} ${INPUTS})
    set_property(GLOBAL PROPERTY PARABIX_SRCHASH_DEPS_${NAME} ${ARG_DEPS})
endfunction(parabix_srchash_add_inputs)

function(parabix_srchash_attach TARGET)
    string(MAKE_C_IDENTIFIER ${TARGET} ID)
    set(LIST_FILE ${PARABIX_SRCHASH_DIR}/${ID}.inputs)
    set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/${ID}_srchash.cpp)
    set(STAMP ${CMAKE_CURRENT_BINARY_DIR}/${ID}_srchash.stamp)
    # The closure is only known once every target is defined, so the
    # dependencies come from a property that parabix_srchash_finalize sets.
    # The generated file is rewritten only when the hash changes, so an
    # unchanged hash causes no recompile or relink.
    add_custom_command(
        OUTPUT ${STAMP}
        BYPRODUCTS ${GENERATED}
        COMMAND ${CMAKE_COMMAND}
            -DTARGET=${TARGET}
            -DINPUTS_FILE=${LIST_FILE}
            -DOUTPUT=${GENERATED}
            -DSTAMP=${STAMP}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_BINARY_DIR}
            -P ${PARABIX_SRCHASH_SCRIPT}
        DEPENDS ${PARABIX_SRCHASH_SCRIPT} ${LIST_FILE} "$<TARGET_PROPERTY:${TARGET},PARABIX_SRCHASH_CLOSURE>"
        COMMENT "Computing source hash of ${TARGET}"
        VERBATIM)
    target_sources(${TARGET} PRIVATE ${GENERATED} ${STAMP})
    if(NOT ARTIFACT_MODE STREQUAL "Single")
        # The generated file registers the hash with lib/srchash.  In Single
        # mode that is part of the single artifact.
        target_link_libraries(${TARGET} PRIVATE srchash)
    endif()
    set_property(GLOBAL APPEND PROPERTY PARABIX_SRCHASH_TARGETS ${TARGET})
endfunction(parabix_srchash_attach)

# Writes CONTENT to FILE unless FILE already holds it, keeping its timestamp.
function(parabix_srchash_write_if_different FILE CONTENT)
    if(EXISTS ${FILE})
        file(READ ${FILE} OLD)
        if(OLD STREQUAL CONTENT)
            return()
        endif()
    endif()
    file(WRITE ${FILE} "${CONTENT}")
endfunction(parabix_srchash_write_if_different)

# Settings outside the source tree that change the code a module generates.
function(parabix_srchash_write_config FILE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE)
    # PARABIX_VERSION names the git commit: it says nothing about the source
    # of any particular module.
    string(REGEX REPLACE "-DPARABIX_VERSION=[^ ]*" "" FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${BUILD_TYPE}}")
    string(REGEX REPLACE " +" " " FLAGS "${FLAGS}")
    string(STRIP "${FLAGS}" FLAGS)
    set(CONTENT "")
    string(APPEND CONTENT "llvm=${LLVM_PACKAGE_VERSION}\n")
    string(APPEND CONTENT "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n")
    string(APPEND CONTENT "system=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}\n")
    string(APPEND CONTENT "boost=${Boost_VERSION}\n")
    string(APPEND CONTENT "build-type=${CMAKE_BUILD_TYPE}\n")
    string(APPEND CONTENT "flags=${FLAGS}\n")
    string(APPEND CONTENT "artifacts=${ARTIFACT_MODE} ${ARTIFACT_TYPE}\n")
    string(APPEND CONTENT "papi=${ENABLE_PAPI}\n")
    string(APPEND CONTENT "z3=${Z3_FOUND}\n")
    parabix_srchash_write_if_different(${FILE} "${CONTENT}")
endfunction(parabix_srchash_write_config)

function(parabix_srchash_finalize)
    file(MAKE_DIRECTORY ${PARABIX_SRCHASH_DIR})
    set(CONFIG_FILE ${PARABIX_SRCHASH_DIR}/config.txt)
    parabix_srchash_write_config(${CONFIG_FILE})

    get_property(NAMES GLOBAL PROPERTY PARABIX_SRCHASH_NAMES)
    get_property(TARGETS GLOBAL PROPERTY PARABIX_SRCHASH_TARGETS)
    foreach(TGT ${TARGETS})
        # Direct dependencies: those recorded with the inputs, plus whatever
        # was linked later with target_link_libraries.
        get_property(DEPS GLOBAL PROPERTY PARABIX_SRCHASH_DEPS_${TGT})
        get_target_property(LINKED ${TGT} LINK_LIBRARIES)
        if(LINKED)
            list(APPEND DEPS ${LINKED})
        endif()
        if(TGT STREQUAL PARABIX_MONO_TARGET)
            # The single artifact holds every module.
            list(APPEND DEPS ${NAMES})
        endif()
        # Depth-first closure over the Parabix modules only.
        set(CLOSURE ${TGT})
        set(PENDING ${DEPS})
        while(PENDING)
            list(POP_FRONT PENDING DEP)
            list(FIND NAMES "${DEP}" IS_MODULE)
            list(FIND CLOSURE "${DEP}" SEEN)
            if(IS_MODULE GREATER -1 AND SEEN EQUAL -1)
                list(APPEND CLOSURE ${DEP})
                get_property(MORE GLOBAL PROPERTY PARABIX_SRCHASH_DEPS_${DEP})
                if(TARGET ${DEP})
                    get_target_property(MORE_LINKED ${DEP} LINK_LIBRARIES)
                    if(MORE_LINKED)
                        list(APPEND MORE ${MORE_LINKED})
                    endif()
                endif()
                list(APPEND PENDING ${MORE})
            endif()
        endwhile()
        set(INPUTS ${CONFIG_FILE})
        foreach(MODULE ${CLOSURE})
            get_property(MODULE_INPUTS GLOBAL PROPERTY PARABIX_SRCHASH_INPUTS_${MODULE})
            list(APPEND INPUTS ${MODULE_INPUTS})
        endforeach()
        list(REMOVE_DUPLICATES INPUTS)
        list(SORT INPUTS)
        set_property(TARGET ${TGT} PROPERTY PARABIX_SRCHASH_CLOSURE ${INPUTS})
        list(SORT CLOSURE)
        string(MAKE_C_IDENTIFIER ${TGT} ID)
        string(REPLACE ";" "\n" INPUT_LINES "${INPUTS}")
        string(REPLACE ";" " " MODULE_NAMES "${CLOSURE}")
        parabix_srchash_write_if_different(${PARABIX_SRCHASH_DIR}/${ID}.inputs
            "# modules: ${MODULE_NAMES}\n${INPUT_LINES}\n")
    endforeach()
endfunction(parabix_srchash_finalize)

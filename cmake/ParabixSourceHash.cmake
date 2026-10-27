#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# Source hashes of Parabix modules and executables.
#
# Every library module and executable gets a generated source file,
# <target>_srchash.cpp, holding a SHA-256 over the sources and headers of that
# target and of every Parabix module its code generation depends on
# (transitively), plus the inputs that shape the code of every kernel (the
# pipeline driver, which optimizes and compiles each kernel module) and a
# configuration salt (LLVM, compiler, build type and flags).  The hash keys the
# cached kernels defined in the target.
#
# Code generation dependencies.  A kernel's cached object must change when
# code that runs while the kernel's IR is generated, or turned into an object,
# changes; other code need not be in its key.  Each module therefore has
# CODEGEN_DEPS, the modules whose code its kernels' generation can run.  By
# default they are its DEPS (and whatever target_link_libraries adds) less the
# modules in PARABIX_SRCHASH_NON_CODEGEN_MODULES.  A module lists CODEGEN_DEPS
# explicitly when some of its DEPS are needed only to build pipelines: kernel
# modules link other kernel modules to instantiate their kernels in helper
# pipelines, and those kernels are compiled, and cached, under their own keys.
# A dependency missing from CODEGEN_DEPS makes stale kernels look current;
# -VerifyCache detects that.
#
# Usage:
#   parabix_srchash_add_inputs(<name> SRC <files...> DEPS <deps...>
#                              [CODEGEN_DEPS <deps...>] [INCLUDE_DIR <dir>])
#       Records the inputs and direct dependencies of a module or executable.
#       Called for every module, including in ARTIFACT_MODE=Single, where the
#       module is not a target of its own.
#   parabix_srchash_add_codegen_inputs(<label> <files...>)
#       Adds files that shape the code of every kernel to every hash; <label>
#       names them in the module lists.
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

# Modules whose code never runs while a kernel's IR is generated or compiled:
# kernel.pipeline holds the pipeline builder and compiler (the driver part of
# it is added to every hash with parabix_srchash_add_codegen_inputs), objcache
# stores compiled objects and srchash identifies them.
set(PARABIX_SRCHASH_NON_CODEGEN_MODULES kernel.pipeline objcache srchash)

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
    cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SRC;DEPS;CODEGEN_DEPS" ${ARGN})
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
    # An explicit CODEGEN_DEPS, even an empty one, replaces the default.
    if("CODEGEN_DEPS" IN_LIST ARGN)
        set_property(GLOBAL PROPERTY PARABIX_SRCHASH_CODEGEN_DEPS_${NAME} ${ARG_CODEGEN_DEPS})
        set_property(GLOBAL PROPERTY PARABIX_SRCHASH_EXPLICIT_${NAME} TRUE)
    endif()
endfunction(parabix_srchash_add_inputs)

function(parabix_srchash_add_codegen_inputs LABEL)
    foreach(FILE ${ARGN})
        get_filename_component(ABS ${FILE} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        set_property(GLOBAL APPEND PROPERTY PARABIX_SRCHASH_CODEGEN_INPUTS ${ABS})
    endforeach()
    set_property(GLOBAL APPEND PROPERTY PARABIX_SRCHASH_CODEGEN_LABELS ${LABEL})
endfunction(parabix_srchash_add_codegen_inputs)

# The direct code generation dependencies of module or executable NAME.
function(parabix_srchash_codegen_deps OUT NAME)
    get_property(EXPLICIT GLOBAL PROPERTY PARABIX_SRCHASH_EXPLICIT_${NAME})
    if(EXPLICIT)
        get_property(DEPS GLOBAL PROPERTY PARABIX_SRCHASH_CODEGEN_DEPS_${NAME})
    else()
        # Those recorded with the inputs, plus whatever was linked later with
        # target_link_libraries, less the modules that never shape kernel code.
        get_property(DEPS GLOBAL PROPERTY PARABIX_SRCHASH_DEPS_${NAME})
        if(TARGET ${NAME})
            get_target_property(LINKED ${NAME} LINK_LIBRARIES)
            if(LINKED)
                list(APPEND DEPS ${LINKED})
            endif()
        endif()
        if(DEPS)
            list(REMOVE_ITEM DEPS ${PARABIX_SRCHASH_NON_CODEGEN_MODULES})
        endif()
    endif()
    set(${OUT} ${DEPS} PARENT_SCOPE)
endfunction(parabix_srchash_codegen_deps)

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
    parabix_srchash_write_if_different(${FILE} "${CONTENT}")
endfunction(parabix_srchash_write_config)

function(parabix_srchash_finalize)
    file(MAKE_DIRECTORY ${PARABIX_SRCHASH_DIR})
    set(CONFIG_FILE ${PARABIX_SRCHASH_DIR}/config.txt)
    parabix_srchash_write_config(${CONFIG_FILE})

    get_property(NAMES GLOBAL PROPERTY PARABIX_SRCHASH_NAMES)
    get_property(TARGETS GLOBAL PROPERTY PARABIX_SRCHASH_TARGETS)
    get_property(CODEGEN_INPUTS GLOBAL PROPERTY PARABIX_SRCHASH_CODEGEN_INPUTS)
    get_property(CODEGEN_LABELS GLOBAL PROPERTY PARABIX_SRCHASH_CODEGEN_LABELS)
    foreach(TGT ${TARGETS})
        if(TGT STREQUAL PARABIX_MONO_TARGET)
            # The single artifact holds every module.
            set(DEPS ${NAMES})
        else()
            parabix_srchash_codegen_deps(DEPS ${TGT})
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
                parabix_srchash_codegen_deps(MORE ${DEP})
                list(APPEND PENDING ${MORE})
            endif()
        endwhile()
        set(INPUTS ${CONFIG_FILE} ${CODEGEN_INPUTS})
        foreach(MODULE ${CLOSURE})
            get_property(MODULE_INPUTS GLOBAL PROPERTY PARABIX_SRCHASH_INPUTS_${MODULE})
            list(APPEND INPUTS ${MODULE_INPUTS})
        endforeach()
        list(REMOVE_DUPLICATES INPUTS)
        list(SORT INPUTS)
        set_property(TARGET ${TGT} PROPERTY PARABIX_SRCHASH_CLOSURE ${INPUTS})
        list(SORT CLOSURE)
        list(APPEND CLOSURE ${CODEGEN_LABELS})
        string(MAKE_C_IDENTIFIER ${TGT} ID)
        string(REPLACE ";" "\n" INPUT_LINES "${INPUTS}")
        string(REPLACE ";" " " MODULE_NAMES "${CLOSURE}")
        parabix_srchash_write_if_different(${PARABIX_SRCHASH_DIR}/${ID}.inputs
            "# modules: ${MODULE_NAMES}\n${INPUT_LINES}\n")
    endforeach()
endfunction(parabix_srchash_finalize)

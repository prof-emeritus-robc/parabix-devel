#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# IMPORTANT:
#
# If an explicit link with LLVM is needed, link with the toolchain library indead.
# See lib/toolchain/CMakeLists.txt for more info.
#
# i.e.,
#   DO:
#       target_link_libraries(<mylib> toolchain)
#
#   DON'T:
#       target_link_libraries(<mylib> ${REQ_LLVM_LIBRARIES})

cmake_minimum_required(VERSION 3.12)

# Allow target_link_libraries on the targets of other directories, as
# parabix_single_finalize does.  Set before any function is defined, since
# functions use the policies in effect where they are defined.
if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

project(parabix VERSION 0.9.0)
enable_testing()

###   Build Options   ###

option(USE_ADDRESS_SANITIZER "Enables use of address sanitizer in debug mode if available" OFF)
option(ENABLE_ICU "Enable ICU support if available" ON)
option(ENABLE_PAPI "Enable PAPI support if available" OFF)
option(REPORT_PAPI_TESTS "Report PAPI testing results" OFF)
option(LLVM_PATH "LLVM root directory")
option(GENERATE_TOOLS "Controls whether or not it generate tools" ON)
option(USE_LIBBACKTRACE "Allows framework to provide more detailed backtraces in JIT compiled code. Library available from github.com/ianlancetaylor/libbacktrace" OFF)
option(PARABIX_BUILD_TESTS "Build the kernel tests (tests/) and unit tests (QA/unit-tests)" ON)

# Build Artifact Configuration
set(ARTIFACT_MODE "Multiple" CACHE STRING "Build framework into a single artifact (Single) or multiple artifacts (Multiple)")
set(ARTIFACT_TYPE "Shared" CACHE STRING "Build framework as shared (Shared) or static (Static) libraries")

# Validate framework build style
set(VALID_BUILD_MODE "Single" "Multiple")
set(VALID_BUILD_TYPE "Shared" "Static")
list(FIND VALID_BUILD_MODE ${ARTIFACT_MODE} __am_index)
if(__am_index EQUAL -1)
    message(FATAL_ERROR "ARTIFACT_MODE must be either Single or Multiple")
endif()
list(FIND VALID_BUILD_TYPE ${ARTIFACT_TYPE} __at_index)
if(__at_index EQUAL -1)
    message(FATAL_ERROR "ARTIFACT_TYPE must be either Shared or Static")
endif()

# Target for single shared library
set(PARABIX_MONO_TARGET parabix)

# Installation (cmake/ParabixInstall.cmake), from ARTIFACT_MODE=Single builds only.
set(PARABIX_INSTALL_SCOPE "User" CACHE STRING
    "Install for the user building (User, default prefix ~/.local) or for all users (System, default prefix /usr/local)")
set_property(CACHE PARABIX_INSTALL_SCOPE PROPERTY STRINGS User System)
if(NOT PARABIX_INSTALL_SCOPE MATCHES "^(User|System)$")
    message(FATAL_ERROR "PARABIX_INSTALL_SCOPE must be either User or System")
endif()
# The prefix follows the scope (also when the scope is changed later), unless
# it was given: PARABIX_SCOPE_PREFIX is the one last set for a scope.
if(PARABIX_INSTALL_SCOPE STREQUAL "User")
    set(SCOPE_PREFIX "$ENV{HOME}/.local")
else()
    set(SCOPE_PREFIX "/usr/local")
endif()
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR CMAKE_INSTALL_PREFIX STREQUAL "${PARABIX_SCOPE_PREFIX}")
    set(CMAKE_INSTALL_PREFIX "${SCOPE_PREFIX}" CACHE PATH "Install path prefix" FORCE)
    set(PARABIX_SCOPE_PREFIX "${SCOPE_PREFIX}" CACHE INTERNAL "The install prefix set for PARABIX_INSTALL_SCOPE")
endif()
set(PARABIX_INSTALL_TOOLS "default" CACHE STRING
    "Tools installed by make install: default, dev, all, or tool names (see cmake/ParabixInstall.cmake)")
option(PARABIX_STATIC_BOOST "Link Boost statically into the single library (ARTIFACT_MODE=Single)" ON)
# The system-wide object cache: read-only for the tools, filled when
# installing (see cmake/ParabixInstall.cmake).  Only for System installs.
set(PARABIX_SYSTEM_OBJECT_CACHE_DIR "" CACHE STRING
    "System-wide object cache of a System install (empty: /var/cache/parabix, /Library/Caches/parabix on macOS; none: no system cache)")
set(PARABIX_SYSTEM_CACHE "")
if(ARTIFACT_MODE STREQUAL "Single" AND PARABIX_INSTALL_SCOPE STREQUAL "System"
   AND NOT PARABIX_SYSTEM_OBJECT_CACHE_DIR STREQUAL "none")
    if(PARABIX_SYSTEM_OBJECT_CACHE_DIR)
        set(PARABIX_SYSTEM_CACHE ${PARABIX_SYSTEM_OBJECT_CACHE_DIR})
    elseif(APPLE)
        set(PARABIX_SYSTEM_CACHE /Library/Caches/parabix)
    else()
        set(PARABIX_SYSTEM_CACHE /var/cache/parabix)
    endif()
endif()

# Ensure custom FindXYZ.cmake files are found
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

###   Versioning   ###
set(Parabix_REVISION "parabix")
include(VersionFromVCS)
add_version_info_from_vcs(Parabix_REVISION)
message(STATUS "Parabix Revision: ${Parabix_REVISION}")


###   Import Dependency: LLVM   ###
set (MIN_LLVM_VERSION "17")
if (LLVM_PATH)
find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_PATH} NO_DEFAULT_PATH)
else()
find_package(LLVM REQUIRED CONFIG)
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in: ${LLVM_DIR}")
if (${LLVM_PACKAGE_VERSION} VERSION_LESS MIN_LLVM_VERSION)
message(FATAL_ERROR "LLVM ${MIN_LLVM_VERSION} or greater required.  ${LLVM_PACKAGE_VERSION} unsupported.")
endif()
message(STATUS "Host Processor ${CMAKE_HOST_SYSTEM_PROCESSOR}")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64") 
set(PARABIX_TARGETS_TO_BUILD AArch64 ARM)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DPARABIX_ARM_TARGET")
if(APPLE)
# An option of Apple's linker only
add_link_options(LINKER:-no_warn_duplicate_libraries)
endif()
else()
set(PARABIX_TARGETS_TO_BUILD X86)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DPARABIX_X86_TARGET")
endif()

llvm_map_components_to_libnames(REQ_LLVM_LIBRARIES ${PARABIX_TARGETS_TO_BUILD} orcjit jitlink mcjit native IRReader Linker IRPrinter Passes)
# The shared module libraries of a development build link LLVM's own shared
# library where LLVM was built with one (as LLVM's tools are, e.g., in the
# Linux distributions and FreeBSD).  Linked with the static LLVM libraries,
# several module libraries would each have a copy of some of LLVM's
# command-line options ("Option ... registered more than once").
if(ARTIFACT_MODE STREQUAL "Multiple" AND ARTIFACT_TYPE STREQUAL "Shared" AND NOT APPLE
   AND LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
    set(REQ_LLVM_LIBRARIES LLVM)
    message(STATUS "Linking LLVM's shared library")
endif()

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
math(EXPR LLVM_VERSION_INTEGER "${LLVM_VERSION_MAJOR} * 10000 + ${LLVM_VERSION_MINOR} * 100 + ${LLVM_VERSION_PATCH}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DLLVM_VERSION_INTEGER=${LLVM_VERSION_INTEGER}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DLLVM_VERSION_MAJOR=${LLVM_VERSION_MAJOR} -DLLVM_VERSION_MINOR=${LLVM_VERSION_MINOR}")
message(STATUS "LLVM Version Integer: ${LLVM_VERSION_INTEGER}")

###   Import Dependency: threads   ###

# The C library has the POSIX thread functions on Linux and macOS, but not on
# the BSDs (FreeBSD's libthr): every module links them (Threads::Threads).
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

###   Import Dependency: Boost   ###

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)   # use Boost's own BoostConfig.cmake, not CMake's FindBoost
endif()
if(ARTIFACT_MODE STREQUAL "Single" AND PARABIX_STATIC_BOOST AND NOT APPLE
   AND NOT DEFINED PARABIX_BOOST_STATIC_IS_PIC)
    # Static libraries built without -fPIC cannot be part of a shared library
    # (as the x86-64 Boost archives of Debian and Ubuntu are).
    message(STATUS "Checking that the static Boost libraries can be part of a shared library")
    string(REPLACE ";" "\\;" PREFIX_PATH "${CMAKE_PREFIX_PATH}")
    try_compile(PARABIX_BOOST_STATIC_IS_PIC
        "${PROJECT_BINARY_DIR}/CMakeFiles/BoostStaticPicCheck"
        "${PROJECT_SOURCE_DIR}/cmake/BoostStaticPicCheck" boost_static_pic_check
        CMAKE_FLAGS "-DCMAKE_PREFIX_PATH=${PREFIX_PATH}" "-DBoost_DIR=${Boost_DIR}"
                    "-DBOOST_ROOT=${BOOST_ROOT}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    set(PARABIX_BOOST_STATIC_IS_PIC ${PARABIX_BOOST_STATIC_IS_PIC} CACHE INTERNAL
        "The static Boost libraries can be part of a shared library")
endif()
if(ARTIFACT_MODE STREQUAL "Single" AND PARABIX_STATIC_BOOST
   AND (APPLE OR PARABIX_BOOST_STATIC_IS_PIC))
    # Part of libparabix, so that installed programs do not depend on the
    # Boost libraries (and versions) of the build machine.
    set(Boost_USE_STATIC_LIBS ON)
elseif(ARTIFACT_MODE STREQUAL "Single" AND PARABIX_STATIC_BOOST)
    message(STATUS "The static Boost libraries are not position-independent (built without "
                   "-fPIC): linking Boost dynamically, so the installed tools use the Boost "
                   "libraries of this machine")
    set(Boost_USE_STATIC_LIBS OFF)
else()
    set(Boost_USE_STATIC_LIBS ${USE_STATIC_LIBS})
endif()
set(Boost_USE_MULTITHREADED OFF)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.61 REQUIRED COMPONENTS filesystem iostreams regex)
message(STATUS "Found Boost_LIBRARY_DIRS: ${Boost_LIBRARY_DIRS}")
include_directories("${Boost_INCLUDE_DIRS}")
link_directories(${Boost_LIBRARY_DIRS})

###   Import Dependency: ICU   ###

if (ENABLE_ICU)
    if(APPLE)
        execute_process(
            COMMAND brew --prefix icu4c
            OUTPUT_STRIP_TRAILING_WHITESPACE
            OUTPUT_VARIABLE BREW_ICU_ROOT
            ERROR_QUIET
        )
        if(BREW_ICU_ROOT)
            set(ICU_ROOT "${BREW_ICU_ROOT}")
            message(STATUS "Found ICU: ${ICU_ROOT}")
        endif()
    endif()
endif()

###   Static Boost: its dependencies   ###

if(Boost_USE_STATIC_LIBS)
    # Static Boost libraries name their own dependencies (e.g., iostreams'
    # bz2, lzma, z and zstd, regex's ICU) without a directory, which the
    # linker may not search (e.g., Homebrew's lib, or ICU's); give the ones
    # found in full.
    foreach(LIB ${Boost_LIBRARIES})
        if(NOT TARGET ${LIB})
            continue()
        endif()
        get_target_property(DEPS ${LIB} INTERFACE_LINK_LIBRARIES)
        if(NOT DEPS)
            continue()
        endif()
        set(RESOLVED "")
        foreach(DEP ${DEPS})
            if(DEP MATCHES "^[A-Za-z0-9_+.]+$" AND NOT TARGET ${DEP})
                find_library(PARABIX_BOOST_DEP_${DEP} ${DEP} HINTS ${ICU_ROOT}/lib)
                if(PARABIX_BOOST_DEP_${DEP})
                    set(DEP ${PARABIX_BOOST_DEP_${DEP}})
                elseif(NOT DEP MATCHES "^(pthread|rt|dl|m|c)$")
                    # (the linker finds those of the C library itself)
                    list(APPEND PARABIX_BOOST_DEPS_MISSING ${DEP})
                endif()
            endif()
            list(APPEND RESOLVED ${DEP})
        endforeach()
        set_target_properties(${LIB} PROPERTIES INTERFACE_LINK_LIBRARIES "${RESOLVED}")
    endforeach()
    if(PARABIX_BOOST_DEPS_MISSING)
        list(REMOVE_DUPLICATES PARABIX_BOOST_DEPS_MISSING)
        string(REPLACE ";" " " MISSING "${PARABIX_BOOST_DEPS_MISSING}")
        message(FATAL_ERROR
            "The static Boost libraries need libraries that are not installed: ${MISSING}\n"
            "Install their development packages (e.g., libbz2-dev liblzma-dev libzstd-dev on "
            "Debian and Ubuntu; bzip2-devel xz-devel libzstd-devel on Fedora), or link Boost "
            "dynamically with -DPARABIX_STATIC_BOOST=OFF (the installed tools then depend on "
            "the Boost libraries of this machine).")
    endif()
endif()

###   Import Dependency: PAPI   ###

if (ENABLE_PAPI)
find_package(PAPI REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DENABLE_PAPI -lpapi")
include_directories(${PAPI_INCLUDE_DIRS})
message(STATUS "Found PAPI_INCLUDE_DIRS: ${PAPI_INCLUDE_DIRS}")
if (REPORT_PAPI_TESTS)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DREPORT_PAPI_TESTS")
else()
message(STATUS "Report PAPI tests was not enabled")
endif()
endif()

###   Import Dependency: LibBacktrace   ###

IF (CMAKE_BUILD_TYPE MATCHES Debug)
if (USE_LIBBACKTRACE)
# MacOS stores debug info in dSYM files but the source of this information is
# not accessible by libbacktrace without first running dsymutil on the executable.
    find_package(Libbacktrace)
    if (LIBBACKTRACE_FOUND)
        find_package(Libunwind)
        if (NOT LIBUNWIND_FOUND)
            find_package(Backtrace)
        endif()
        if (LIBUNWIND_FOUND OR Backtrace_FOUND)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DENABLE_LIBBACKTRACE -g")
            SET (CMAKE_ENABLE_EXPORTS TRUE)
            if(APPLE)
            set(CMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf")
            endif()
            include_directories(${LIBBACKTRACE_INCLUDE_DIRS})
            if (LIBUNWIND_FOUND)
            include_directories(${LIBUNWIND_INCLUDE_DIR})
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHAS_LIBUNWIND")
            endif()
            if (Backtrace_FOUND)
            include_directories(${Backtrace_INCLUDE_DIRS})
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHAS_BACKTRACE")
            endif()
            message(STATUS "Found LibBacktrace: ${LIBBACKTRACE_INCLUDE_DIRS}")
        else()
            message(STATUS "Found LibBacktrace but neither libunwind nor backtrace are present.")
            UNSET(LIBBACKTRACE_FOUND)
        endif()
    endif()
endif()
endif()

###   Project Configuration   ###

# Require that compilers support all features of C17 and C++17.
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

# Define `CXX_STANDARD` to enable conditional compilation based on standard version.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCXX_STANDARD=${CMAKE_CXX_STANDARD}")

# Add include directories.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Place compiled executables in build/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

# Installation directories (GNUInstallDirs), and the files used at run time.
# Programs find them relative to their own directory, by the relations
# between the installation directories (include/toolchain/install_paths.h),
# and the build tree is laid out the same way: <build>/bin with
# <build>/libexec/parabix and <build>/share/parabix, by default.  The object
# cache is found at run time (-object-cache-dir, $PARABIX_OBJECT_CACHE_DIR,
# or ~/.parabix/objcache), so nothing is written outside the build tree.
include(GNUInstallDirs)
file(RELATIVE_PATH PARABIX_BINDIR_TO_LIBEXECDIR ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_LIBEXECDIR})
file(RELATIVE_PATH PARABIX_BINDIR_TO_DATADIR ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DATADIR})
get_filename_component(PARABIX_BUILD_LIBEXECDIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${PARABIX_BINDIR_TO_LIBEXECDIR}/parabix ABSOLUTE)
get_filename_component(PARABIX_BUILD_DATADIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${PARABIX_BINDIR_TO_DATADIR}/parabix ABSOLUTE)

# In ARTIFACT_MODE=Single, the build tree has the installed layout of the
# libraries as well (<build>/lib/libparabix), and programs find them by a
# run path relative to their own directory ($ORIGIN/../lib), the one they
# are installed with.  Installing a program is then copying it, under its
# installed name (e.g., pwc for wc).
if(ARTIFACT_MODE STREQUAL "Single")
    file(RELATIVE_PATH PARABIX_BINDIR_TO_LIBDIR ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_LIBDIR})
    get_filename_component(PARABIX_BUILD_LIBDIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${PARABIX_BINDIR_TO_LIBDIR} ABSOLUTE)
    if(APPLE)
        set(PARABIX_RPATH_ORIGIN "@loader_path")
    else()
        set(PARABIX_RPATH_ORIGIN "$ORIGIN")
    endif()
    set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    # Libraries outside the project (e.g., ICU) stay where they were found.
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# Sets the run path of a Parabix program or library of ARTIFACT_MODE=Single:
# from its output directory to <build>/lib, as from the installed one.
function(parabix_set_single_rpath TARGET OUTPUT_DIR)
    file(RELATIVE_PATH REL ${OUTPUT_DIR} ${PARABIX_BUILD_LIBDIR})
    if(REL STREQUAL "")
        set(RPATH "${PARABIX_RPATH_ORIGIN}")
    else()
        set(RPATH "${PARABIX_RPATH_ORIGIN}/${REL}")
    endif()
    set_target_properties(${TARGET} PROPERTIES INSTALL_RPATH "${RPATH}")
endfunction(parabix_set_single_rpath)

# Use @rpath for dylibs on macOS
set(CMAKE_MACOSX_RPATH ON)

# Enable all warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pedantic") # -Werror=global-constructors

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fvisibility-inlines-hidden" SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
if(SUPPORTS_FVISIBILITY_INLINES_HIDDEN_FLAG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility-inlines-hidden")
endif()

# no pie disables the generation of position-independent executables, which is a default security feature of newer compiles
# that prevents addr2line from being able to resolve which line corresponds to the code at run time. This in turn prevents
# CreateAssert from being able to provide the compilation call stack for each JIT'ed assertion error.

UNSET(HAS_EXECINFO CACHE)
UNSET(HAS_MACH_VM_TYPES CACHE)

# Disable Dual ABI support
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=1")

set(CMAKE_REQUIRED_FLAGS)

# Define parabix version
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DPARABIX_VERSION='\"${Parabix_REVISION}\"'")

# Kernels defined in an image without a registered source hash fall back to the
# image's linker build ID (see lib/srchash/source_hash.cpp); make sure ELF
# binaries always carry an NT_GNU_BUILD_ID note (Linux and the BSDs alike).
# Mach-O binaries get LC_UUID by default.
if (NOT APPLE)
    add_link_options(-Wl,--build-id)
endif()

# Use address sanitizer if desiered
unset(HAS_ADDRESS_SANITIZER)
if (USE_ADDRESS_SANITIZER)
    #CHECK_INCLUDE_FILE_CXX("sanitizer/asan_interface.h" HAS_ADDRESS_SANITIZER)
    #if (HAS_ADDRESS_SANITIZER)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHAS_ADDRESS_SANITIZER -fsanitize=address,undefined")
    #link_libraries(-fsanitize=address -shared-libasan)
    #endif()
endif()

###   Functions   ###

# Sets the output name for a given target.
# Naming scheme is "parabix_<target>" with any '.' replaced with a '_'
function(parabix_set_library_name NAME)
    string(REPLACE "." "_" DELIMITED_NAME ${NAME})
    string(CONCAT LIBRARY_NAME "parabix_" ${DELIMITED_NAME})
    set_target_properties(${NAME} PROPERTIES OUTPUT_NAME ${LIBRARY_NAME})
endfunction(parabix_set_library_name)

include(CMakeParseArguments)
include(ParabixSourceHash)

# Release & Debug flags

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG} -O1 -g -fvisibility=default -fno-omit-frame-pointer -fno-optimize-sibling-calls")

# Disable RunTime Type Information
if(MSVC) # using Visual Studio C++
    set(RTTI "/GR")
    set(NO_RTTI "/GR-")
else() # using Clang, GCC, Intel C++, etc
    set(RTTI "-frtti")
    set(NO_RTTI "-fno-rtti")
endif()

function(parabix_add_module)
    cmake_parse_arguments(
        ARG             # resultant argument prefix
        "USES_CERNROOT;REQUIRES_RTTI;NO_SOURCE_HASH;EXCLUDE_FROM_SINGLE" # boolean args
                        #   EXCLUDE_FROM_SINGLE: in ARTIFACT_MODE=Single, build the module as
                        #       a library of its own, not as part of the single artifact
                        #       (for modules only tests use)
        "NAME"          # mono-valued arguments:
                        #   NAME: target name
        "SRC;DEPS;CODEGEN_DEPS" # multi-valued arguments:
                        #   SRC:  source file list
                        #   DEPS: target dependencies
                        #   CODEGEN_DEPS: the DEPS whose code can run while this module's
                        #       kernels are generated (see cmake/ParabixSourceHash.cmake)
        ${ARGN}         # arguments to parse
    )
    if(NOT ARG_NAME)
        message(FATAL_ERROR "parabix_add_module: A target name is required")
    endif()
    # The public headers of lib/<path> are in include/<path>
    file(RELATIVE_PATH MODULE_PATH ${PROJECT_SOURCE_DIR}/lib ${CMAKE_CURRENT_SOURCE_DIR})
    set(INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include/${MODULE_PATH})
    if("CODEGEN_DEPS" IN_LIST ARGN)
        set(SRCHASH_CODEGEN_ARGS CODEGEN_DEPS ${ARG_CODEGEN_DEPS})
    endif()
    parabix_srchash_add_inputs(${ARG_NAME} SRC ${ARG_SRC} DEPS ${ARG_DEPS} ${SRCHASH_CODEGEN_ARGS} INCLUDE_DIR ${INCLUDE_DIR})
    if(ARTIFACT_MODE STREQUAL "Single" AND NOT ARG_EXCLUDE_FROM_SINGLE)
        # An object library: its objects are linked into the single artifact
        # (parabix_single_finalize), and its settings and dependencies apply
        # to them as they would to a library of its own.
        add_library(${ARG_NAME} OBJECT ${ARG_SRC})
        set_target_properties(${ARG_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        set_property(GLOBAL APPEND PROPERTY PARABIX_SINGLE_MODULES ${ARG_NAME})
        target_link_libraries(${ARG_NAME} PUBLIC ${ARG_DEPS})
    else()
        if(ARTIFACT_TYPE STREQUAL "Static")
            add_library(${ARG_NAME} STATIC ${ARG_SRC})
        else()
            add_library(${ARG_NAME} SHARED ${ARG_SRC})
        endif()
        parabix_set_library_name(${ARG_NAME})
        if(NOT ARG_NO_SOURCE_HASH)
            parabix_srchash_attach(${ARG_NAME})
        endif()
        if(ARTIFACT_MODE STREQUAL "Single")
            set_target_properties(${ARG_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PARABIX_BUILD_LIBDIR})
            parabix_set_single_rpath(${ARG_NAME} ${PARABIX_BUILD_LIBDIR})
            parabix_single_link_later(${ARG_NAME} ${ARG_DEPS})
        else()
            target_link_libraries(${ARG_NAME} PUBLIC ${ARG_DEPS})
        endif()
    endif()
    target_link_libraries(${ARG_NAME} PUBLIC Threads::Threads)
    if (ARG_REQUIRES_RTTI)
        target_compile_options(${ARG_NAME} PUBLIC ${RTTI})
    else()
        target_compile_options(${ARG_NAME} PUBLIC ${NO_RTTI})
    endif()
    if (ENABLE_PAPI)
        target_link_libraries(${ARG_NAME} PUBLIC ${PAPI_LIBRARIES})
    endif()
    if (LIBBACKTRACE_FOUND)
        target_link_libraries(${ARG_NAME} PUBLIC ${LIBBACKTRACE_LIBRARIES})
        if (LIBUNWIND_FOUND)
        target_link_libraries(${ARG_NAME} PUBLIC ${LIBUNWIND_LIBRARIES})
        endif()
        if (Backtrace_FOUND)
        target_link_libraries(${ARG_NAME} PUBLIC ${Backtrace_LIBRARIES})
        endif()
    endif()
    IF (CMAKE_BUILD_TYPE MATCHES Debug)
        target_link_libraries(${ARG_NAME} PUBLIC ${CMAKE_DL_LIBS})
    endif()
endfunction(parabix_add_module)

# In ARTIFACT_MODE=Single, the modules a target depends on are part of the
# single artifact, so it links that instead of them; linking their object
# libraries would add a second copy of their objects.  Which DEPS are such
# modules is known only once all are defined, so the links are made by
# parabix_single_finalize.
function(parabix_single_link_later TARGET)
    set_property(GLOBAL APPEND PROPERTY PARABIX_SINGLE_USERS ${TARGET})
    set_property(GLOBAL PROPERTY PARABIX_SINGLE_DEPS_${TARGET} ${ARGN})
endfunction(parabix_single_link_later)

function(parabix_single_finalize)
    get_property(MODULES GLOBAL PROPERTY PARABIX_SINGLE_MODULES)
    # PRIVATE: the users of the single artifact are not to link the modules'
    # own dependencies (e.g., the LLVM archives) a second time.
    target_link_libraries(${PARABIX_MONO_TARGET} PRIVATE ${MODULES})
    if(Boost_USE_STATIC_LIBS AND NOT ARTIFACT_TYPE STREQUAL "Static")
        # All of Boost's archives are in the artifact, for its users too (on
        # Apple systems, toolchain's -all_load does this); a second copy in
        # a program would have its own Boost globals.
        if(NOT APPLE)
            set(ARCHIVES "")
            foreach(LIB ${Boost_LIBRARIES})
                if(TARGET ${LIB})
                    list(APPEND ARCHIVES "$<TARGET_LINKER_FILE:${LIB}>")
                else()
                    list(APPEND ARCHIVES ${LIB})
                endif()
            endforeach()
            target_link_libraries(${PARABIX_MONO_TARGET} PRIVATE -Wl,--whole-archive ${ARCHIVES} -Wl,--no-whole-archive)
        endif()
    else()
        # What a user of the separate module libraries would get from them.
        target_link_libraries(${PARABIX_MONO_TARGET} INTERFACE ${Boost_LIBRARIES})
    endif()
    if(NOT APPLE OR ARTIFACT_TYPE STREQUAL "Static")
        # Not embedded in the artifact (see lib/toolchain/CMakeLists.txt)
        target_link_libraries(${PARABIX_MONO_TARGET} INTERFACE ${REQ_LLVM_LIBRARIES})
    endif()
    get_property(USERS GLOBAL PROPERTY PARABIX_SINGLE_USERS)
    foreach(USER ${USERS})
        get_property(DEPS GLOBAL PROPERTY PARABIX_SINGLE_DEPS_${USER})
        if(DEPS)
            list(REMOVE_ITEM DEPS ${MODULES})
        endif()
        target_link_libraries(${USER} PUBLIC ${PARABIX_MONO_TARGET} ${DEPS})
    endforeach()
endfunction(parabix_single_finalize)

function(parabix_add_executable)
    cmake_parse_arguments(
        ARG         # resultant argument prefix
        ""          # boolean args
        "NAME"      # mono-valued arguments:
                    #   NAME: target name
        "SRC;DEPS"  # multi-valued arguments:
                    #   SRC:  source file list
                    #   DEPS: target dependencies
        ${ARGN}     # arguments to parse
    )
    if(NOT ARG_NAME)
        message(FATAL_ERROR "parabix_add_executable: A target name is required")
    endif()
    add_executable(${ARG_NAME} ${ARG_SRC})
    # The tools, which cmake/ParabixInstall.cmake installs
    string(FIND "${CMAKE_CURRENT_SOURCE_DIR}/" "${PROJECT_SOURCE_DIR}/tools/" TOOLS_DIR_POS)
    if(TOOLS_DIR_POS EQUAL 0)
        set_property(GLOBAL APPEND PROPERTY PARABIX_TOOLS ${ARG_NAME})
    endif()
    parabix_srchash_add_inputs(${ARG_NAME} SRC ${ARG_SRC} DEPS ${ARG_DEPS} toolchain kernel.pipeline)
    parabix_srchash_attach(${ARG_NAME})
    target_compile_options(${ARG_NAME} PRIVATE ${NO_RTTI})
    if(ARTIFACT_MODE STREQUAL "Single")
        parabix_set_single_rpath(${ARG_NAME} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        parabix_single_link_later(${ARG_NAME} ${ARG_DEPS})
    else()
        target_link_libraries(${ARG_NAME} PUBLIC ${ARG_DEPS})
        target_link_libraries(${ARG_NAME} PUBLIC toolchain)
        target_link_libraries(${ARG_NAME} PUBLIC kernel.pipeline)
    endif()
endfunction(parabix_add_executable)

# Creates a pablo source target which copies pablo source files to the data
# directory of the build tree (<build>/share/parabix/pablo), where the
# programs reading them find them (pablo::parse::SourceFile::Relative).
#
# Usage: parabix_add_pablo_src(target-name source-files...)
function(parabix_add_pablo_src NAME)
    set(COPIES "")
    foreach(PABLO_FILE ${ARGN})
        get_filename_component(PABLO_NAME ${PABLO_FILE} NAME)
        set(COPY ${PARABIX_BUILD_DATADIR}/pablo/${PABLO_NAME})
        add_custom_command(OUTPUT ${COPY}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PARABIX_BUILD_DATADIR}/pablo
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/${PABLO_FILE} ${COPY}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${PABLO_FILE})
        list(APPEND COPIES ${COPY})
    endforeach(PABLO_FILE)
    add_custom_target(${NAME} DEPENDS ${COPIES})
    # Installed with the tool <tool> of the target <tool>.pablosrc
    string(REGEX REPLACE "\\.pablosrc$" "" TOOL ${NAME})
    foreach(PABLO_FILE ${ARGN})
        set_property(GLOBAL APPEND PROPERTY PARABIX_PABLO_SOURCES_${TOOL} ${CMAKE_CURRENT_SOURCE_DIR}/${PABLO_FILE})
    endforeach(PABLO_FILE)
endfunction(parabix_add_pablo_src)


###   Add Subdirectories   ###

if(ARTIFACT_MODE STREQUAL "Single")
    # CMP0079 (3.13) for parabix_single_finalize, install(... EXCLUDE_FROM_ALL)
    # of all kinds (3.14) for cmake/ParabixInstall.cmake
    if(CMAKE_VERSION VERSION_LESS 3.14)
        message(FATAL_ERROR "ARTIFACT_MODE=Single requires CMake 3.14 or later")
    endif()
    if(ARTIFACT_TYPE STREQUAL "Static")
        add_library(${PARABIX_MONO_TARGET} STATIC "")
    else()
        add_library(${PARABIX_MONO_TARGET} SHARED "")
        # The library's ABI is that of its version: 0.9 (until 1.0)
        set_target_properties(${PARABIX_MONO_TARGET} PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
    endif()
    set_target_properties(${PARABIX_MONO_TARGET} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${PARABIX_BUILD_LIBDIR}
        ARCHIVE_OUTPUT_DIRECTORY ${PARABIX_BUILD_LIBDIR})
    parabix_set_single_rpath(${PARABIX_MONO_TARGET} ${PARABIX_BUILD_LIBDIR})
    parabix_srchash_attach(${PARABIX_MONO_TARGET})
endif()

# Framework Libraries
add_subdirectory(lib/allocator)
add_subdirectory(lib/fileselect)
add_subdirectory(lib/grep)
add_subdirectory(lib/idisa)
add_subdirectory(lib/kernel/core)
add_subdirectory(lib/kernel/pipeline)
if (ROOT_FOUND)
add_subdirectory(lib/kernel/pipeline/compiler/statistics/cern-root)
endif()
add_subdirectory(lib/objcache)
add_subdirectory(runtime/cachejanitord)
add_subdirectory(lib/pablo/toolchain)
add_subdirectory(lib/pablo/compiler)
add_subdirectory(lib/pablo/bixnum)
add_subdirectory(lib/pablo/parse)
add_subdirectory(lib/re/adt)
add_subdirectory(lib/re/analysis)
add_subdirectory(lib/re/cc)
add_subdirectory(lib/re/compile)
add_subdirectory(lib/re/parse)
add_subdirectory(lib/re/printer)
add_subdirectory(lib/re/toolchain)
add_subdirectory(lib/re/transforms)
add_subdirectory(lib/re/unicode)
add_subdirectory(lib/srchash)
add_subdirectory(lib/testing)
add_subdirectory(lib/toolchain)
add_subdirectory(lib/ucd/algo)
add_subdirectory(lib/ucd/core)
add_subdirectory(lib/ucd/data)
add_subdirectory(lib/ucd/utf)
add_subdirectory(lib/audio)
add_subdirectory(lib/csv)
add_subdirectory(lib/json)
add_subdirectory(lib/ldml)

# Kernel Libraries
add_subdirectory(lib/kernel/basis)
add_subdirectory(lib/kernel/bitwise)
add_subdirectory(lib/kernel/io)
add_subdirectory(lib/kernel/re)
add_subdirectory(lib/kernel/scan)
add_subdirectory(lib/kernel/streamutils)
add_subdirectory(lib/kernel/unicode)
add_subdirectory(lib/kernel/unicode/normalization)
add_subdirectory(lib/kernel/util)
add_subdirectory(lib/kernel/illustrator)

# Executables
if (GENERATE_TOOLS)
    add_subdirectory(tools/base64)
    add_subdirectory(tools/editd)
    add_subdirectory(tools/icgrep)
    add_subdirectory(tools/idisa_test)
    add_subdirectory(tools/idisa_exerciser)
    add_subdirectory(tools/json)
    add_subdirectory(tools/lex)
    add_subdirectory(tools/transcoders)
    add_subdirectory(tools/wc)
    # add_subdirectory(tools/wc/wc-pablo)
    add_subdirectory(tools/xml)
    add_subdirectory(tools/ztf8)
    add_subdirectory(tools/audio)
    add_subdirectory(tools/util)
    add_subdirectory(tools/csv)
    add_subdirectory(tools/ldml)
endif()

if (PARABIX_BUILD_TESTS)
    # Kernel Tests
    add_subdirectory(tests)

    # Standalone unit tests
    add_subdirectory(QA/unit-tests)
endif()

# Links of the single library and of its users, once every module is defined
if(ARTIFACT_MODE STREQUAL "Single")
    parabix_single_finalize()
endif()

# Source hashes need every target defined (see cmake/ParabixSourceHash.cmake)
parabix_srchash_finalize()

# Install and uninstall rules, once every program is defined
include(ParabixInstall)

###   Packaging (CPack)   ###
#
# Binary packages of an ARTIFACT_MODE=Single build, holding what make install
# installs (the runtime and the tools of PARABIX_INSTALL_TOOLS), for all users
# of a system:
#
#   cmake -DCMAKE_BUILD_TYPE=Release -DARTIFACT_MODE=Single -DPARABIX_BUILD_TESTS=OFF \
#         -DPARABIX_INSTALL_SCOPE=System -DCMAKE_INSTALL_PREFIX=/usr/local \
#         [-DPARABIX_INSTALL_TOOLS="default;<tool>..."] [-DPARABIX_PACKAGE_MAINTAINER="Name <address>"] ..
#   make -j8
#   cpack                 all of PARABIX_PACKAGE_GENERATORS: TGZ, and DEB and RPM
#                         where dpkg-deb and rpmbuild are found
#   cpack -G DEB          one generator (TGZ, DEB, RPM, ...; see cpack --help)
#
# The packages are made like a DESTDIR installation, so the system-wide object
# cache is not filled then (its objects are for the processor of the machine
# filling it).  The DEB and RPM packages run <prefix>/share/parabix/prewarm.sh
# after installing, on the machine installed, and "prewarm.sh --remove" when
# removed.  A TGZ package holds bin, lib, ... under its top directory; unpack it
# at the prefix it was configured for (the system-wide cache is fixed), then
# fill the cache by hand (as root):
#   tar xzf parabix-<version>-<system>.tar.gz -C <prefix> --strip-components=1
#   sh <prefix>/share/parabix/prewarm.sh
# Uninstall a package with the package manager (or, for a TGZ, with
# <prefix>/share/parabix/uninstall.cmake, which needs CMake).
#
# The DEB package's dependencies are found by dpkg-shlibdeps, and the RPM's by
# rpmbuild.  Not a source package: make package_source is not configured.
if(ARTIFACT_MODE STREQUAL "Single")
    if(NOT PARABIX_INSTALL_SCOPE STREQUAL "System")
        message(STATUS "Packages (cpack) are for PARABIX_INSTALL_SCOPE=System")
    endif()
    set(PARABIX_PACKAGE_MAINTAINER "Parabix Project" CACHE STRING "The maintainer named in packages (DEB requires one)")
    set(PARABIX_PACKAGE_GENERATORS "TGZ" CACHE STRING "The package generators cpack runs by default")
    find_program(PARABIX_DPKG_DEB dpkg-deb)
    find_program(PARABIX_RPMBUILD rpmbuild)
    mark_as_advanced(PARABIX_DPKG_DEB PARABIX_RPMBUILD)
    set(CPACK_GENERATOR ${PARABIX_PACKAGE_GENERATORS})
    if(PARABIX_DPKG_DEB)
        list(APPEND CPACK_GENERATOR DEB)
    endif()
    if(PARABIX_RPMBUILD)
        list(APPEND CPACK_GENERATOR RPM)
    endif()
    list(REMOVE_DUPLICATES CPACK_GENERATOR)

    set(CPACK_PACKAGE_NAME parabix)
    set(CPACK_PACKAGE_VENDOR "Parabix Project")
    set(CPACK_PACKAGE_CONTACT "${PARABIX_PACKAGE_MAINTAINER}")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Unicode text processing tools (icgrep, transcoders, wc, csv, ...) using SIMD parallel bit streams")
    set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/OSL3.0.txt)
    set(CPACK_RESOURCE_FILE_README ${PROJECT_SOURCE_DIR}/README.md)
    set(CPACK_SOURCE_GENERATOR "")
    # DEB and RPM packages install at the prefix; archives hold bin, lib, ...
    # under their top directory, to unpack at the prefix:
    #   tar xzf parabix-<version>-<system>.tar.gz -C <prefix> --strip-components=1
    file(WRITE ${PROJECT_BINARY_DIR}/CPackProjectConfig.cmake
        "if(CPACK_GENERATOR MATCHES \"^(DEB|RPM)$\")\n"
        "    set(CPACK_PACKAGING_INSTALL_PREFIX \"${CMAKE_INSTALL_PREFIX}\")\n"
        "endif()\n")
    set(CPACK_PROJECT_CONFIG_FILE ${PROJECT_BINARY_DIR}/CPackProjectConfig.cmake)

    # Scripts run by the package managers: fill the system-wide object cache
    # after installing, and empty it when removing (not when upgrading).
    set(PACKAGE_SCRIPTS ${PROJECT_BINARY_DIR}/package-scripts)
    set(PREWARM "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/parabix/prewarm.sh")
    file(WRITE ${PACKAGE_SCRIPTS}/tmp/postinst
        "#!/bin/sh\n# Fill the system-wide object cache of Parabix\n"
        "[ ! -f \"${PREWARM}\" ] || sh \"${PREWARM}\" || true\n")
    file(WRITE ${PACKAGE_SCRIPTS}/tmp/prerm
        "#!/bin/sh\n# DEB: remove (not upgrade); RPM %preun: 0 when removing\n"
        "if [ \"$1\" = remove ] || [ \"$1\" = 0 ]; then\n"
        "    [ ! -f \"${PREWARM}\" ] || sh \"${PREWARM}\" --remove || true\n"
        "fi\n")
    file(COPY ${PACKAGE_SCRIPTS}/tmp/postinst ${PACKAGE_SCRIPTS}/tmp/prerm DESTINATION ${PACKAGE_SCRIPTS}
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${PARABIX_PACKAGE_MAINTAINER}")
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${PACKAGE_SCRIPTS}/postinst ${PACKAGE_SCRIPTS}/prerm)

    set(CPACK_RPM_PACKAGE_LICENSE "OSL-3.0")
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE ${PACKAGE_SCRIPTS}/postinst)
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE ${PACKAGE_SCRIPTS}/prerm)
    # Directories of the system, not of the package
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        ${CMAKE_INSTALL_PREFIX} ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_LIBDIR}
        ${CMAKE_INSTALL_FULL_LIBEXECDIR} ${CMAKE_INSTALL_FULL_DATADIR} ${CMAKE_INSTALL_FULL_DATADIR}/doc)

    include(CPack)
endif()

###   Test Suite   ###

set(QA_DIR ${PROJECT_SOURCE_DIR}/QA)
set(BIN_DIR ${PROJECT_BINARY_DIR}/bin)

add_test(
  NAME greptest
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 greptest.py --random_flag_count=2 --tests_per_grepcase=2 ${BIN_DIR}/icgrep)

add_test(
  NAME possessive_strings_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 possessive_strings_test.py ${BIN_DIR}/icgrep)

add_test(
  NAME proptest
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 greptest.py -d ${QA_DIR} -t ${QA_DIR}/proptest.xml ${BIN_DIR}/icgrep)

if (EXISTS /usr/local/data/arwiki-20150901-pages-articles.xml)
add_test (NAME perf_test_1
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 greptest.py -d /usr/local/data/ -t ${QA_DIR}/perftest.xml ${BIN_DIR}/icgrep)
endif()

add_test(
  NAME abc_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 abc_test.py ${BIN_DIR}/icgrep)

add_test(
  NAME replace_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 replace_test.py ${BIN_DIR}/icgrep)

add_test(
  NAME reversed_lookahead_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 reversed_lookahead_test.py ${BIN_DIR}/icgrep)

# The same, over the codes of multiplexed alphabets where the RE allows them
add_test(
  NAME reversed_lookahead_multiplexed_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 reversed_lookahead_test.py ${BIN_DIR}/icgrep)
set_tests_properties(reversed_lookahead_multiplexed_test PROPERTIES ENVIRONMENT "TEST_FLAGS=--re-multiplex=force")

add_test(
  NAME multiplex_auto_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 multiplex_auto_test.py ${BIN_DIR}/icgrep)

add_test(
  NAME utf8_errors_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 utf8_errors_test.py ${BIN_DIR}/icgrep)

add_test(
  NAME recursive_select_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 recursive_select_test.py ${BIN_DIR}/icgrep)

add_test(
  NAME wc_files_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 wc_files_test.py ${BIN_DIR})

# No two options of a program may share a name (in ARTIFACT_MODE=Single, of any library)
add_test(
  NAME option_registration_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 option_registration_test.py ${BIN_DIR} ${PROJECT_BINARY_DIR}/tests/bin)

add_test(
  NAME ldml_trules_test
  COMMAND ${BIN_DIR}/ldml_trules --self-test)

add_test(
  NAME tconv_engine_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_engine_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_prefix_runs_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_prefix_runs_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_chain_revisits_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_chain_revisits_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_mark_run_edits_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_mark_run_edits_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_walk_selection_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_walk_selection_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_fixed_captures_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_fixed_captures_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_kept_captures_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_kept_captures_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_icu_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_icu_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_possessive_lookahead_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_possessive_lookahead_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_context_strings_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_context_strings_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_text_anchors_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_text_anchors_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_filter_run_end_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_filter_run_end_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_blocking_marks_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_blocking_marks_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_mark_run_lookahead_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_mark_run_lookahead_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_insertion_fold_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_insertion_fold_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_lookahead_strings_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_lookahead_strings_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_capture_strings_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_capture_strings_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_leading_runs_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_leading_runs_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_run_contexts_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_run_contexts_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_walk_runs_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_walk_runs_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_split_runs_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_split_runs_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_context_capture_strings_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_context_capture_strings_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_run_strings_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_run_strings_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_front_letters_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_front_letters_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_converted_star_contexts_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_converted_star_contexts_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_run_moves_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_run_moves_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_capture_variants_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_capture_variants_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_insertion_revisit_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_insertion_revisit_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_prefix_run_contexts_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_prefix_run_contexts_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_prefix_run_exceptions_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_prefix_run_exceptions_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_prefix_run_revisits_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_prefix_run_revisits_test.py ${BIN_DIR}/tconv)

add_test(
  NAME tconv_repeated_captures_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 tconv_repeated_captures_test.py ${BIN_DIR}/tconv)

add_test(
  NAME u8u16_test
  WORKING_DIRECTORY ${QA_DIR}/u8u16
  COMMAND python3 u8u16_test.py --flags=-thread-num=2 "${BIN_DIR}/u8u16")

add_test(
  NAME u32u8_test
  WORKING_DIRECTORY ${QA_DIR}/
  COMMAND python3 u32u8_test.py "${BIN_DIR}/u32u8")

# A few critical combinations of nfd/nfc/nfkd with pipeline and focus modes,
# kept under one second; `make nfd_test`/`make nfc_test` run the fuller tests.
add_test(
  NAME normalization_test
  WORKING_DIRECTORY ${QA_DIR}
  COMMAND python3 normalization_test.py "${BIN_DIR}")

add_test(
    NAME editd_test
    WORKING_DIRECTORY ${QA_DIR}/editd
    COMMAND python3 run_all.py -e ${BIN_DIR})

add_test(
    NAME base64_test
    WORKING_DIRECTORY ${QA_DIR}/base64
    COMMAND python3 base64test.py ${BIN_DIR}/base64)

add_test(
  NAME gb18030_test
  WORKING_DIRECTORY ${QA_DIR}/gb18030
  COMMAND python3 gb18030_test.py "${BIN_DIR}/gb18030")

add_test(
  NAME json_test
  WORKING_DIRECTORY ${QA_DIR}/json
  COMMAND python3 json_test.py "${BIN_DIR}/json")

add_test(
  NAME objcache_test
  WORKING_DIRECTORY ${QA_DIR}/objcache
  COMMAND python3 objcache_test.py "${BIN_DIR}")

add_test(
  NAME xch_test
  WORKING_DIRECTORY ${QA_DIR}/xch
  COMMAND python3 xch_test.py "${BIN_DIR}/xch")

add_test(
  NAME hexlines_test
  WORKING_DIRECTORY ${QA_DIR}/hexlines
  COMMAND python3 hexlines_test.py "${BIN_DIR}/hexlines")

add_test(
  NAME csv2json_test
  WORKING_DIRECTORY ${QA_DIR}/csv/csv2json
  COMMAND python3 csv2json_test.py "${BIN_DIR}/csv2json")


SET_PROPERTY(TEST proptest PROPERTY TIMEOUT 1500)
SET_PROPERTY(TEST abc_test PROPERTY TIMEOUT 100)
SET_PROPERTY(TEST u8u16_test editd_test base64_test PROPERTY TIMEOUT 40)


add_custom_target (greptest
    WORKING_DIRECTORY ${QA_DIR}
    COMMAND python3 greptest.py --random_flag_count=2 --tests_per_grepcase=2 "${BIN_DIR}/icgrep")

add_custom_target (proptest
    WORKING_DIRECTORY ${QA_DIR}
    COMMAND python3 greptest.py -d ${QA_DIR} -t ${QA_DIR}/proptest.xml "${BIN_DIR}/icgrep")

add_custom_target (u8u16_test
    WORKING_DIRECTORY ${QA_DIR}/u8u16
    COMMAND python3 u8u16_test.py --flags=-thread-num=2 "${BIN_DIR}/u8u16")

add_custom_target (u32u8_test
    WORKING_DIRECTORY ${QA_DIR}
    COMMAND python3 u32u8_test.py "--flags=-EnableAsserts -VerifyIR" "${BIN_DIR}/u32u8")

add_custom_target (editd_test
    WORKING_DIRECTORY ${QA_DIR}/editd
    COMMAND python3 run_all.py -e ${BIN_DIR})

add_custom_target (base64_test
    WORKING_DIRECTORY ${QA_DIR}/base64
    COMMAND python3 base64test.py "${BIN_DIR}/base64")

add_custom_target(gb18030_test
  WORKING_DIRECTORY ${QA_DIR}/gb18030
  COMMAND python3 gb18030_test.py "${BIN_DIR}/gb18030")

# Also edits sources (restoring them) and rebuilds, checking which cached kernels are invalidated
add_custom_target(objcache_test
  WORKING_DIRECTORY ${QA_DIR}/objcache
  COMMAND python3 objcache_test.py --build-dir "${PROJECT_BINARY_DIR}" "${BIN_DIR}")

add_custom_target(nfd_test
  WORKING_DIRECTORY ${QA_DIR}/nfd
  COMMAND python3 nfd_test.py "${BIN_DIR}/nfd")

add_custom_target(nfc_test
  WORKING_DIRECTORY ${QA_DIR}/nfc
  COMMAND python3 nfc_test.py "${BIN_DIR}/nfc")

add_custom_target (abc_gen
    WORKING_DIRECTORY ${QA_DIR}
    COMMAND python3 abc_testgen.py)

add_custom_target (abc_test
    WORKING_DIRECTORY ${QA_DIR}
    COMMAND python3 abc_test.py "${BIN_DIR}/icgrep")

add_custom_target (check
    # Build everything before testing, rather than a hand-maintained DEPENDS
    # list of the binaries individual `add_test` entries happen to invoke --
    # that list has drifted out of sync before (nfd/nfc were missing, so
    # nfd_test/nfc_test silently ran against whatever nfd/nfc binary was
    # already sitting in the build directory, stale or absent) and is a
    # standing risk for every future test/tool addition.
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)

add_custom_target (perf_icgrep
    COMMAND python3 perf.py icgrep
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/../performance
    DEPENDS icgrep)

add_custom_target (viewer
    COMMAND python3 viewer.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/../performance)

add_custom_target (text_viewer
    COMMAND python3 viewer.py -t
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/../performance)

add_custom_target (xml_test
    WORKING_DIRECTORY ${QA_DIR}/xml
    COMMAND python3 run_test_suite.py "${BIN_DIR}/xml")

add_custom_target (json_test
    WORKING_DIRECTORY ${QA_DIR}/json
    COMMAND python3 json_test.py "${BIN_DIR}/json")

add_custom_target (json_perf
    WORKING_DIRECTORY ${QA_DIR}/json_perf
    COMMAND python3 json_perf_test.py "${BIN_DIR}/json")

add_custom_target (csv2json_test
  WORKING_DIRECTORY ${QA_DIR}/csv/csv2json
  COMMAND python3 csv2json_test.py "${BIN_DIR}/csv2json")

# Custom test targets run the tools directly, so each must depend on the tools
# it runs; otherwise e.g. `make greptest` tests whatever icgrep binary is already
# in the build directory, stale or absent.
foreach(TEST_AND_TOOL
        greptest:icgrep proptest:icgrep abc_test:icgrep
        u8u16_test:u8u16 u32u8_test:u32u8 editd_test:editd base64_test:base64
        gb18030_test:gb18030 nfd_test:nfd nfc_test:nfc
        xml_test:xml json_test:json json_perf:json csv2json_test:csv2json)
    string(REPLACE ":" ";" PAIR ${TEST_AND_TOOL})
    list(GET PAIR 0 TEST_TARGET)
    list(GET PAIR 1 TOOL_TARGET)
    if (TARGET ${TOOL_TARGET})
        add_dependencies(${TEST_TARGET} ${TOOL_TARGET})
    endif()
endforeach()


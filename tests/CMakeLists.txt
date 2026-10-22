#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#

# Place test executables in build/tests/bin
set(TEST_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIR})

# Add a new kernel-test target.
#
# Creates a new target with a given `NAME` from a source file with the same name
# (e.g., target `test0` from `test0.cpp`). Additional dependencies may be added
# after `NAME` and they will be linked with the resultant executable.
#
# Also adds a new test to `make check`.
function(parabix_add_kernel_test NAME)
    parabix_add_executable(NAME ${NAME} SRC ${NAME}.cpp DEPS testing ${ARGN})
    set(KERNEL_TEST_LIST ${KERNEL_TEST_LIST} ${TEST_BIN_DIR}/${NAME} PARENT_SCOPE)
    set(KERNEL_TEST_TARGET_LIST ${KERNEL_TEST_TARGET_LIST} ${NAME} PARENT_SCOPE)
    add_test(NAME "${NAME}"
        WORKING_DIRECTORY ${TEST_BIN_DIR}
        COMMAND ./${NAME})
endfunction(parabix_add_kernel_test)


###   Test Targets   ###
parabix_add_kernel_test(meta_tests kernel.streamutils)
parabix_add_kernel_test(test_scan kernel.basis kernel.scan kernel.unicode kernel.util)
parabix_add_kernel_test(test_bit_movement kernel.streamutils)
parabix_add_kernel_test(test_run_replace kernel.streamutils)
parabix_add_kernel_test(index_test kernel.streamutils)
parabix_add_kernel_test(nesting_tests kernel.util)
parabix_add_kernel_test(everynth_tests pablo.compiler)
parabix_add_kernel_test(test_repeatingstreamset kernel.io)
parabix_add_kernel_test(test_bitwise_kernels kernel.bitwise)
parabix_add_kernel_test(test_bytefilterandspread kernel.io kernel.streamutils)
parabix_add_kernel_test(test_emptyprogram kernel.io)
parabix_add_kernel_test(test_emptystreamset kernel.basis kernel.streamutils)
parabix_add_kernel_test(test_truncatedstreamset kernel.io)
parabix_add_kernel_test(test_sorting kernel.streamutils)
parabix_add_kernel_test(test_hex_decode kernel.util)

# ProvisionalLookAheadStride: compare provisional and normal processing of a lookahead chain with a reference
parabix_add_executable(NAME provisional_lookahead SRC provisional_lookahead.cpp
                       DEPS pablo.compiler kernel.basis kernel.io kernel.pipeline toolchain)
add_test(NAME provisional_lookahead_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/provisional_lookahead_test.py ${TEST_BIN_DIR}/provisional_lookahead)

# Clearing of a wide stream's output past its final produced item, checked with a reference
parabix_add_executable(NAME unwritten_wide_output SRC unwritten_wide_output.cpp
                       DEPS kernel.io kernel.pipeline toolchain)
add_test(NAME unwritten_wide_output_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/unwritten_wide_output_test.py ${TEST_BIN_DIR}/unwritten_wide_output)

# The preconditions of the regular expression engine and the passes of its goals (no pipeline compiled)
parabix_add_executable(NAME re_preconditions SRC re_preconditions.cpp
                       DEPS kernel.re re.parse re.unicode toolchain)
add_test(NAME re_preconditions_test
    WORKING_DIRECTORY ${TEST_BIN_DIR}
    COMMAND ./re_preconditions)

# Replacement patterns of search and replace and their analyses (no pipeline compiled)
parabix_add_executable(NAME re_replacement SRC re_replacement.cpp
                       DEPS re.parse re.analysis re.adt toolchain)
add_test(NAME re_replacement_test
    WORKING_DIRECTORY ${TEST_BIN_DIR}
    COMMAND ./re_replacement)

# The MatchStarts mode of the regular expression engine, checked with Python's re
parabix_add_executable(NAME match_starts SRC match_starts.cpp
                       DEPS kernel.re kernel.basis kernel.io kernel.pipeline kernel.streamutils kernel.util re.parse toolchain)
add_test(NAME match_starts_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/match_starts_test.py ${TEST_BIN_DIR}/match_starts)

# `make kernel-tests` to run all kernel tests
add_custom_target(kernel-tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND python3 run_kernel_tests.py ${KERNEL_TEST_LIST})

add_dependencies(kernel-tests ${KERNEL_TEST_TARGET_LIST})

# Build target for all kernel tests: does not run any tests
add_custom_target(kernel-test-suite DEPENDS ${KERNEL_TEST_TARGET_LIST})

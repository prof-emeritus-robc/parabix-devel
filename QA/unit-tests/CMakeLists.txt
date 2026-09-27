#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# Standalone unit tests.

parabix_add_executable(
NAME
    unicode_set_test
SRC
    unicode_set_test.cpp
DEPS
    unicode.core
)

add_test(
    NAME unicode_set_test
    COMMAND unicode_set_test)

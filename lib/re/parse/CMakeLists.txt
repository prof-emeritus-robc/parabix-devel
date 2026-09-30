#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: re.parse

parabix_add_module(
NAME
    re.parse
SRC
    BRE_parser.cpp
    ERE_parser.cpp
    fixed_string_parser.cpp
    GLOB_parser.cpp
    parser.cpp
    PCRE_parser.cpp
    Prosite_parser.cpp
DEPS
    re.adt
    ${Boost_LIBRARIES}
    unicode.data
)

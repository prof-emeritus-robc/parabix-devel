#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: ldml.transforms

parabix_add_module(
NAME
    ldml.transforms
SRC
    transform_rules.cpp
    transform_rules_parser.cpp
    transform_rules_printer.cpp
DEPS
    re.adt
    unicode.core
    slaballocator
)

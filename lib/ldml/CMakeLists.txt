#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: ldml.transforms

parabix_add_module(
NAME
    ldml.transforms
SRC
    charset_analysis.cpp
    transform_rules.cpp
    transform_rules_captures.cpp
    transform_rules_direction.cpp
    transform_rules_disambiguate.cpp
    transform_rules_exclusive.cpp
    transform_rules_overlap.cpp
    transform_rules_partition.cpp
    transform_rules_parser.cpp
    transform_rules_printer.cpp
    transform_registry.cpp
DEPS
    re.adt
    re.analysis
    re.unicode
    unicode.core
    slaballocator
)

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
    transform_rules_expand.cpp
    transform_rules_functions.cpp
    transform_rules_nfd.cpp
    transform_rules_overlap.cpp
    transform_rules_partition.cpp
    transform_rules_parser.cpp
    transform_rules_printer.cpp
    transform_rules_revisit.cpp
    transform_registry.cpp
    transform_plan.cpp
    shape_groups.cpp
    divcode_plan.cpp
DEPS
    re.adt
    re.analysis
    re.transforms
    re.unicode
    unicode.core
    unicode.algo
    unicode.data
    slaballocator
)

#  The CLDR data of the source tree (third_party/cldr, imported from CLDR by
#  scripts/CLDR/import_cldr.sh): the default transform directory.
target_compile_definitions(ldml.transforms PRIVATE
    PARABIX_SOURCE_CLDR_DIR="${PROJECT_SOURCE_DIR}/third_party/cldr")

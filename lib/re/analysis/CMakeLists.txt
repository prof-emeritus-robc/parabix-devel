#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: re.analysis

parabix_add_module(
NAME
    re.analysis
SRC
    capture-ref.cpp
    cc_sequence_search.cpp
    collect_ccs.cpp
    nullable.cpp
    re_analysis.cpp
    re_inspector.cpp
    re_name_gather.cpp
    validation.cpp
DEPS
    re.adt
    re.printer
    unicode.utf
)

#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: ldml

parabix_add_executable(
NAME
    ldml_trules
SRC
    ldml_trules.cpp
DEPS
    ldml.transforms
    re.analysis
    re.transforms
    re.unicode
)

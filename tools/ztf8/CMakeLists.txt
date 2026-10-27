#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: ztf8

parabix_add_executable(
NAME
    matchprior
SRC
    matchprior.cpp
DEPS
    fileselect
    kernel.basis
    kernel.io
)

parabix_add_pablo_src(ztf1.pablosrc ztf1.pablo)

parabix_add_executable(
NAME
    ztf1
SRC
    ztf1.cpp
DEPS
    pablo.parse
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    toolchain
)

add_dependencies(ztf1 ztf1.pablosrc)


parabix_add_executable(
NAME
    ztf-runs
SRC
    ztf-logic.cpp
    ztf-runs.cpp
DEPS
    re.unicode
    unicode.utf
    pablo.compiler
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    toolchain
)

parabix_add_executable(
NAME
    ztf-hash
SRC
    ztf-logic.cpp
    ztf-scan.cpp
    ztf-hash.cpp
DEPS
    grep
    pablo.compiler
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.util
    toolchain
)

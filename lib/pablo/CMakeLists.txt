#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: pablo

parabix_add_module(
NAME
    pablo
SRC
    arithmetic.cpp
    branch.cpp
    builder.cpp
    carry_manager.cpp
    codegenstate.cpp
    codemotionpass.cpp
    compressed_carry_manager.cpp
    distributivepass.cpp
    flattenif.cpp
    pabloAST.cpp
    # pablo_automultiplexing.cpp # TODO: use source variable
    pablo_compiler.cpp
    pablo_illustratorpass.cpp
    pablo_kernel.cpp
    pablo_simplifier.cpp
    pablo_toolchain.cpp
    pabloverifier.cpp
    passes.cpp
    printer_pablos.cpp
    schedulingprepass.cpp
    symbol_generator.cpp
DEPS
    kernel.core
    idisa
    toolchain
    slaballocator
)

#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: pablo.compiler
#
# PabloKernel/PabloBlock/PabloBuilder/PabloCompiler: the Pablo AST node types
# (ast/), the optimization passes that rewrite it (passes/), and the
# PabloKernel/PabloCompiler machinery that drives those passes and lowers
# the result to LLVM IR (this directory) -- one build target, because
# PabloBlock/PabloBuilder's node-creation methods call back into PabloKernel
# (which interns names/constants/LLVM types for the AST it owns), and
# PabloKernel::generateKernel() calls pablo_function_passes() in passes/,
# which itself operates on PabloKernel/PabloBlock. That's a genuine circular
# dependency in the existing logic, not just a file-layout accident, so it
# can't be split into separate libraries without introducing an abstract
# interface purely to satisfy the module boundary -- not worth the added
# indirection in Pablo's hottest code path for no real design benefit.
# pablo.toolchain (this module's only pablo-internal dependency) is a
# genuine leaf and lives as its own module.

parabix_add_module(
NAME
    pablo.compiler
SRC
    ../ast/arithmetic.cpp
    ../ast/branch.cpp
    ../ast/builder.cpp
    ../ast/codegenstate.cpp
    ../ast/pabloAST.cpp
    ../ast/symbol_generator.cpp
    carry_manager.cpp
    compressed_carry_manager.cpp
    pablo_compiler.cpp
    pablo_illustratorpass.cpp
    pablo_kernel.cpp
    pabloverifier.cpp
    printer_pablos.cpp
    ../passes/codemotionpass.cpp
    ../passes/distributivepass.cpp
    ../passes/flattenif.cpp
    ../passes/pablo_simplifier.cpp
    ../passes/passes.cpp
    ../passes/schedulingprepass.cpp
DEPS
    pablo.toolchain
    kernel.core
    idisa
    kernel.illustrator
    toolchain
    slaballocator
)

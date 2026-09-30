/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

// Umbrella header for authoring Pablo kernels: pulls in the AST node
// vocabulary (pablo/ast/pe_*.h, ps_*.h, boolean.h), PabloBuilder, and
// PabloKernel, so most consumers can write a single
//   #include <pablo/pablo.h>
// instead of hunting down the individual headers they need. Consumers
// needing something more specialized (pablo/parse/*, pablo/bixnum/*, or an
// internal-only header such as pablo/compiler/carry_manager.h) still
// include those directly.

#include <pablo/ast/arithmetic.h>
#include <pablo/ast/boolean.h>
#include <pablo/ast/branch.h>
#include <pablo/ast/builder.hpp>
#include <pablo/ast/codegenstate.h>
#include <pablo/ast/pabloAST.h>
#include <pablo/ast/pe_advance.h>
#include <pablo/ast/pe_constant.h>
#include <pablo/ast/pe_count.h>
#include <pablo/ast/pe_debugprint.h>
#include <pablo/ast/pe_everynth.h>
#include <pablo/ast/pe_illustrator.h>
#include <pablo/ast/pe_infile.h>
#include <pablo/ast/pe_integer.h>
#include <pablo/ast/pe_lookahead.h>
#include <pablo/ast/pe_matchstar.h>
#include <pablo/ast/pe_ones.h>
#include <pablo/ast/pe_pack.h>
#include <pablo/ast/pe_repeat.h>
#include <pablo/ast/pe_scanthru.h>
#include <pablo/ast/pe_string.h>
#include <pablo/ast/pe_var.h>
#include <pablo/ast/pe_zeroes.h>
#include <pablo/ast/ps_assign.h>
#include <pablo/ast/ps_terminate.h>
#include <pablo/ast/symbol_generator.h>
#include <pablo/compiler/pablo_kernel.h>

/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

namespace llvm { class raw_ostream; }

namespace pablo {

class PabloKernel;
class PabloBlock;
class Statement;
class PabloAST;

class PabloPrinter {
public:
    static void print(const PabloKernel * kernel, llvm::raw_ostream & out);
    static void print(const PabloAST * expr, llvm::raw_ostream & out);
    static void print(const PabloBlock * block, llvm::raw_ostream & strm, const bool expandNested = false, const unsigned indent = 0);
    static void print(const Statement * stmt, llvm::raw_ostream & out, const bool expandNested = false, const unsigned indent = 0);
};

}


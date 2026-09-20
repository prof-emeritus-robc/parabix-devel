/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <pablo/parse/pablo_source_kernel.h>

#include <llvm/Support/raw_ostream.h>
#include <pablo/compiler/printer_pablos.h>
#include <pablo/parse/error.h>
#include <pablo/parse/pablo_parser.h>
#include <pablo/parse/source_file.h>

namespace pablo {

void PabloSourceKernel::generatePabloMethod() {
    bool parseSuccessful = mParser->parseKernel(mSource, this, mKernelName);
    if (!parseSuccessful) {
        auto em = mParser->getErrorManager();
        em->dumpErrors();
        llvm::report_fatal_error("parse error", false);
    }
}

PabloSourceKernel::PabloSourceKernel(LLVMTypeSystemInterface & ts,
                                     std::shared_ptr<parse::PabloParser> & parser,
                                     std::shared_ptr<parse::SourceFile> & sourceFile,
                                     std::string const & kernelName,
                                     kernel::Bindings inputStreamBindings,
                                     kernel::Bindings outputStreamBindings,
                                     kernel::Bindings inputScalarBindings,
                                     kernel::Bindings outputScalarBindings)
: PabloKernel(ts, std::string(kernelName), inputStreamBindings, outputStreamBindings, inputScalarBindings, outputScalarBindings)
, mParser(parser)
, mSource(sourceFile)
, mKernelName(kernelName)
{}

PabloSourceKernel::PabloSourceKernel(LLVMTypeSystemInterface & ts,
                                     std::shared_ptr<parse::PabloParser>  & parser,
                                     std::string const & sourceFile,
                                     std::string const & kernelName,
                                     kernel::Bindings inputStreamBindings,
                                     kernel::Bindings outputStreamBindings,
                                     kernel::Bindings inputScalarBindings,
                                     kernel::Bindings outputScalarBindings)
: PabloKernel(ts, std::string(kernelName), inputStreamBindings, outputStreamBindings, inputScalarBindings, outputScalarBindings)
, mParser(parser)
, mSource(new parse::SourceFile(sourceFile))
, mKernelName(kernelName)
{}

} // namespace pablo

/*
 * Part of the Parabix Project, under the Open Software License 3.0.
 * SPDX-License-Identifier: OSL-3.0
 */

#include <llvm/Support/raw_ostream.h>
#include <testing/macro.h>
#include <testing/runtime.h>

namespace testing {

namespace cli {

using namespace llvm;

bool SuppressCLIOutput;

static cl::OptionCategory testingFlags("Unit Test Flags");

cl::OptionCategory * testFlags() {
    return &testingFlags;
}

static cl::opt<bool, true> SuppressCLIOutputOption("suppress-output", cl::location(SuppressCLIOutput), cl::init(false),
    cl::desc("Don't display any text to stdout"), cl::cat(testingFlags));

bool ExtendedTesting;

static cl::opt<bool, true> ExtendedTestingOption("extended-testing", cl::location(ExtendedTesting), cl::init(false),
    cl::desc("Also run the test cases marked as extended"), cl::cat(testingFlags));

}

int32_t RunTestSuite(InvocationList list) {
    bool allTestsPassed = true;
    int failCount = 0;
    int runCount = 0;
    for (auto const & elem : list) {
        if (std::get<2>(elem) && !cli::ExtendedTesting) {
            continue;
        }
        runCount++;
        if (!cli::SuppressCLIOutput) {
            llvm::outs() << " test " << std::get<0>(elem) << "... ";
        }
        auto fn = std::get<1>(elem);
        int32_t result = fn();
        allTestsPassed = allTestsPassed && result == 0;
        if (!cli::SuppressCLIOutput) {
            if (result == 0) {
                llvm::outs().changeColor(llvm::outs().GREEN) << "ok\n";
                llvm::outs().resetColor();
            } else {
                failCount++;
                llvm::outs().changeColor(llvm::outs().RED) << "fail\n";
                llvm::outs().resetColor();
            }
        }
    }
    if (!cli::SuppressCLIOutput) {
        if (allTestsPassed) {
            llvm::outs().changeColor(llvm::outs().GREEN) << " passed ";
            llvm::outs().resetColor();
            llvm::outs() << "all tests\n";
        } else {
            llvm::outs().changeColor(llvm::outs().RED) << " failed ";
            llvm::outs().resetColor();
            llvm::outs() << failCount << "/" << runCount << " tests\n";
        }
    }
    return allTestsPassed ? 0 : 1;
}

}

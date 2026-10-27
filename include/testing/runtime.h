/*
 * Part of the Parabix Project, under the Open Software License 3.0.
 * SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <tuple>
#include <vector>
#include <llvm/Support/CommandLine.h>

namespace testing {

namespace cli {

llvm::cl::OptionCategory * testFlags();

extern bool SuppressCLIOutput;

extern bool ExtendedTesting;

}

using TestCaseInvocationType = int32_t(*)();

// The name of a test case, its invocation and whether it runs only with --extended-testing.
using InvocationList = std::vector<std::tuple<std::string, TestCaseInvocationType, bool>>;

int32_t RunTestSuite(InvocationList list);

}

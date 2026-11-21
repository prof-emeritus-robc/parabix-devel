#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The mode of the tconv regression tests (QA/tconv_*_test.py), imported by
#  each before it reads its arguments.
#
#  By default a test is reduced to finish within about 5 s with a cold object
#  cache (as at the start of make check, NFD and NFC kernels aside) and 1 s
#  with a warm one: the tconv runs compile with -backend-optimization-level=none
#  (most of a cold run is the back end compiling the pipeline), and the larger
#  tests run a selection of their cases.  With --extended-testing (removed
#  from sys.argv here), the tests run all their cases at the default
#  optimization level.
#
#  The level is passed through TEST_FLAGS, which every Parabix tool appends to
#  its arguments (codegen::ParseCommandLineOptions), so that it reaches the
#  tconv runs without changing their command lines.  A level already in
#  TEST_FLAGS is kept.

import os
import sys

EXTENDED = "--extended-testing" in sys.argv
if EXTENDED:
    sys.argv = [a for a in sys.argv if a != "--extended-testing"]
else:
    flags = os.environ.get("TEST_FLAGS", "")
    if "-backend-optimization-level" not in flags:
        os.environ["TEST_FLAGS"] = (flags + " -backend-optimization-level=none").strip()

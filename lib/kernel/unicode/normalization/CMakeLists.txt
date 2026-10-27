#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: kernel.unicode.normalization
#
# Unicode NFD/NFC/NFKD normalization kernels. Split out of kernel.unicode:
# unlike the rest of that module (basic UTF encode/decode, char-class/
# boundary kernels), building a normalization pipeline genuinely requires
# UTF decode/encode and character-class testing as pipeline stages, so this
# depends on kernel.unicode (one-directional -- nothing in kernel.unicode
# depends back on this).

parabix_add_module(
NAME
    kernel.unicode.normalization
SRC
    fcd.cpp
    nfc.cpp
    nfd.cpp
    normalization-generated.cpp
    normalization.cpp
DEPS
    kernel.unicode
# kernel.unicode (and the kernel modules it links) are used only to build pipelines from
# their kernels; these are the modules the normalization kernels' generation uses.
CODEGEN_DEPS
    re.unicode
    re.cc
    pablo.compiler
    pablo.bixnum
    unicode.algo
    unicode.data
)

#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: kernel.core

parabix_add_module(
NAME    
    kernel.core
SRC
    attributes.cpp
    binding.cpp
    block_kernel.cpp
    callback.cpp
    idisa_target.cpp
    kernel_builder.cpp
    kernel_compiler.cpp
    block_kernel_compiler.cpp
    kernel.cpp
    multiblock_kernel.cpp
    processing_rate.cpp
    relationship.cpp
    streamset.cpp
DEPS
    idisa
    kernel.illustrator
    slaballocator
)

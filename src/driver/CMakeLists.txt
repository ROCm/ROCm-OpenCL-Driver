################################################################################
##
## The University of Illinois/NCSA
## Open Source License (NCSA)
##
## Copyright (c) 2016, Advanced Micro Devices, Inc. All rights reserved.
##
## Developed by:
##
##                 AMD Research and AMD HSA Software Development
##
##                 Advanced Micro Devices, Inc.
##
##                 www.amd.com
##
## Permission is hereby granted, free of charge, to any person obtaining a copy
## of this software and associated documentation files (the "Software"), to
## deal with the Software without restriction, including without limitation
## the rights to use, copy, modify, merge, publish, distribute, sublicense,
## and#or sell copies of the Software, and to permit persons to whom the
## Software is furnished to do so, subject to the following conditions:
##
##  - Redistributions of source code must retain the above copyright notice,
##    this list of conditions and the following disclaimers.
##  - Redistributions in binary form must reproduce the above copyright
##    notice, this list of conditions and the following disclaimers in
##    the documentation and#or other materials provided with the distribution.
##  - Neither the names of Advanced Micro Devices, Inc,
##    nor the names of its contributors may be used to endorse or promote
##    products derived from this Software without specific prior written
##    permission.
##
## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
## IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
## FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
## THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
## OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
## ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
## DEALINGS WITH THE SOFTWARE.
##
################################################################################

file(GLOB sources
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/*.h
)

include_directories(${LLVM_INCLUDE_DIR})
include_directories(${LLVM_CONFIG_INCLUDE_DIR})
include_directories(${LLVM_MAIN_INCLUDE_DIR})
include_directories(${LLVM_INCLUDE_DIR}/llvm/Target/AMDGPU)
include_directories(${LLD_INCLUDE_DIR})

link_directories(${LLVM_LIBRARY_DIRS})
add_library(opencl_driver ${sources})
set_target_properties(opencl_driver PROPERTIES POSITION_INDEPENDENT_CODE ON)

llvm_map_components_to_libnames(llvm_libs
  AllTargetsAsmPrinters
  AllTargetsDescs
  AllTargetsDisassemblers
  AllTargetsInfos
  CodeGen
  MC
  MCDisassembler
  Object
  Symbolize
  Core
  Option
  Support
  AMDGPUCodeGen
  AMDGPUAsmParser
  )

if (WIN32)
target_link_libraries(opencl_driver version)
endif (WIN32)

target_link_libraries(opencl_driver
  clangDriver
  clangFrontend
  clangEdit
  clangLex
  clangBasic
  clangCodeGen
  lldELF
  lldCore
  LLVMDebugInfoDWARF
)
target_link_libraries(opencl_driver ${llvm_libs})
target_include_directories(opencl_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS opencl_driver DESTINATION lib)

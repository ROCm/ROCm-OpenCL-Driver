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

cmake_minimum_required(VERSION 2.6)

file(GLOB sources
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cl
)

set(CL $<TARGET_FILE:roc-cl>)
set(LLVMBIN ${LLVM_TOOLS_BINARY_DIR})

macro(roc_cl_test name)
  string(REGEX REPLACE ":" "_" dir ${CMAKE_CURRENT_BINARY_DIR}/${name})
  file(MAKE_DIRECTORY ${dir})
  add_test(
    NAME ${name}
    COMMAND ${CL} -llvmbin ${LLVMBIN} ${ARGN}
    WORKING_DIRECTORY ${dir}
  )
endmacro()

set(SIMPLE ${CMAKE_CURRENT_SOURCE_DIR}/simple.cl)
roc_cl_test(simple:compile_to_llvm -compile_to_llvm ${SIMPLE} -o out.bc)
roc_cl_test(simple:compile_to_llvm-g -compile_to_llvm ${SIMPLE} -g -o out.bc)
roc_cl_test(simple:compile_to_llvm-fembed-bitcode -compile_to_llvm ${SIMPLE} -fembed-bitcode -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-opt-disable -compile_to_llvm ${SIMPLE} -cl-opt-disable -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-strict-aliasing -compile_to_llvm ${SIMPLE} -cl-strict-aliasing -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-single-precision-constant -compile_to_llvm ${SIMPLE} -cl-single-precision-constant -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-finite-math-only -compile_to_llvm ${SIMPLE} -cl-finite-math-only -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-kernel-arg-info -compile_to_llvm ${SIMPLE} -cl-kernel-arg-info -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-unsafe-math-optimizations -compile_to_llvm ${SIMPLE} -cl-unsafe-math-optimizations -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-fast-relaxed-math -compile_to_llvm ${SIMPLE} -cl-fast-relaxed-math -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-mad-enable -compile_to_llvm ${SIMPLE} -cl-mad-enable -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-no-signed-zeros -compile_to_llvm ${SIMPLE} -cl-no-signed-zeros -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-std-1.2 -compile_to_llvm ${SIMPLE} -cl-std=CL1.2 -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-std-2.0 -compile_to_llvm ${SIMPLE} -cl-std=CL2.0 -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-denorms-are-zero -compile_to_llvm ${SIMPLE} -cl-denorms-are-zero -o out.bc)
roc_cl_test(simple:compile_to_llvm-cl-fp32-correctly-rounded-divide-sqrt -compile_to_llvm ${SIMPLE} -cl-fp32-correctly-rounded-divide-sqrt -o out.bc)
roc_cl_test(simple:compile_and_link -compile_and_link ${SIMPLE} -o out.co)

set(EXTERN_FUNCTION ${CMAKE_CURRENT_SOURCE_DIR}/extern_function1.cl ${CMAKE_CURRENT_SOURCE_DIR}/extern_function2.cl)
roc_cl_test(extern_function:compile_to_llvm -compile_to_llvm ${EXTERN_FUNCTION} -o out.bc -cl-std=CL1.2)
roc_cl_test(extern_function:compile_and_link -compile_and_link ${EXTERN_FUNCTION} -o out.co -cl-std=CL1.2)

set(DEFINED ${CMAKE_CURRENT_SOURCE_DIR}/defined.cl)
roc_cl_test(defined:compile_to_llvm ${DEFINED} -compile_to_llvm -DDEF=10 -o out.co)
roc_cl_test(defined:compile_and_link ${DEFINED} -compile_and_link -DDEF=10 -o out.co)

set(INCLUDER ${CMAKE_CURRENT_SOURCE_DIR}/includer.cl)
set(INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
roc_cl_test(include-I1:compile_to_llvm -compile_to_llvm ${INCLUDER} -I${INCLUDE} -o out.bc)
roc_cl_test(include-I1:compile_and_link -compile_and_link ${INCLUDER} -I${INCLUDE} -o out.co)
#roc_cl_test(include-I2:compile_to_llvm -compile_to_llvm ${INCLUDER} -I ${INCLUDE} -o out.bc)
#oc_cl_test(include-I2:compile_and_link -compile_and_link ${INCLUDER} -I ${INCLUDE} -o out.co)

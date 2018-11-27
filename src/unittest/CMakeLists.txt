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

find_package(Threads REQUIRED)
include(ExternalProject)

ExternalProject_Add(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest
    TIMEOUT 10
    CMAKE_ARGS
      -DBUILD_GTEST=ON -DBUILD_GMOCK=OFF
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
      -Dgtest_force_shared_crt=ON
    # Disable install step
    INSTALL_COMMAND ""
    # Wrap download, configure and build steps in a script to log output
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON)

ExternalProject_Get_Property(googletest source_dir)
include_directories(${source_dir}/googletest/include)
ExternalProject_Get_Property(googletest binary_dir)
link_directories(${binary_dir}/lib)

#include(${GTEST_INCLUDE_DIRS})
file(GLOB sources ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

link_directories(${LLVM_LIBRARY_DIRS})
add_executable(roc-cl-unittest ${sources})
add_dependencies(roc-cl-unittest googletest)
target_link_libraries(roc-cl-unittest opencl_driver)
target_link_libraries(roc-cl-unittest gtest gtest_main)
target_link_libraries(roc-cl-unittest ${CMAKE_THREAD_LIBS_INIT})

add_test(NAME roc-cl-unittest
         COMMAND $<TARGET_FILE:roc-cl-unittest>)

set_property(TEST roc-cl-unittest PROPERTY ENVIRONMENT
             "LLVM_BIN=${LLVM_BINARY_DIR}/bin;TEST_DIR=${CMAKE_SOURCE_DIR}/src/test")

## OVERVIEW

ROCm OpenCL Driver is a thin wrapper over Clang Driver API. It provides C++ interface to compilation services.

## BUILDING

This project requires reasonably recent LLVM/Clang build (April 2016 trunk).

Use out-of-source CMake build and create separate directory to run CMake.

The following build steps are performed:

    mkdir -p build
    cd build
    export LLVM_DIR=... (path to LLVM dist)
    cmake -DLLVM_DIR=$LLVM_DIR ..
    make
    make test

This assumes that LLVM_DIR points to dist directory, so we recommend to make sure LLVM is configured
with -DCMAKE_INSTALL_PREFIX=dist and 'make install' is run.

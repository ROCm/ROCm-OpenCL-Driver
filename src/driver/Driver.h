#ifndef AMD_COMPILER_DRIVER_H
#define AMD_COMPILER_DRIVER_H

#include <string>
#include <vector>
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Triple.h"

namespace clang {
  class DiagnosticsEngine;
  namespace driver {
    class Driver;
  }
}

namespace amd {

class CompilerDriver {
public:

  CompilerDriver();

  virtual ~CompilerDriver() {}

  virtual bool Init(llvm::StringRef ClangExecutable = "") = 0;

  virtual bool Fini();

  virtual int Build(llvm::ArrayRef<const char *> args) = 0;

protected:

  void setTriple(const llvm::Triple &trpl) { triple = trpl; }

  void setTriple(const llvm::Triple::ArchType &arch,
                 const llvm::Triple::VendorType &vendor,
                 const llvm::Triple::OSType &os);

  llvm::Triple triple;

  clang::driver::Driver *clangDriver;

  clang::DiagnosticsEngine *diags;

  llvm::StringRef clangExe;
};

class OpenCLDriver : public CompilerDriver {
public:

  OpenCLDriver();

  virtual ~OpenCLDriver() {}

  virtual bool Init(llvm::StringRef ClangExecutable);

  virtual int Build(llvm::ArrayRef<const char *> args);

};

}

#endif

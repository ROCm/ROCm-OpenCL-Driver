#include <string>
#include "Driver.h"
#include "llvm/ADT/SmallVector.h"

using namespace amd;
using namespace llvm;

int main(int argc, char* argv[])
{
  OpenCLDriver theDriver;
  // the first argument is clang executable path (internal Clang's Driver implementation limitation)
  // ToDo: get rid of passing clang path as an argument
  if (!theDriver.Init(argv[1])) {
    return 1;
  }
  SmallVector<const char *, 16> args(argv+1, argv + argc);
  args.push_back("-Dcl_clang_storage_class_specifiers");
  args.push_back("-v");
  // Building from OpenCL to AMDHSA CO via AMDGPU backend
  int Res = theDriver.Build(args);

  return Res;
}

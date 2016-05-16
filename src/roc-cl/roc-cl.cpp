#include <string>
#include "Driver.h"
#include "llvm/ADT/SmallVector.h"

using namespace amd;
using namespace llvm;

int main(int argc, char* argv[])
{
  std::string path = argv[1];
  OpenCLDriver theDriver;
  if (!theDriver.Init(path)) {
    return 1;
  }

  using namespace std;
  SmallVector<const char *, 16> args;
  args.push_back(argv[1]);
  args.push_back(argv[2]);
  args.push_back("-g");
//  args.push_back("--save-temps");
  string device = "-mcpu=";
  device.append(argv[3]);
  args.push_back(device.c_str());
  args.push_back("-Dcl_clang_storage_class_specifiers");
  args.push_back("-v");

  int Res = theDriver.Build(args);

  return Res;
}

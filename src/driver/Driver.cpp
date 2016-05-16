#include "Driver.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/ManagedStatic.h"

#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"

// implicitly needed
#include "clang/Basic/VirtualFileSystem.h"
// #include "clang/Tooling/Tooling.h"

using namespace llvm;
using namespace clang;
using namespace amd;

CompilerDriver::CompilerDriver() {
}

void CompilerDriver::setTriple(const Triple::ArchType &arch,
                       const Triple::VendorType &vendor,
                       const Triple::OSType &os) {
  triple.setArch(arch);
  triple.setVendor(vendor);
  triple.setOS(os);
  //  triple.setEnvironment(llvm::Triple::EnvironmentType::AMDOpenCL);
  //  triple.setObjectFormat(llvm::Triple::ObjectFormatType::ELF);
}

bool CompilerDriver::Fini() {
  delete clangDriver;
  llvm_shutdown();
  return true;
}

bool OpenCLDriver::Init(StringRef ClangExecutable) {
  clangExe = ClangExecutable;
  using namespace clang::driver;
  if (clangExe.empty()) {
    // ToDo: find clang
  }
  if (clangExe.empty()) {
    return false;
  }

  setTriple(Triple::ArchType::amdgcn, Triple::VendorType::AMD, Triple::OSType::AMDHSA);

  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts = new DiagnosticOptions();
  TextDiagnosticPrinter *diagClient = new TextDiagnosticPrinter(llvm::errs(), &*diagOpts);
  IntrusiveRefCntPtr<DiagnosticIDs> diagID(new DiagnosticIDs());
  DiagnosticsEngine diags(diagID, &*diagOpts, diagClient);

  clangDriver = new Driver(clangExe, triple.str(), diags);

  return true;
}

OpenCLDriver::OpenCLDriver() : CompilerDriver() {}

int OpenCLDriver::Build(ArrayRef<const char *> args) {
  using namespace clang::driver;

  clangDriver->setTitle("AMDGPU OpenCL driver");
  clangDriver->setCheckInputsExist(false);

  std::unique_ptr<Compilation> C(clangDriver->BuildCompilation(args));

  int Res = 0;
  SmallVector<std::pair<int, const Command *>, 4> failingCommands;
  if (C.get()) {
    Res = clangDriver->ExecuteCompilation(*C, failingCommands);
  }

  for (const auto &P : failingCommands) {
    int CommandRes = P.first;
    const Command *failingCommand = P.second;
    if (!Res)
      Res = CommandRes;
    // If result status is < 0, then the driver command signalled an error.
    // If result status is 70, then the driver command reported a fatal error.
    // On Windows, abort will return an exit code of 3.  In these cases,
    // generate additional diagnostic information if possible.
    bool DiagnoseCrash = CommandRes < 0 || CommandRes == 70;
#ifdef LLVM_ON_WIN32
    DiagnoseCrash |= CommandRes == 3;
#endif
    if (DiagnoseCrash) {
      clangDriver->generateCompilationDiagnostics(*C, *failingCommand);
      break;
    }
  }

  clangDriver->PrintActions(*C);


//  const DiagnosticsEngine &diags = clangDriver->getDiags();
//  DiagnosticConsumer *diagCons = const_cast<DiagnosticConsumer*>(diags.getClient());
// failing here:
//  diagCons->finish();

#ifdef LLVM_ON_WIN32
  // Exit status should not be negative on Win32, unless abnormal termination.
  // Once abnormal termiation was caught, negative status should not be
  // propagated.
  if (Res < 0)
    Res = 1;
#endif

  return Res;
}

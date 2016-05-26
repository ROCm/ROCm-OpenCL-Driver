#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#include "AmdCompiler.h"

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

#ifdef _WIN32
#include "windows.h"
#include "io.h"
#else
#include <stdio.h>
#include <stdlib.h>
#endif

using namespace llvm;
using namespace clang;
using namespace clang::driver;

namespace amd {

class AMDGPUCompiler : public Compiler {
private:
  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts;
  TextDiagnosticPrinter* diagClient;
  IntrusiveRefCntPtr<DiagnosticIDs> diagID;
  DiagnosticsEngine diags;
  std::unique_ptr<Driver> driver;
  std::vector<Data*> datas;
  std::string llvmLinkExe;

  template <typename T>
  inline T* AddData(T* d) { datas.push_back(d); return d; }
  void AddCommonArgs(std::vector<const char*>& args);
  bool InvokeDriver(ArrayRef<const char*> args);
  bool InvokeLLVMLink(ArrayRef<const char*> args);

  File* ToInputFile(Data* input);
  File* ToOutputFile(Data* output);

  bool CompileToLLVMBitcode(Data* input, Data* output, const std::vector<std::string>& options);
  bool CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options);

public:
  AMDGPUCompiler(const std::string& llvmBin);

  std::string Output() override { return std::string(); }

  File* NewInputFile(DataType type, const std::string& path) override;

  File* NewOutputFile(DataType type, const std::string& path) override;

  File* NewTempFile(DataType type, const char* ext) override;

  Data* NewBufferReference(DataType type, const char* ptr, size_t size) override;

  Data* NewBuffer(DataType type) override;

  bool CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  bool LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  bool CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;
};

void AMDGPUCompiler::AddCommonArgs(std::vector<const char*>& args)
{
  args.push_back("-x cl");
  args.push_back("-Xclang"); args.push_back("-cl-std=CL1.2");
  args.push_back("-v");
}

AMDGPUCompiler::AMDGPUCompiler(const std::string& llvmBin)
  : diagOpts(new DiagnosticOptions()),
    diagClient(new TextDiagnosticPrinter(llvm::errs(), &*diagOpts)),
    diags(diagID, &*diagOpts, &*diagClient),
    driver(new Driver(llvmBin + "/clang", "amdgcn-amd-amdhsa", diags)),
    llvmLinkExe(llvmBin + "/llvm-link")
{
  driver->setTitle("AMDGPU OpenCL driver");
  driver->setCheckInputsExist(false);
}

bool AMDGPUCompiler::InvokeDriver(ArrayRef<const char*> args)
{
  errs() << "All arguments: ";
  for (const char* arg : args) {
    errs() << "\"" << arg << "\" ";
  }
  errs() << "\n";
  std::unique_ptr<Compilation> C(driver->BuildCompilation(args));

  int Res = 0;
  SmallVector<std::pair<int, const Command *>, 4> failingCommands;
  if (C.get()) {
    Res = driver->ExecuteCompilation(*C, failingCommands);
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
      driver->generateCompilationDiagnostics(*C, *failingCommand);
      break;
    }
  }

  driver->PrintActions(*C);

//  const DiagnosticsEngine &diags = driver->getDiags();
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

  return Res == 0;
}

bool AMDGPUCompiler::InvokeLLVMLink(ArrayRef<const char*> args)
{
  SmallVector<const char*, 128> args1;
  args1.push_back(llvmLinkExe.c_str());
  args1.push_back("-v");
  for (const char *arg : args) { args1.push_back(arg); }
  args1.push_back(nullptr);
  errs() << "All arguments: ";
  errs() << llvmLinkExe << "\n";
  for (const char* arg : args1) {
    if (arg) {
      errs() << "\"" << arg << "\" ";
    }
  }
  errs() << "\n\n";

  int res = llvm::sys::ExecuteAndWait(llvmLinkExe, args1.data());
  if (res != 0) {
    return false;
  }
  return true;
}

File* AMDGPUCompiler::ToInputFile(Data* input)
{
  // TODO: convert arbitrary Data to temporary file if needed.
  return (File*) input;
}

File* AMDGPUCompiler::ToOutputFile(Data* output)
{
  // TODO: convert arbitrary Data to temporary file if needed.
  return (File*) output;
}

File* AMDGPUCompiler::NewInputFile(DataType type, const std::string& path)
{
  return AddData(new File(type, path, true));
}

File* AMDGPUCompiler::NewOutputFile(DataType type, const std::string& path)
{
  return AddData(new File(type, path, false));
}

File* AMDGPUCompiler::NewTempFile(DataType type, const char* ext)
{
  static int counter = 1;
  static char templ[] = "AMD_tmp_XXXXXX";
  char fname[15];
  strcpy(fname, templ);
  mktemp(fname);
  std::string name(fname);
  name += "_";
  name += std::to_string((counter++));
  name += ".";
  name += ext;

#ifdef _WIN32
  char buf[MAX_PATH];
  if (GetTempPath(MAX_PATH, buf) != 0) {
    name = std::string(buf) + name;
  }
#else
  name += "/tmp/" + name;
#endif

  return AddData(new File(type, name, true));
}

Data* AMDGPUCompiler::NewBufferReference(DataType type, const char* ptr, size_t size)
{
  return 0;
}

Data* AMDGPUCompiler::NewBuffer(DataType type)
{
  return 0;
}

bool AMDGPUCompiler::CompileToLLVMBitcode(Data* input, Data* output, const std::vector<std::string>& options)
{
  std::vector<const char*> args;

  AddCommonArgs(args);
  args.push_back("-c");
  args.push_back("-emit-llvm");

  File* inputFile = ToInputFile(input);
  args.push_back(inputFile->Name().c_str());

  File* bcFile = ToOutputFile(output);

  args.push_back("-o"); args.push_back(bcFile->Name().c_str());

  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }

  return InvokeDriver(args);
}

const std::vector<std::string> emptyOptions;

bool AMDGPUCompiler::CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options)
{
  if (inputs.size() == 1) {
    return CompileToLLVMBitcode(inputs[0], output, options);
  } else {
    std::vector<Data*> bcFiles;
    for (Data* input : inputs) {
      File* bcFile = NewTempFile(DT_LLVM_BC, "bc");
      if (!CompileToLLVMBitcode(input, bcFile, options)) { return false; }
      bcFiles.push_back(bcFile);
    }
    return LinkLLVMBitcode(bcFiles, output, emptyOptions);
  }
}

bool AMDGPUCompiler::LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options)
{
  std::vector<const char*> args;
  for (Data* input : inputs) {
    File* inputFile = ToInputFile(input);
    args.push_back(inputFile->Name().c_str());
  }
  File* outputFile = ToOutputFile(output);
  args.push_back("-o"); args.push_back(outputFile->Name().c_str());
  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }

  return InvokeLLVMLink(args);
}

bool AMDGPUCompiler::CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options)
{
  std::vector<const char*> args;

  AddCommonArgs(args);

  File* inputFile = ToInputFile(input);
  args.push_back(inputFile->Name().c_str());

  File* outputFile = ToOutputFile(output);

  args.push_back("-o"); args.push_back(outputFile->Name().c_str());

  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }


  return InvokeDriver(args);
}

bool AMDGPUCompiler::CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options)
{
  if (inputs.size() == 1) {
    return CompileAndLinkExecutable(inputs[0], output, options);
  } else {
    File* bcFile = NewTempFile(DT_LLVM_BC, "bc");
    if (!CompileToLLVMBitcode(inputs, bcFile, emptyOptions)) { return false; }
    return CompileAndLinkExecutable(bcFile, output, options);
  }
}

Compiler* CompilerDriver::CreateAMDGPUCompiler(const std::string& llvmBin)
{
  return new AMDGPUCompiler(llvmBin);
}

}

#include "AmdCompiler.h"
#include <cstdio>
#include <fstream>
#include <cstdlib>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"

#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"

// implicitly needed
#include "clang/Basic/VirtualFileSystem.h"
// #include "clang/Tooling/Tooling.h"

#include "AMDGPU.h"
#include "Disassembler/CodeObjectDisassembler.h"

#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Object/Binary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

// in-process headers
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Bitcode/BitcodeWriter.h"

#include <sstream>
#include <iostream>

#ifdef _WIN32
#define NODRAWTEXT // avoids #define of DT_INTERNAL
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#define QUOTE(s) #s
#define STRING(s) QUOTE(s)
#ifndef AMDGCN_TRIPLE
#define AMDGCN_TRIPLE amdgcn-amd-amdhsa-opencl
#endif

using namespace llvm;
using namespace llvm::object;
using namespace clang;
using namespace clang::driver;

static cl::opt<bool>
Verbose("v", cl::ZeroOrMore, cl::desc("Print information about actions taken"), cl::init(false));

namespace amd {
namespace opencl_driver {

static std::string joinf(const std::string& p1, const std::string& p2)
{
  std::string r;
  if (!p1.empty()) { r += p1; r += "/"; }
  r += p2;
  return r;
}

const char* DataTypeExt(DataType type)
{
  switch (type) {
  case DT_CL: return "cl";
  case DT_CL_HEADER: return 0;
  case DT_LLVM_BC: return "bc";
  case DT_LLVM_LL: return "ll";
  case DT_EXECUTABLE: return "bc";
  case DT_MAP: return "map";
  case DT_INTERNAL: return 0;
  default:
    assert(false); return 0;
  }
}

bool File::WriteData(const char* ptr, size_t size)
{
  using namespace std;
  ofstream out(Name().c_str(), ios::out | ios::trunc | ios::binary);
  if (!out.good()) { return false; }
  out.write(ptr, size);
  if (!out.good()) { return false; }
  return true;
}

bool FileExists(const std::string& name)
{
  FILE* f = fopen(name.c_str(), "r");
  if (f) {
    fclose(f);
    return true;
  }
  return false;
}

bool File::Exists() const
{
  return FileExists(Name());
}

class TempFile : public File {
public:
  TempFile(DataType type, const std::string& name)
    : File(type, name) {}
  ~TempFile();
};

TempFile::~TempFile()
{
  if (getenv("KEEP_TMP"))
    return;
  std::remove(Name().c_str());
}

class TempDir : public File {
public:
  TempDir(const std::string& name)
    : File(DT_INTERNAL, name) {}
  ~TempDir();
};

TempDir::~TempDir()
{
  if (getenv("KEEP_TMP"))
    return;
#ifdef _WIN32
  RemoveDirectory(Name().c_str());
#else // _WIN32
  rmdir(Name().c_str());
#endif // _WIN32
}

FileReference* BufferReference::ToInputFile(Compiler* comp, File *parent)
{
  File* f;
  f = comp->NewTempFile(Type(), Id(), parent);
  if (!f) { return 0; }
  if (!f->WriteData(ptr, size)) { return 0; }
  return f;
}

bool FileReference::ReadToString(std::string& s)
{
  using namespace std;
  ifstream in(Name().c_str(), ios::in | ios::binary | ios::ate);
  if (!in.good()) { return false; }
  streampos size = in.tellg();
  in.seekg(0, ios::beg);
  s.reserve(size);
  s.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (!in.good()) { return false; }
  return true;
}

File* BufferReference::ToOutputFile(Compiler* comp, File *parent)
{
  assert(false);
  return 0;
}

FileReference* Buffer::ToInputFile(Compiler* comp, File *parent)
{
  File* f = comp->NewTempFile(Type());
  if (!f->WriteData(&buf[0], buf.size())) { delete f; return 0; }
  return f;
}

File* Buffer::ToOutputFile(Compiler* comp, File* parent)
{
  File* f = comp->NewTempFile(Type(), "", parent);
  return f;
}

bool Buffer::ReadOutputFile(File* f)
{
  using namespace std;
  ifstream in(f->Name().c_str(), ios::in | ios::binary | ios::ate);
  if (!in.good()) { return false; }
  streampos size = in.tellg();
  in.seekg(0, ios::beg);
  buf.reserve(size);
  buf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (!in.good()) { return false; }
  return true;
}

class TempFiles {
private:
#ifdef _WIN32
  char tempDir[MAX_PATH];
#else // _WIN32
  const char* tempDir;
#endif // _WIN32

public:
  TempFiles() {
#ifdef _WIN32
    if (!GetTempPath(MAX_PATH, tempDir)) {
      assert(!"GetTempPath failed");
    }
#else
    tempDir = getenv("TMPDIR");
#ifdef P_tmpdir
    if (!tempDir) {
      tempDir = P_tmpdir;
    }
#endif // P_tmpdir
    if (!tempDir) {
      tempDir = "/tmp";
    }
#endif
  }

  static const TempFiles& Instance() {
    static TempFiles instance;
    return instance;
  }

#ifdef _WIN32
#define getpid _getpid
#endif

  std::string NewTempName(const char* dir, const char* prefix, const char* ext, bool pid = true) const {
    static std::atomic_size_t counter(1);

    if (!dir) { dir = tempDir; }
    std::ostringstream name;
    name << dir << "/" << prefix << getpid() << "_" << counter++;
    if (ext) { name << "." << ext; }
    return name.str();
  }
};

class AMDGPUCompiler : public Compiler {
private:
  std::string output;
  llvm::raw_string_ostream OS;
  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts;
  TextDiagnosticPrinter* diagClient;
  IntrusiveRefCntPtr<DiagnosticIDs> diagID;
  DiagnosticsEngine diags;
  std::vector<Data*> datas;
  std::string llvmBin;
  std::string llvmLinkExe;
  File* compilerTempDir;
  bool debug;
  bool inprocess;
  bool linkinprocess;

  template <typename T>
  inline T* AddData(T* d) { datas.push_back(d); return d; }
  void AddCommonArgs(std::vector<const char*>& args);
  bool InvokeDriver(ArrayRef<const char*> args);
  bool InvokeLLVMLink(ArrayRef<const char*> args);

  File* CompilerTempDir() {
    if (!compilerTempDir) {
      compilerTempDir = NewTempDir();
    }
    return compilerTempDir;
  }

  FileReference* ToInputFile(Data* input, File *parent);
  File* ToOutputFile(Data* output, File *parent);

  bool CompileToLLVMBitcode(Data* input, Data* output, const std::vector<std::string>& options);
  bool CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options);
  bool DumpExecutableAsText(Buffer* exec, File* dump) override;

public:
  AMDGPUCompiler(const std::string& llvmBin);
  ~AMDGPUCompiler();

  std::string Output() override { OS.flush(); return output; }

  FileReference* NewFileReference(DataType type, const std::string& path, File* parent = 0) override;

  File* NewFile(DataType type, const std::string& name, File* parent = 0) override;

  File* NewTempFile(DataType type, const std::string& name = "", File* parent = 0) override;

  File* NewTempDir(File* parent = 0) override;

  BufferReference* NewBufferReference(DataType type, const char* ptr, size_t size, const std::string& id) override;

  Buffer* NewBuffer(DataType type) override;

  bool CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  bool LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  bool CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  void SetInProcess(bool binprocess = true) override { inprocess = binprocess; }

  bool IsInProcess() override;

  bool IsLinkInProcess() override;
};

void AMDGPUCompiler::AddCommonArgs(std::vector<const char*>& args)
{
  args.push_back("-x cl");
//  args.push_back("-v");
}

bool AMDGPUCompiler::IsInProcess()
{
  const char* in_process_env = getenv("AMD_OCL_IN_PROCESS");
  if (in_process_env) {
    if (in_process_env[0] != '0')
      return true;
    else
      return false;
  }
  return inprocess;
}

bool AMDGPUCompiler::IsLinkInProcess()
{
  const char* in_process_env = getenv("AMD_OCL_LINK_IN_PROCESS");
  if (in_process_env) {
    if (in_process_env[0] != '0')
      return true;
    else
      return false;
  }
  return linkinprocess;
}

AMDGPUCompiler::AMDGPUCompiler(const std::string& llvmBin_)
  : OS(output),
    diagOpts(new DiagnosticOptions()),
    diagClient(new TextDiagnosticPrinter(OS, &*diagOpts)),
    diags(diagID, &*diagOpts, &*diagClient),
    llvmBin(llvmBin_),
    llvmLinkExe(llvmBin + "/llvm-link"),
    compilerTempDir(0),
    debug(false),
    inprocess(false),
    linkinprocess(true)
{
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUDisassembler();
}

AMDGPUCompiler::~AMDGPUCompiler()
{
  for (size_t i = datas.size(); i > 0; --i) {
    delete datas[i-1];
  }
}

bool AMDGPUCompiler::InvokeDriver(ArrayRef<const char*> args)
{
  std::unique_ptr<Driver> driver(new Driver(llvmBin + "/clang", STRING(AMDGCN_TRIPLE), diags));
  driver->CCPrintOptions = !!::getenv("CC_PRINT_OPTIONS");
  driver->setTitle("AMDGPU OpenCL driver");
  driver->setCheckInputsExist(false);
  if (debug) {
    OS << "InvokeDriver: ";
    for (const char* arg : args) {
      OS << "\"" << arg << "\" ";
    }
    OS << "\n";
  }
  std::unique_ptr<Compilation> C(driver->BuildCompilation(args));

  File* out = NewTempFile(DT_INTERNAL);
  File* err = NewTempFile(DT_INTERNAL);

  const StringRef** Redirects = new const StringRef*[3];
  Redirects[0] = nullptr;
  Redirects[1] = new StringRef(out->Name());
  Redirects[2] = new StringRef(err->Name());

  C->Redirect(Redirects);

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

  if (debug) {
    driver->PrintActions(*C);
  }

  std::string outStr, errStr;
  out->ReadToString(outStr);
  err->ReadToString(errStr);

  if (!outStr.empty()) { OS << outStr; }
  if (!errStr.empty()) { OS << errStr; }

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
//  args1.push_back("-v");
  for (const char *arg : args) { args1.push_back(arg); }
  args1.push_back(nullptr);
  if (debug) {
    OS << "InvokeLLVMLink: ";
    OS << llvmLinkExe << "\n";
    for (const char* arg : args1) {
      if (arg) {
        OS << "\"" << arg << "\" ";
      }
    }
    OS << "\n\n";
  }

  File* out = NewTempFile(DT_INTERNAL);
  File* err = NewTempFile(DT_INTERNAL);

  const StringRef** Redirects = new const StringRef*[3];
  Redirects[0] = nullptr;
  Redirects[1] = new StringRef(out->Name());
  Redirects[2] = new StringRef(err->Name());


  int res = llvm::sys::ExecuteAndWait(llvmLinkExe, args1.data(), nullptr, Redirects);

  std::string outStr, errStr;
  out->ReadToString(outStr);
  err->ReadToString(errStr);

  if (!outStr.empty()) { OS << outStr; }
  if (!errStr.empty()) { OS << errStr; }


  if (res != 0) {
    return false;
  }
  return true;
}

FileReference* AMDGPUCompiler::ToInputFile(Data* input, File *parent)
{
  return input->ToInputFile(this, parent);
}

File* AMDGPUCompiler::ToOutputFile(Data* output, File* parent)
{
  return output->ToOutputFile(this, parent);
}

File* AMDGPUCompiler::NewFile(DataType type, const std::string& name, File* parent)
{
  std::string fname = parent ? joinf(parent->Name(), name) : name;
  return AddData(new File(type, fname));
}

FileReference* AMDGPUCompiler::NewFileReference(DataType type, const std::string& name, File* parent)
{
  std::string fname = parent ? joinf(parent->Name(), name) : name;
  return AddData(new FileReference(type, fname));
}

File* AMDGPUCompiler::NewTempFile(DataType type, const std::string& name, File* parent)
{
  if (!parent) { parent = CompilerTempDir(); }
  const char* dir = parent->Name().c_str();
  const char* ext = DataTypeExt(type);
  bool pid = !parent;
  std::string fname = name.empty() ?
                        TempFiles::Instance().NewTempName(dir, "t_", ext, pid) :
                        joinf(parent->Name(), name);
  if (FileExists(fname)) { return 0; }
  return AddData(new TempFile(type, fname));
}

File* AMDGPUCompiler::NewTempDir(File* parent)
{
  const char* dir = parent ? parent->Name().c_str() : 0;
  bool pid = !parent;
  std::string name = TempFiles::Instance().NewTempName(dir, "AMD_", 0, pid);
#ifdef _WIN32
  CreateDirectory(name.c_str(), NULL);
#else // _WIN32
  mkdir(name.c_str(), 0700);
#endif // _WIN32
  return AddData(new TempDir(name));
}

BufferReference* AMDGPUCompiler::NewBufferReference(DataType type, const char* ptr, size_t size, const std::string& id)
{
  return AddData(new BufferReference(type, ptr, size, id));
}

Buffer* AMDGPUCompiler::NewBuffer(DataType type)
{
  return AddData(new Buffer(type));
}

bool AMDGPUCompiler::CompileToLLVMBitcode(Data* input, Data* output, const std::vector<std::string>& options)
{
  std::vector<const char*> args;

  AddCommonArgs(args);
  args.push_back("-c");
  args.push_back("-emit-llvm");

  FileReference* inputFile = ToInputFile(input, CompilerTempDir());
  args.push_back(inputFile->Name().c_str());

  File* bcFile = ToOutputFile(output, CompilerTempDir());

  args.push_back("-o");
  args.push_back(bcFile->Name().c_str());

  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }
  if (IsInProcess()) {
    std::unique_ptr<Driver> driver(new Driver("", STRING(AMDGCN_TRIPLE), diags));
    driver->CCPrintOptions = !!::getenv("CC_PRINT_OPTIONS");
    driver->setTitle("AMDGPU OpenCL driver");
    driver->setCheckInputsExist(false);
    std::unique_ptr<Compilation> C(driver->BuildCompilation(args));
    const driver::JobList &Jobs = C->getJobs();
    const driver::Command &Cmd = cast<driver::Command>(*Jobs.begin());
    const driver::ArgStringList &CCArgs = Cmd.getArguments();
    // Create the compiler invocation
    std::shared_ptr<clang::CompilerInvocation> CI(new clang::CompilerInvocation);
    // Create the compiler instance
    clang::CompilerInstance Clang;
    Clang.createDiagnostics();
    if (!Clang.hasDiagnostics()) {
      return false;
    }
    if (!CompilerInvocation::CreateFromArgs(*CI,
        const_cast<const char **>(CCArgs.data()),
        const_cast<const char **>(CCArgs.data()) +
        CCArgs.size(),
        Clang.getDiagnostics())) {
      return false;
    }
    Clang.setInvocation(CI);
    // Action Backend_EmitBC
    std::unique_ptr<clang::CodeGenAction> Act(new clang::EmitBCAction());
    if (!Act.get()) {
      return false;
    }
    if (!Clang.ExecuteAction(*Act)) {
        return false;
    }
    // Grab the module built by the EmitLLVMOnlyAction
    // ToDo: 2nd phase. In-memory.
    //std::unique_ptr<llvm::Module> module = Act->takeModule();
    //if (!module.get()) {
    //    return false;
    //}
    //module->dump();
  } else {
    if (!InvokeDriver(args)) {
      return false;
    }
  }
  if (!output->ReadOutputFile(bcFile)) {
      return false;
  }
  return true;
}

const std::vector<std::string> emptyOptions;

bool AMDGPUCompiler::CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options)
{
  if (inputs.size() == 1) {
    return CompileToLLVMBitcode(inputs[0], output, options);
  } else {
    std::vector<Data*> bcFiles;
    std::vector<std::string> xoptions;
    File* includeDir = 0;
    for (Data* input : inputs) {
      if (input->Type() == DT_CL_HEADER) {
        if (!includeDir) {
          includeDir = NewTempDir(CompilerTempDir());
          xoptions.push_back("-I" + includeDir->Name());
        }
        ToInputFile(input, includeDir);
      }
    }
    for (const std::string& o : options) { xoptions.push_back(o); }
    for (Data* input : inputs) {
      if (input->Type() == DT_CL_HEADER) { continue; }
      File* bcFile = NewTempFile(DT_LLVM_BC);
      if (!CompileToLLVMBitcode(input, bcFile, xoptions)) { return false; }
      bcFiles.push_back(bcFile);
    }
    return LinkLLVMBitcode(bcFiles, output, emptyOptions);
  }
}

bool AMDGPUCompiler::LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options)
{
  std::vector<const char*> args;
  for (Data* input : inputs) {
    FileReference* inputFile = ToInputFile(input, CompilerTempDir());
    args.push_back(inputFile->Name().c_str());
  }
  File* outputFile = ToOutputFile(output, CompilerTempDir());
  if (IsInProcess() || IsLinkInProcess()) {
    if (!options.empty()) {
      std::vector<const char*> argv;
      for (auto option : options) {
        argv.push_back("");
        argv.push_back(option.c_str());
        // parse linker options
        if (!cl::ParseCommandLineOptions(argv.size(), &argv[0], "llvm linker\n")) {
          return false;
        }
        argv.clear();
      }
    }
    LLVMContext context;
    auto Composite = make_unique<llvm::Module>("composite", context);
    Linker L(*Composite);
    unsigned ApplicableFlags = Linker::Flags::None;
    for (auto arg : args) {
      SMDiagnostic error;
      auto m = getLazyIRFileModule(arg, error, context);
      if (!m.get()) {
        errs() << "ERROR: the module '" << arg << "' loading failed!\n";
        return false;
      }
      if (verifyModule(*m, &errs())) {
        outs() << "ERROR: loaded module '" << arg << "' to link is broken!\n";
        return false;
      }
      if (Verbose) {
        std::cout << "Linking in '" << arg << "'\n";
      }
      if (L.linkInModule(std::move(m), ApplicableFlags)) {
        errs() << "ERROR: the module '" << arg << "' is not linked!\n";
        return false;
      }
    }
    if (verifyModule(*Composite, &errs())) {
      errs() << "ERROR: the linked module '" << outputFile->Name() << "' is broken!\n";
      return false;
    }
    std::error_code ec;
    llvm::tool_output_file out(outputFile->Name(), ec, sys::fs::F_None);
    WriteBitcodeToFile(Composite.get(), out.os());
    out.keep();
  } else {
    args.push_back("-o");
    args.push_back(outputFile->Name().c_str());
    for (const std::string& s : options) {
      args.push_back(s.c_str());
    }
    if (!InvokeLLVMLink(args)) {
      return false;
    }
  }
  return output->ReadOutputFile(outputFile);
}

bool AMDGPUCompiler::CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options)
{
  std::vector<const char*> args;

  AddCommonArgs(args);

  File* mapFile = NewTempFile(DT_MAP, "", CompilerTempDir());
  std::string smap = "-Wl,-Map=";
  smap.append(mapFile->Name().c_str());
  args.push_back(smap.c_str());

  FileReference* inputFile = ToInputFile(input, CompilerTempDir());
  args.push_back(inputFile->Name().c_str());

  File* outputFile = ToOutputFile(output, CompilerTempDir());

  args.push_back("-o"); args.push_back(outputFile->Name().c_str());

  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }

  bool res = InvokeDriver(args);
  if (res) { output->ReadOutputFile(outputFile); }
  return res;
}

bool AMDGPUCompiler::CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options)
{
  if (inputs.size() == 1) {
    return CompileAndLinkExecutable(inputs[0], output, options);
  } else {
    File* bcFile = NewTempFile(DT_LLVM_BC);
    if (!CompileToLLVMBitcode(inputs, bcFile, options)) { return false; }
    return CompileAndLinkExecutable(bcFile, output, options);
  }
}

bool AMDGPUCompiler::DumpExecutableAsText(Buffer* exec, File* dump)
{
  StringRef TripleName = Triple::normalize(STRING(AMDGCN_TRIPLE));

  StringRef execRef(exec->Ptr(), exec->Size());
  std::unique_ptr<MemoryBuffer> execBuffer(MemoryBuffer::getMemBuffer(execRef, "", false));
  Expected<std::unique_ptr<Binary>> execBinary = createBinary(*execBuffer);
  if (!execBinary) { return false; }
  std::unique_ptr<Binary> Binary(execBinary.get().release());
  if (!Binary)
    return false;

  // setup context
  const auto &TheTarget = getTheGCNTarget();

  std::unique_ptr<MCRegisterInfo> MRI(TheTarget.createMCRegInfo(TripleName));
  if (!MRI) { return false; }

  std::unique_ptr<MCAsmInfo> AsmInfo(TheTarget.createMCAsmInfo(*MRI, TripleName));
  if (!AsmInfo) { return false; }

  std::unique_ptr<MCInstrInfo> MII(TheTarget.createMCInstrInfo());
  if (!MII) { return false; }

  MCObjectFileInfo MOFI;
  MCContext Ctx(AsmInfo.get(), MRI.get(), &MOFI);
  MOFI.InitMCObjectFileInfo(Triple(TripleName), false, CodeModel::Default, Ctx);

  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  MCInstPrinter *IP(TheTarget.createMCInstPrinter(Triple(TripleName),
                                                  AsmPrinterVariant,
                                                  *AsmInfo, *MII, *MRI));
  if (!IP)
    report_fatal_error("error: no instruction printer");

  std::error_code EC;

  raw_fd_ostream FO(dump->Name(), EC, sys::fs::F_None);

  auto FOut = make_unique<formatted_raw_ostream>(FO);

  std::unique_ptr<MCStreamer> MCS(
    TheTarget.createAsmStreamer(Ctx, std::move(FOut), true, false, IP,
                                nullptr, nullptr, false));

  CodeObjectDisassembler CODisasm(&Ctx, TripleName, IP, MCS->getTargetStreamer());

  EC = CODisasm.Disassemble(Binary->getMemoryBufferRef(), errs());
  if (EC) { return false; }

  return true;
}

Compiler* CompilerFactory::CreateAMDGPUCompiler(const std::string& llvmBin)
{
  return new AMDGPUCompiler(llvmBin);
}

}
}

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
#include "clang/Driver/Tool.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Bitcode/BitcodeWriter.h"

#include <sstream>
#include <iostream>
#include <mutex>

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

namespace amd {
namespace opencl_driver {

class AMDGPUCompiler;
static void DiagnosticHandler(const DiagnosticInfo &DI, void *C);

class LinkerDiagnosticInfo : public DiagnosticInfo {
private:
  const Twine &Message;

public:
  LinkerDiagnosticInfo(DiagnosticSeverity Severity, const Twine &Message)
    : DiagnosticInfo(DK_Linker, Severity), Message(Message) {}

  void print(DiagnosticPrinter &DP) const override { DP << Message; }
};

const char* DataTypeExt(DataType type) {
  switch (type) {
    case DT_CL: return "cl";
    case DT_CL_HEADER: return 0;
    case DT_LLVM_BC: return "bc";
    case DT_LLVM_LL: return "ll";
    case DT_EXECUTABLE: return "bc";
    case DT_MAP: return "map";
    case DT_INTERNAL: return 0;
    default: assert(false); return 0;
  }
}

bool File::WriteData(const char* ptr, size_t size) {
  using namespace std;
  ofstream out(Name().c_str(), ios::out | ios::trunc | ios::binary);
  if (!out.good()) { return false; }
  out.write(ptr, size);
  if (!out.good()) { return false; }
  return true;
}

bool FileExists(const std::string& name) {
  FILE* f = fopen(name.c_str(), "r");
  if (f) {
    fclose(f);
    return true;
  }
  return false;
}

bool File::Exists() const {
  return FileExists(Name());
}

class TempFile : public File {
public:
  TempFile(Compiler* comp, DataType type, const std::string& name)
    : File(comp, type, name) {}
  ~TempFile();
};

class TempDir : public File {
public:
  TempDir(Compiler* comp, const std::string& name)
    : File(comp, DT_INTERNAL, name) {}
  ~TempDir();
};

FileReference* BufferReference::ToInputFile(File *parent) {
  File* f;
  f = compiler->NewTempFile(Type(), Id(), parent);
  if (!f) { return 0; }
  if (!f->WriteData(ptr, size)) { return 0; }
  return f;
}

bool FileReference::ReadToString(std::string& s) {
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

File* BufferReference::ToOutputFile(File *parent) {
  assert(false);
  return 0;
}

FileReference* Buffer::ToInputFile(File *parent) {
  File* f = compiler->NewTempFile(Type());
  if (!f->WriteData(&buf[0], buf.size())) { delete f; return 0; }
  return f;
}

File* Buffer::ToOutputFile(File* parent) {
  File* f = compiler->NewTempFile(Type(), "", parent);
  return f;
}

bool Buffer::ReadOutputFile(File* f) {
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
friend void DiagnosticHandler(const DiagnosticInfo &DI, void *C);
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
  bool inprocess;
  bool linkinprocess;
  LogLevel logLevel;
  bool printlog;
  bool keeptmp;

  template <typename T>
  inline T* AddData(T* d) { datas.push_back(d); return d; }
  void AddCommonArgs(std::vector<const char*>& args);
  void ResetOptionsToDefault();
  // Filter out option(s) contradictory to in-process compilation
  void FilterArgs(ArgStringList& args);
  // Parse -mllvm options
  bool ParseLLVMOptions(const CompilerInstance& clang);
  bool PrepareCompiler(CompilerInstance& clang, const Command& job);
  void InitDriver(std::unique_ptr<Driver>& driver);
  bool InvokeDriver(ArrayRef<const char*> args);
  bool InvokeTool(ArrayRef<const char*> args, const std::string& sToolName);
  void PrintOptions(ArrayRef<const char*> args, const std::string& sToolName, bool isInProcess);
  void PrintJobs(const JobList &jobs);
  void PrintPhase(const std::string& phase, bool isInProcess);
  bool Return(bool retValue);
  File* CompilerTempDir();
  bool IsVar(const std::string& sEnvVar, bool bVar);
  bool EmitLinkerError(LLVMContext &context, const Twine &message);
  std::string JoinFileName(const std::string& p1, const std::string& p2);

  FileReference* ToInputFile(Data* input, File *parent);

  File* ToOutputFile(Data* output, File *parent);

  bool CompileToLLVMBitcode(Data* input, Data* output, const std::vector<std::string>& options);

  bool CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options);

  bool DumpExecutableAsText(Buffer* exec, File* dump) override;

public:
  AMDGPUCompiler(const std::string& llvmBin);

  ~AMDGPUCompiler();

  const std::string& Output() override;

  FileReference* NewFileReference(DataType type, const std::string& path, File* parent = 0) override;

  File* NewFile(DataType type, const std::string& name, File* parent = 0) override;

  File* NewTempFile(DataType type, const std::string& name = "", File* parent = 0) override;

  File* NewTempDir(File* parent = 0) override;

  BufferReference* NewBufferReference(DataType type, const char* ptr, size_t size, const std::string& id) override;

  Buffer* NewBuffer(DataType type) override;

  bool CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  bool LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  bool CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) override;

  void SetInProcess(bool binprocess = true) override;

  bool IsInProcess() override { return IsVar("AMD_OCL_IN_PROCESS", inprocess); }

  bool IsLinkInProcess() override { return IsVar("AMD_OCL_LINK_IN_PROCESS", linkinprocess); }

  void SetKeepTmp(bool bkeeptmp = true) override { keeptmp = bkeeptmp; }

  bool IsKeepTmp() override { return IsVar("AMD_OCL_KEEP_TMP", keeptmp); }

  void SetPrintLog(bool bprintlog = true) override { printlog = bprintlog; }

  bool IsPrintLog() override { return IsVar("AMD_OCL_PRINT_LOG", printlog); }

  void SetLogLevel(LogLevel ll) override { logLevel = ll; }

  LogLevel GetLogLevel() override;
};

static void DiagnosticHandler(const DiagnosticInfo &DI, void *C) {
  if (!C) { return; }
  AMDGPUCompiler* compiler = static_cast<AMDGPUCompiler*>(C);
  if (compiler->GetLogLevel() < LL_VERBOSE) { return; }
  unsigned Severity = DI.getSeverity();
  switch (Severity) {
    case DS_Error:
      compiler->OS << "ERROR: ";
      break;
    default:
      llvm_unreachable("Only expecting errors");
  }
  DiagnosticPrinterRawOStream DP(errs());
  DI.print(DP);
  compiler->OS << "\n";
}

TempFile::~TempFile() {
  if (compiler->IsKeepTmp()) { return; }
  std::remove(Name().c_str());
}

TempDir::~TempDir() {
  if (compiler->IsKeepTmp()) { return; }
#ifdef _WIN32
  RemoveDirectory(Name().c_str());
#else // _WIN32
  rmdir(Name().c_str());
#endif // _WIN32
}

File* AMDGPUCompiler::CompilerTempDir() {
  if (!compilerTempDir) { compilerTempDir = NewTempDir(); }
  return compilerTempDir;
}

void AMDGPUCompiler::SetInProcess(bool binprocess) {
  inprocess = binprocess;
  if (IsInProcess()) { LLVMInitializeAMDGPUAsmPrinter(); }
}

std::string AMDGPUCompiler::JoinFileName(const std::string& p1, const std::string& p2) {
  std::string r;
  if (!p1.empty()) { r += p1; r += "/"; }
  r += p2;
  return r;
}

void AMDGPUCompiler::InitDriver(std::unique_ptr<Driver>& driver) {
  driver->CCPrintOptions = !!::getenv("CC_PRINT_OPTIONS");
  driver->setTitle("AMDGPU OpenCL driver");
  driver->setCheckInputsExist(false);
}

void AMDGPUCompiler::AddCommonArgs(std::vector<const char*>& args) {
  args.push_back("-x cl");
}

void AMDGPUCompiler::FilterArgs(ArgStringList& args) {
  ArgStringList::iterator it = std::find(args.begin(), args.end(), "-disable-free");
  if (it != args.end()) {
    args.erase(it);
  }
}

bool AMDGPUCompiler::ParseLLVMOptions(const CompilerInstance& clang) {
  if (clang.getFrontendOpts().LLVMArgs.empty()) { return true; }
  std::vector<const char*> args;
  for (auto A : clang.getFrontendOpts().LLVMArgs) {
    args.push_back("");
    args.push_back(A.c_str());
    if (!cl::ParseCommandLineOptions(args.size(), &args[0], "-mllvm options parsing")) { return false; }
    args.clear();
  }
  return true;
}

void AMDGPUCompiler::ResetOptionsToDefault() {
  cl::ResetAllOptionOccurrences();
  for (auto SC : cl::getRegisteredSubcommands()) {
    for (auto &OM : SC->OptionsMap) {
      cl::Option *O = OM.second;
      O->setDefault();
    }
  }
}

bool AMDGPUCompiler::PrepareCompiler(CompilerInstance& clang, const Command& job) {
  ResetOptionsToDefault();
  ArgStringList CCArgs(job.getArguments());
  FilterArgs(CCArgs);
  clang.createDiagnostics();
  if (!clang.hasDiagnostics()) { return false; }
  if (!CompilerInvocation::CreateFromArgs(clang.getInvocation(),
    const_cast<const char**>(CCArgs.data()),
    const_cast<const char**>(CCArgs.data()) +
    CCArgs.size(),
    clang.getDiagnostics())) { return false; }
  if (!ParseLLVMOptions(clang)) { return false; }
  return true;
}

bool AMDGPUCompiler::IsVar(const std::string& sEnvVar, bool bVar) {
  const char* env = getenv(sEnvVar.c_str());
  if (env) {
    if (env[0] != '0') { return true; }
    else { return false; }
  }
  return bVar;
}

LogLevel AMDGPUCompiler::GetLogLevel() {
  const char* log_level = getenv("AMD_OCL_LOG_LEVEL");
  if (log_level) {
    std::stringstream ss(log_level);
    unsigned ll;
    ss >> ll;
    switch (ll) {
      default:
      case LL_QUIET: return LL_QUIET;
      case LL_ERRORS: return LL_ERRORS;
      case LL_LLVM_ONLY: return LL_LLVM_ONLY;
      case LL_VERBOSE: return LL_VERBOSE;
    }
  }
  return logLevel;
}

const std::string& AMDGPUCompiler::Output() {
  output = {};
  if (GetLogLevel() > LL_QUIET) { OS.flush(); }
  return output;
}

bool AMDGPUCompiler::Return(bool retValue) {
  if (IsPrintLog()) {
    static std::mutex m_screen;
    m_screen.lock();
    std::cout << Output();
    m_screen.unlock();
  }
  return retValue;
}

void AMDGPUCompiler::PrintJobs(const JobList &jobs) {
  if (GetLogLevel() < LL_VERBOSE || jobs.empty()) { return; }
  OS << "\n[AMD OCL] " << jobs.size() << " job" << (jobs.size() == 1 ? "" : "s") << ":\n";
  int i = 1;
  for (auto const & J : jobs) {
    std::string sJobName(J.getCreator().getName());
    OS << (i > 1 ? "\n" : "") << "  JOB [" << i << "] " << sJobName << "\n";
    ArgStringList Args(J.getArguments());
    if (sJobName == "clang") {
      FilterArgs(Args);
    }
    for (auto A : Args) {
      OS << "      " << A << "\n";
    }
    ++i;
  }
  OS << "\n";
}

void AMDGPUCompiler::PrintPhase(const std::string& phase, bool isInProcess) {
  if (GetLogLevel() < LL_VERBOSE) { return; }
  OS << "\n[AMD OCL] " << "Phase: " << phase << (isInProcess ? " [in-process]" : "") << "\n";
}

void AMDGPUCompiler::PrintOptions(ArrayRef<const char*> args, const std::string& sToolName, bool isInProcess) {
  if (GetLogLevel() < LL_VERBOSE) { return; }
  OS << "\n[AMD OCL] " << sToolName << (isInProcess ? " [in-process]" : "") << " options:\n";
  for (const char* A : args) {
    OS << "      " << A << "\n";
  }
  OS << "\n";
}

AMDGPUCompiler::AMDGPUCompiler(const std::string& llvmBin_)
  : OS(output),
    diagOpts(new DiagnosticOptions()),
    diagClient(new TextDiagnosticPrinter(OS, &*diagOpts)),
    diags(diagID, &*diagOpts, &*diagClient),
    llvmBin(llvmBin_),
    llvmLinkExe(llvmBin + "/llvm-link"),
    compilerTempDir(0),
    inprocess(true),
    linkinprocess(true),
    logLevel(LL_ERRORS),
    printlog(false),
    keeptmp(false) {
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUDisassembler();
  if (IsInProcess()) {
    LLVMInitializeAMDGPUAsmPrinter();
  }
}

AMDGPUCompiler::~AMDGPUCompiler() {
  for (size_t i = datas.size(); i > 0; --i) {
    delete datas[i-1];
  }
}

bool AMDGPUCompiler::InvokeDriver(ArrayRef<const char*> args) {
  std::unique_ptr<Driver> driver(new Driver(llvmBin + "/clang", STRING(AMDGCN_TRIPLE), diags));
  InitDriver(driver);
  std::unique_ptr<Compilation> C(driver->BuildCompilation(args));
  PrintJobs(C->getJobs());
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
    if (!Res) { Res = CommandRes; }
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
  std::string outStr, errStr;
  out->ReadToString(outStr);
  err->ReadToString(errStr);
  if (GetLogLevel() >= LL_LLVM_ONLY && !outStr.empty()) { OS << outStr; }
  if (GetLogLevel() >= LL_ERRORS && !errStr.empty()) { OS << errStr; }
#ifdef LLVM_ON_WIN32
  // Exit status should not be negative on Win32, unless abnormal termination.
  // Once abnormal termiation was caught, negative status should not be
  // propagated.
  if (Res < 0)
    Res = 1;
#endif
  return Res == 0;
}

bool AMDGPUCompiler::InvokeTool(ArrayRef<const char*> args, const std::string& sToolName) {
  PrintOptions(args, sToolName, false);
  SmallVector<const char*, 128> args1;
  args1.push_back(sToolName.c_str());
  for (const char *arg : args) { args1.push_back(arg); }
  args1.push_back(nullptr);
  File* out = NewTempFile(DT_INTERNAL);
  File* err = NewTempFile(DT_INTERNAL);
  const StringRef** Redirects = new const StringRef*[3];
  Redirects[0] = nullptr;
  Redirects[1] = new StringRef(out->Name());
  Redirects[2] = new StringRef(err->Name());
  int res = llvm::sys::ExecuteAndWait(sToolName, args1.data(), nullptr, Redirects);
  std::string outStr, errStr;
  out->ReadToString(outStr);
  err->ReadToString(errStr);
  if (GetLogLevel() >= LL_LLVM_ONLY && !outStr.empty()) { OS << outStr; }
  if (GetLogLevel() >= LL_ERRORS && !errStr.empty()) { OS << errStr; }
  return res == 0;
}

FileReference* AMDGPUCompiler::ToInputFile(Data* input, File *parent) {
  return input->ToInputFile(parent);
}

File* AMDGPUCompiler::ToOutputFile(Data* output, File* parent) {
  return output->ToOutputFile(parent);
}

File* AMDGPUCompiler::NewFile(DataType type, const std::string& name, File* parent) {
  std::string fname = parent ? JoinFileName(parent->Name(), name) : name;
  return AddData(new File(this, type, fname));
}

FileReference* AMDGPUCompiler::NewFileReference(DataType type, const std::string& name, File* parent) {
  std::string fname = parent ? JoinFileName(parent->Name(), name) : name;
  return AddData(new FileReference(this, type, fname));
}

File* AMDGPUCompiler::NewTempFile(DataType type, const std::string& name, File* parent) {
  if (!parent) { parent = CompilerTempDir(); }
  const char* dir = parent->Name().c_str();
  const char* ext = DataTypeExt(type);
  bool pid = !parent;
  std::string fname = name.empty() ?
                        TempFiles::Instance().NewTempName(dir, "t_", ext, pid) :
                        JoinFileName(parent->Name(), name);
  if (FileExists(fname)) { return 0; }
  return AddData(new TempFile(this, type, fname));
}

File* AMDGPUCompiler::NewTempDir(File* parent) {
  const char* dir = parent ? parent->Name().c_str() : 0;
  bool pid = !parent;
  std::string name = TempFiles::Instance().NewTempName(dir, "AMD_", 0, pid);
#ifdef _WIN32
  CreateDirectory(name.c_str(), NULL);
#else // _WIN32
  mkdir(name.c_str(), 0700);
#endif // _WIN32
  return AddData(new TempDir(this, name));
}

BufferReference* AMDGPUCompiler::NewBufferReference(DataType type, const char* ptr, size_t size, const std::string& id) {
  return AddData(new BufferReference(this, type, ptr, size, id));
}

Buffer* AMDGPUCompiler::NewBuffer(DataType type) {
  return AddData(new Buffer(this, type));
}

bool AMDGPUCompiler::CompileToLLVMBitcode(Data* input, Data* output, const std::vector<std::string>& options) {
  PrintPhase("CompileToLLVMBitcode", IsInProcess());
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
  PrintOptions(args, "clang Driver", IsInProcess());
  if (IsInProcess()) {
    std::unique_ptr<Driver> driver(new Driver("", STRING(AMDGCN_TRIPLE), diags));
    InitDriver(driver);
    std::unique_ptr<Compilation> C(driver->BuildCompilation(args));
    const JobList &Jobs = C->getJobs();
    PrintJobs(Jobs);
    CompilerInstance Clang;
    if (!PrepareCompiler(Clang, *Jobs.begin())) { return Return(false); }
    // Action Backend_EmitBC
    std::unique_ptr<CodeGenAction> Act(new EmitBCAction());
    if (!Act.get()) { return Return(false); }
    if (!Clang.ExecuteAction(*Act)) { return Return(false); }
  } else {
    if (!InvokeDriver(args)) { return Return(false); }
  }
  return Return(output->ReadOutputFile(bcFile));
}

const std::vector<std::string> emptyOptions;

bool AMDGPUCompiler::CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) {
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

bool AMDGPUCompiler::EmitLinkerError(LLVMContext &context, const Twine &message) {
  context.diagnose(LinkerDiagnosticInfo(DS_Error, message));
  return false;
}

bool AMDGPUCompiler::LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) {
  bool bIsInProcess = IsInProcess() || IsLinkInProcess();
  PrintPhase("LinkLLVMBitcode", bIsInProcess);
  std::vector<const char*> args;
  for (Data* input : inputs) {
    FileReference* inputFile = ToInputFile(input, CompilerTempDir());
    args.push_back(inputFile->Name().c_str());
  }
  File* outputFile = ToOutputFile(output, CompilerTempDir());
  if (!options.empty()) {
    std::vector<const char*> argv;
    for (auto option : options) {
      argv.push_back("");
      argv.push_back(option.c_str());
      args.push_back(option.c_str());
      if (!cl::ParseCommandLineOptions(argv.size(), &argv[0], "llvm linker")) {
        return Return(false);
      }
      argv.clear();
    }
  }
  if (!bIsInProcess) {
    args.push_back("-o");
    args.push_back(outputFile->Name().c_str());
  }
  if (bIsInProcess) {
    PrintOptions(args, "llvm linker", bIsInProcess);
    LLVMContext context;
    context.setDiagnosticHandler(DiagnosticHandler, this, true);
    auto Composite = make_unique<llvm::Module>("composite", context);
    Linker L(*Composite);
    unsigned ApplicableFlags = Linker::Flags::None;
    for (auto arg : args) {
      SMDiagnostic error;
      auto m = getLazyIRFileModule(arg, error, context);
      if (!m.get()) {
        return EmitLinkerError(context, "The module '" + Twine(arg) + "' loading failed.");
      }
      if (verifyModule(*m, &errs())) {
        return EmitLinkerError(context, "The loaded module '" + Twine(arg) + "' to link is broken.");
      }
      if (GetLogLevel() >= LL_LLVM_ONLY) {
        OS << "[AMD OCL] Linking in '" << arg << "'" << "\n";
      }
      if (L.linkInModule(std::move(m), ApplicableFlags)) {
        return EmitLinkerError(context, "The module '" + Twine(arg) + "' is not linked.");
      }
    }
    if (verifyModule(*Composite, &errs())) {
      return EmitLinkerError(context, "The linked module '" + outputFile->Name() + "' is broken.");
    }
    std::error_code ec;
    llvm::tool_output_file out(outputFile->Name(), ec, sys::fs::F_None);
    WriteBitcodeToFile(Composite.get(), out.os());
    out.keep();
  } else {
    if (!InvokeTool(args, llvmLinkExe)) { return Return(false); }
  }
  return Return(output->ReadOutputFile(outputFile));
}

bool AMDGPUCompiler::CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options) {
  PrintPhase("CompileAndLinkExecutable", IsInProcess());
  std::vector<const char*> args;
  AddCommonArgs(args);
  FileReference* inputFile = ToInputFile(input, CompilerTempDir());
  args.push_back(inputFile->Name().c_str());
  File* outputFile = ToOutputFile(output, CompilerTempDir());
  args.push_back("-o"); args.push_back(outputFile->Name().c_str());
  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }
  PrintOptions(args, "clang Driver", IsInProcess());
  if (IsInProcess()) {
    std::unique_ptr<Driver> driver(new Driver("", STRING(AMDGCN_TRIPLE), diags));
    InitDriver(driver);
    std::unique_ptr<Compilation> C(driver->BuildCompilation(args));
    const JobList &Jobs = C->getJobs();
    PrintJobs(Jobs);
    int i = 1;
    for (auto const & J : Jobs) {
      if (Jobs.size() == 2) {
        std::string sJobName(J.getCreator().getName());
        if (i == 1 && sJobName == "clang") {
          CompilerInstance Clang;
          if (!PrepareCompiler(Clang, J)) { return Return(false); }
          // Action Backend_EmitObj
          std::unique_ptr<CodeGenAction> Act(new EmitObjAction());
          if (!Act.get()) { return Return(false); }
          if (!Clang.ExecuteAction(*Act)) { return Return(false); }
        }
        else if (i == 2 && sJobName == "amdgpu::Linker") {
          // lld fork
          if (!InvokeTool(J.getArguments(), llvmBin + +"/ld.lld")) { return Return(false); }
          /* lld statically linked */
          //ArrayRef<const char *> ArgRefs = llvm::makeArrayRef(J.getArguments());
          //if (!lld::elf::link(ArgRefs, false)) { return Return(false); }
        }
        else { return Return(false); }
        i++;
      }
      else { return Return(false); }
    }
  } else {
    if (!InvokeDriver(args)) { return Return(false); }
  }
  return Return(output->ReadOutputFile(outputFile));
}

bool AMDGPUCompiler::CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) {
  if (inputs.size() == 1) {
    return CompileAndLinkExecutable(inputs[0], output, options);
  } else {
    File* bcFile = NewTempFile(DT_LLVM_BC);
    if (!CompileToLLVMBitcode(inputs, bcFile, options)) { return false; }
    return CompileAndLinkExecutable(bcFile, output, options);
  }
}

bool AMDGPUCompiler::DumpExecutableAsText(Buffer* exec, File* dump) {
  StringRef TripleName = Triple::normalize(STRING(AMDGCN_TRIPLE));
  StringRef execRef(exec->Ptr(), exec->Size());
  std::unique_ptr<MemoryBuffer> execBuffer(MemoryBuffer::getMemBuffer(execRef, "", false));
  Expected<std::unique_ptr<Binary>> execBinary = createBinary(*execBuffer);
  if (!execBinary) { return false; }
  std::unique_ptr<Binary> Binary(execBinary.get().release());
  if (!Binary) { return false; }
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
  MOFI.InitMCObjectFileInfo(Triple(TripleName), false, Ctx);
  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  MCInstPrinter *IP(TheTarget.createMCInstPrinter(Triple(TripleName),
                                                  AsmPrinterVariant,
                                                  *AsmInfo, *MII, *MRI));
  if (!IP) { report_fatal_error("error: no instruction printer"); }
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

Compiler* CompilerFactory::CreateAMDGPUCompiler(const std::string& llvmBin) {
  return new AMDGPUCompiler(llvmBin);
}

}
}

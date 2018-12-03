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
#include "llvm/Support/VirtualFileSystem.h"

#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"

// implicitly needed.

#include "Disassembler/CodeObjectDisassembler.h"

#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
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
#include "clang/CodeGen/BackendUtil.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "lld/Common/Driver.h"

// in-process assembler
#include "clang/Basic/Diagnostic.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/Utils.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include <memory>
#include <system_error>

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
#define AMDGCN_TRIPLE amdgcn-amd-amdhsa

using namespace llvm;
using namespace llvm::object;
using namespace llvm::opt;
using namespace clang;
using namespace clang::driver;
using namespace clang::driver::options;

namespace amd {
namespace opencl_driver {

class AMDGPUCompiler;

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
    case DT_ASSEMBLY: return "s";
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
private:
  struct AMDGPUCompilerDiagnosticHandler : public DiagnosticHandler {
    AMDGPUCompiler *Compiler = nullptr;

    AMDGPUCompilerDiagnosticHandler(AMDGPUCompiler *Compiler)
      : Compiler(Compiler) {}

    bool handleDiagnostics(const DiagnosticInfo &DI) override {
      assert(Compiler && "Compiler cannot be nullptr");
      if (Compiler->GetLogLevel() < LL_VERBOSE) { return true; }
      unsigned Severity = DI.getSeverity();
      switch (Severity) {
      case DS_Error:
        Compiler->OS << "ERROR: ";
        break;
      default:
        llvm_unreachable("Only expecting errors");
      }
      DiagnosticPrinterRawOStream DP(errs());
      DI.print(DP);
      Compiler->OS << "\n";
      return true;
    }
  };

  // Helper class for representing a single invocation of the assembler.
  struct AssemblerInvocation {
    enum FileType {
      FT_Asm,  // Assembly (.s) output, transliterate mode.
      FT_Null, // No output, for timing purposes.
      FT_Obj   // Object file output.
    };
    FileType OutputType = FT_Asm;
    std::string Triple;
    // If given, the name of the target CPU to determine which instructions are legal.
    std::string CPU;
    // The list of target specific features to enable or disable -- this should
    // be a list of strings starting with '+' or '-'.
    std::vector<std::string> Features;
    // The list of symbol definitions.
    std::vector<std::string> SymbolDefs;
    std::vector<std::string> IncludePaths;
    unsigned NoInitialTextSection : 1;
    unsigned SaveTemporaryLabels : 1;
    unsigned GenDwarfForAssembly : 1;
    unsigned RelaxELFRelocations : 1;
    unsigned DwarfVersion = 0;
    std::string DwarfDebugFlags;
    std::string DwarfDebugProducer;
    std::string DebugCompilationDir;
    llvm::DebugCompressionType CompressDebugSections = llvm::DebugCompressionType::None;
    std::string MainFileName;
    std::string InputFile = "-";
    std::vector<std::string> LLVMArgs;
    std::string OutputPath = "-";
    unsigned OutputAsmVariant = 0;
    unsigned ShowEncoding : 1;
    unsigned ShowInst : 1;
    unsigned RelaxAll : 1;
    unsigned NoExecStack : 1;
    unsigned FatalWarnings : 1;
    unsigned IncrementalLinkerCompatible : 1;
    // The name of the relocation model to use.
    std::string RelocationModel;
    AssemblerInvocation()
      : NoInitialTextSection(0),
        SaveTemporaryLabels(0),
        GenDwarfForAssembly(0),
        RelaxELFRelocations(0),
        ShowEncoding(0),
        ShowInst(0),
        RelaxAll(0),
        NoExecStack(0),
        FatalWarnings(0),
        IncrementalLinkerCompatible(0) {}
  };

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
  LogLevel logLevel;
  bool printlog;
  bool keeptmp;
  const std::string clangJobName = "clang";
  const std::string clangasJobName = "clang::as";
  const std::string linkerJobName = "amdgpu::Linker";
  const std::string clangDriverName = "clang Driver";

  template <typename T>
  inline T* AddData(T* d) { datas.push_back(d); return d; }
  void StartWithCommonArgs(std::vector<const char*>& args);
  void TransformOptionsForAssembler(const std::vector<std::string>& options, std::vector<std::string>& transformed_options);
  void ResetOptionsToDefault();
  // Filter out job arguments contradictory to in-process compilation
  ArgStringList GetJobArgsFitered(const Command& job);
  // Parse -mllvm options
  bool ParseLLVMOptions(const std::vector<std::string>& options);
  bool PrepareCompiler(CompilerInstance& clang, const Command& job);
  bool PrepareAssembler(AssemblerInvocation &Opts, const Command& job);
  bool ExecuteCompiler(CompilerInstance& clang, BackendAction action);
  bool ExecuteAssembler(AssemblerInvocation &Opts);
  bool CreateAssemblerInvocationFromArgs(AssemblerInvocation &Opts, ArrayRef<const char *> Argv);
  std::unique_ptr<raw_fd_ostream> GetAssemblerOutputStream(AssemblerInvocation &Opts, bool Binary);
  void InitDriver(std::unique_ptr<Driver>& driver);
  bool InvokeDriver(ArrayRef<const char*> args);
  bool InvokeTool(ArrayRef<const char*> args, const std::string& sToolName);
  void PrintOptions(ArrayRef<const char*> args, const std::string& sToolName, bool isInProcess);
  void PrintJobs(const JobList &jobs);
  void PrintPhase(const std::string& phase, bool isInProcess);
  bool Return(bool retValue);
  void FlushLog();
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

  void SetKeepTmp(bool bkeeptmp = true) override { keeptmp = bkeeptmp; }

  bool IsKeepTmp() override { return IsVar("AMD_OCL_KEEP_TMP", keeptmp); }

  void SetPrintLog(bool bprintlog = true) override { printlog = bprintlog; }

  bool IsPrintLog() override { return IsVar("AMD_OCL_PRINT_LOG", printlog); }

  void SetLogLevel(LogLevel ll) override { logLevel = ll; }

  LogLevel GetLogLevel() override;
};

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
  if (IsInProcess()) {
    LLVMInitializeAMDGPUAsmParser();
    LLVMInitializeAMDGPUAsmPrinter();
  }
}

std::string AMDGPUCompiler::JoinFileName(const std::string& p1, const std::string& p2) {
  std::string r;
  if (!p1.empty()) { r += p1; r += "/"; }
  r += p2;
  return r;
}

std::unique_ptr<raw_fd_ostream>
AMDGPUCompiler::GetAssemblerOutputStream(AssemblerInvocation &Opts, bool Binary) {
  if (Opts.OutputPath.empty()) {
    Opts.OutputPath = "-";
  }
  // Make sure that the Out file gets unlinked from the disk if we get a SIGINT.
  if (Opts.OutputPath != "-") {
    sys::RemoveFileOnSignal(Opts.OutputPath);
  }
  std::error_code EC;
  auto Out = llvm::make_unique<raw_fd_ostream>(Opts.OutputPath, EC, (Binary ? sys::fs::F_None : sys::fs::F_Text));
  if (EC) {
    diags.Report(diag::err_fe_unable_to_open_output) << Opts.OutputPath << EC.message();
    return nullptr;
  }
  return Out;
}

bool AMDGPUCompiler::CreateAssemblerInvocationFromArgs(AssemblerInvocation &Opts, ArrayRef<const char *> Argv) {
  bool Success = true;
  // Parse the arguments.
  std::unique_ptr<OptTable> OptTbl(createDriverOptTable());
  const unsigned IncludedFlagsBitmask = options::CC1AsOption;
  unsigned MissingArgIndex, MissingArgCount;
  InputArgList Args = OptTbl->ParseArgs(Argv, MissingArgIndex, MissingArgCount, IncludedFlagsBitmask);
  // Check for missing argument error.
  if (MissingArgCount) {
    diags.Report(diag::err_drv_missing_argument) << Args.getArgString(MissingArgIndex) << MissingArgCount;
    Success = false;
  }
  // Issue errors on unknown arguments.
  for (const Arg *A : Args.filtered(OPT_UNKNOWN)) {
    diags.Report(diag::err_drv_unknown_argument) << A->getAsString(Args);
    Success = false;
  }
  // Construct the invocation.
  // Target Options
  Opts.Triple = llvm::Triple::normalize(Args.getLastArgValue(OPT_triple));
  Opts.CPU = Args.getLastArgValue(OPT_target_cpu);
  Opts.Features = Args.getAllArgValues(OPT_target_feature);
  // Use the default target triple if unspecified.
  if (Opts.Triple.empty()) {
    Opts.Triple = llvm::sys::getDefaultTargetTriple();
  }
  // Language Options
  Opts.IncludePaths = Args.getAllArgValues(OPT_I);
  Opts.NoInitialTextSection = Args.hasArg(OPT_n);
  Opts.SaveTemporaryLabels = Args.hasArg(OPT_msave_temp_labels);
  // Any DebugInfoKind implies GenDwarfForAssembly.
  Opts.GenDwarfForAssembly = Args.hasArg(OPT_debug_info_kind_EQ);
  if (const Arg *A = Args.getLastArg(OPT_compress_debug_sections, OPT_compress_debug_sections_EQ)) {
    if (A->getOption().getID() == OPT_compress_debug_sections) {
      // TODO: be more clever about the compression type auto-detection
      Opts.CompressDebugSections = llvm::DebugCompressionType::GNU;
    } else {
      Opts.CompressDebugSections =
        llvm::StringSwitch<llvm::DebugCompressionType>(A->getValue())
        .Case("none", llvm::DebugCompressionType::None)
        .Case("zlib", llvm::DebugCompressionType::Z)
        .Case("zlib-gnu", llvm::DebugCompressionType::GNU)
        .Default(llvm::DebugCompressionType::None);
    }
  }
  Opts.RelaxELFRelocations = Args.hasArg(OPT_mrelax_relocations);
  Opts.DwarfVersion = getLastArgIntValue(Args, OPT_dwarf_version_EQ, 2, diags);
  Opts.DwarfDebugFlags = Args.getLastArgValue(OPT_dwarf_debug_flags);
  Opts.DwarfDebugProducer = Args.getLastArgValue(OPT_dwarf_debug_producer);
  Opts.DebugCompilationDir = Args.getLastArgValue(OPT_fdebug_compilation_dir);
  Opts.MainFileName = Args.getLastArgValue(OPT_main_file_name);
  // Frontend Options
  if (Args.hasArg(OPT_INPUT)) {
    bool First = true;
    for (const Arg *A : Args.filtered(OPT_INPUT)) {
      if (First) {
        Opts.InputFile = A->getValue();
        First = false;
      } else {
        diags.Report(diag::err_drv_unknown_argument) << A->getAsString(Args);
        Success = false;
      }
    }
  }
  Opts.LLVMArgs = Args.getAllArgValues(OPT_mllvm);
  Opts.OutputPath = Args.getLastArgValue(OPT_o);
  if (Arg *A = Args.getLastArg(OPT_filetype)) {
    StringRef Name = A->getValue();
    unsigned OutputType = StringSwitch<unsigned>(Name)
      .Case("asm", AssemblerInvocation::FT_Asm)
      .Case("null", AssemblerInvocation::FT_Null)
      .Case("obj", AssemblerInvocation::FT_Obj)
      .Default(~0U);
    if (OutputType == ~0U) {
      diags.Report(diag::err_drv_invalid_value) << A->getAsString(Args) << Name;
      Success = false;
    } else {
      Opts.OutputType = AssemblerInvocation::FileType(OutputType);
    }
  }
  // Transliterate Options
  Opts.OutputAsmVariant = getLastArgIntValue(Args, OPT_output_asm_variant, 0, diags);
  Opts.ShowEncoding = Args.hasArg(OPT_show_encoding);
  Opts.ShowInst = Args.hasArg(OPT_show_inst);
  // Assemble Options
  Opts.RelaxAll = Args.hasArg(OPT_mrelax_all);
  Opts.NoExecStack = Args.hasArg(OPT_mno_exec_stack);
  Opts.FatalWarnings = Args.hasArg(OPT_massembler_fatal_warnings);
  Opts.RelocationModel = Args.getLastArgValue(OPT_mrelocation_model, "pic");
  Opts.IncrementalLinkerCompatible = Args.hasArg(OPT_mincremental_linker_compatible);
  Opts.SymbolDefs = Args.getAllArgValues(OPT_defsym);
  return Success;
}

bool AMDGPUCompiler::ExecuteAssembler(AssemblerInvocation &Opts) {
  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(Opts.Triple, Error);
  if (!TheTarget) {
    return diags.Report(diag::err_target_unknown_triple) << Opts.Triple;
  }
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer = MemoryBuffer::getFileOrSTDIN(Opts.InputFile);
  if (std::error_code EC = Buffer.getError()) {
    Error = EC.message();
    return diags.Report(diag::err_fe_error_reading) << Opts.InputFile;
  }
  SourceMgr SrcMgr;
  // Tell SrcMgr about this buffer, which is what the parser will pick up.
  SrcMgr.AddNewSourceBuffer(std::move(*Buffer), SMLoc());
  // Record the location of the include directories so that the lexer can find it later.
  SrcMgr.setIncludeDirs(Opts.IncludePaths);
  std::unique_ptr<MCRegisterInfo> MRI(TheTarget->createMCRegInfo(Opts.Triple));
  assert(MRI && "Unable to create target register info!");
  std::unique_ptr<MCAsmInfo> MAI(TheTarget->createMCAsmInfo(*MRI, Opts.Triple));
  assert(MAI && "Unable to create target asm info!");
  // Ensure MCAsmInfo initialization occurs before any use, otherwise sections
  // may be created with a combination of default and explicit settings.
  MAI->setCompressDebugSections(Opts.CompressDebugSections);
  MAI->setRelaxELFRelocations(Opts.RelaxELFRelocations);
  bool IsBinary = Opts.OutputType == AssemblerInvocation::FT_Obj;
  std::unique_ptr<raw_fd_ostream> FDOS = GetAssemblerOutputStream(Opts, IsBinary);
  if (!FDOS) { return true; }
  // FIXME: This is not pretty. MCContext has a ptr to MCObjectFileInfo and
  // MCObjectFileInfo needs a MCContext reference in order to initialize itself.
  std::unique_ptr<MCObjectFileInfo> MOFI(new MCObjectFileInfo());
  MCContext Ctx(MAI.get(), MRI.get(), MOFI.get(), &SrcMgr);
  bool PIC = false;
  if (Opts.RelocationModel == "static") {
    PIC = false;
  } else if (Opts.RelocationModel == "pic") {
    PIC = true;
  } else {
    assert(Opts.RelocationModel == "dynamic-no-pic" && "Invalid PIC model!");
    PIC = false;
  }
  MOFI->InitMCObjectFileInfo(llvm::Triple(Opts.Triple), PIC, Ctx);
  if (Opts.SaveTemporaryLabels) { Ctx.setAllowTemporaryLabels(false); }
  if (Opts.GenDwarfForAssembly) { Ctx.setGenDwarfForAssembly(true); }
  if (!Opts.DwarfDebugFlags.empty()) { Ctx.setDwarfDebugFlags(StringRef(Opts.DwarfDebugFlags)); }
  if (!Opts.DwarfDebugProducer.empty()) { Ctx.setDwarfDebugProducer(StringRef(Opts.DwarfDebugProducer)); }
  if (!Opts.DebugCompilationDir.empty()) { Ctx.setCompilationDir(Opts.DebugCompilationDir); }
  if (!Opts.MainFileName.empty()) { Ctx.setMainFileName(StringRef(Opts.MainFileName)); }
  Ctx.setDwarfVersion(Opts.DwarfVersion);
  // Build up the feature string from the target feature list.
  std::string FS;
  if (!Opts.Features.empty()) {
    FS = Opts.Features[0];
    for (unsigned i = 1, e = Opts.Features.size(); i != e; ++i) {
      FS += "," + Opts.Features[i];
    }
  }
  std::unique_ptr<MCStreamer> Str;
  std::unique_ptr<MCInstrInfo> MCII(TheTarget->createMCInstrInfo());
  std::unique_ptr<MCSubtargetInfo> STI(TheTarget->createMCSubtargetInfo(Opts.Triple, Opts.CPU, FS));
  raw_pwrite_stream *Out = FDOS.get();
  std::unique_ptr<buffer_ostream> BOS;
  // FIXME: There is a bit of code duplication with addPassesToEmitFile.
  if (Opts.OutputType == AssemblerInvocation::FT_Asm) {
    MCInstPrinter *IP = TheTarget->createMCInstPrinter(llvm::Triple(Opts.Triple), Opts.OutputAsmVariant, *MAI, *MCII, *MRI);
    std::unique_ptr<MCCodeEmitter> MCE;
    std::unique_ptr<MCAsmBackend> MAB;
    if (Opts.ShowEncoding) {
      MCE.reset(TheTarget->createMCCodeEmitter(*MCII, *MRI, Ctx));
      MCTargetOptions Options;
      MAB.reset(TheTarget->createMCAsmBackend(*STI, *MRI, Options));
    }
    auto FOut = llvm::make_unique<formatted_raw_ostream>(*Out);
    Str.reset(TheTarget->createAsmStreamer(Ctx, std::move(FOut),
        /*asmverbose*/ true, /*useDwarfDirectory*/ true, IP, std::move(MCE),
        std::move(MAB), Opts.ShowInst));
  } else if (Opts.OutputType == AssemblerInvocation::FT_Null) {
    Str.reset(createNullStreamer(Ctx));
  } else {
    assert(Opts.OutputType == AssemblerInvocation::FT_Obj && "Invalid file type!");
    if (!FDOS->supportsSeeking()) {
      BOS = make_unique<buffer_ostream>(*FDOS);
      Out = BOS.get();
    }
    MCCodeEmitter *CE = TheTarget->createMCCodeEmitter(*MCII, *MRI, Ctx);
    MCTargetOptions Options;
    MCAsmBackend *MAB = TheTarget->createMCAsmBackend(*STI, *MRI, Options);
    llvm::Triple T(Opts.Triple);
    Str.reset(TheTarget->createMCObjectStreamer(T, Ctx,
      std::unique_ptr<MCAsmBackend>(MAB), MAB->createObjectWriter(*Out),
      std::unique_ptr<MCCodeEmitter>(CE), *STI, Opts.RelaxAll,
      Opts.IncrementalLinkerCompatible, /*DWARFMustBeAtTheEnd*/ true));
    Str.get()->InitSections(Opts.NoExecStack);
  }
  bool Failed = false;
  std::unique_ptr<MCAsmParser> Parser(createMCAsmParser(SrcMgr, Ctx, *Str.get(), *MAI));
  // FIXME: init MCTargetOptions from sanitizer flags here.
  MCTargetOptions Options;
  std::unique_ptr<MCTargetAsmParser> TAP(TheTarget->createMCAsmParser(*STI, *Parser, *MCII, Options));
  if (!TAP) {
    Failed = diags.Report(diag::err_target_unknown_triple) << Opts.Triple;
  }
  // Set values for symbols, if any.
  for (auto &S : Opts.SymbolDefs) {
    auto Pair = StringRef(S).split('=');
    auto Sym = Pair.first;
    auto Val = Pair.second;
    int64_t Value;
    // We have already error checked this in the driver.
    Val.getAsInteger(0, Value);
    Ctx.setSymbolValue(Parser->getStreamer(), Sym, Value);
  }
  if (!Failed) {
    Parser->setTargetParser(*TAP.get());
    Failed = Parser->Run(Opts.NoInitialTextSection);
  }
  // Close Streamer first. It might have a reference to the output stream.
  Str.reset();
  // Close the output stream early.
  BOS.reset();
  FDOS.reset();
  // Delete output file if there were errors.
  if (Failed && Opts.OutputPath != "-") {
    sys::fs::remove(Opts.OutputPath);
  }
  return Failed;
}

bool AMDGPUCompiler::ExecuteCompiler(CompilerInstance& clang, BackendAction action) {
  std::unique_ptr<CodeGenAction> Act;
  switch (action) {
    case Backend_EmitBC:
      Act = std::unique_ptr<CodeGenAction>(new EmitBCAction());
      break;
    case Backend_EmitObj:
      Act = std::unique_ptr<CodeGenAction>(new EmitObjAction());
      break;
    default: { return false; }
  }
  if (!Act.get()) { return false; }
  if (!clang.ExecuteAction(*Act)) { return false; }
  return true;
}

void AMDGPUCompiler::InitDriver(std::unique_ptr<Driver>& driver) {
  driver->CCPrintOptions = !!::getenv("CC_PRINT_OPTIONS");
  driver->setTitle("AMDGPU OpenCL driver");
  driver->setCheckInputsExist(false);
}

void AMDGPUCompiler::StartWithCommonArgs(std::vector<const char*>& args) {
  // Workaround for flawed Driver::BuildCompilation(...) implementation,
  // which eliminates 1st argument, cause it actually awaits argv[0].
  args.clear();
  args.push_back("");
}

ArgStringList AMDGPUCompiler::GetJobArgsFitered(const Command& job) {
  std::string arg;
  std::string sJobName(job.getCreator().getName());
  if (sJobName == clangJobName) {
    arg = "-disable-free";
  } else if (sJobName == clangasJobName) {
    arg = "-cc1as";
  } else {
    return std::move(job.getArguments());
  }
  ArgStringList args(job.getArguments());
  ArgStringList::iterator it = std::find(args.begin(), args.end(), arg);
  if (it != args.end()) {
    args.erase(it);
  }
  return args;
}

bool AMDGPUCompiler::ParseLLVMOptions(const std::vector<std::string>& options) {
  if (options.empty()) { return true; }
  std::vector<const char*> args;
  for (auto A : options) {
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
  clang.createDiagnostics();
  if (!clang.hasDiagnostics()) { return false; }
  ResetOptionsToDefault();
  ArgStringList args = GetJobArgsFitered(job);
  if (!CompilerInvocation::CreateFromArgs(clang.getInvocation(),
    const_cast<const char**>(args.data()),
    const_cast<const char**>(args.data()) + args.size(),
    clang.getDiagnostics())) { return false; }
  if (!ParseLLVMOptions(clang.getFrontendOpts().LLVMArgs)) { return false; }
  return true;
}

bool AMDGPUCompiler::PrepareAssembler(AssemblerInvocation &Opts, const Command& job) {
  ResetOptionsToDefault();
  if (!CreateAssemblerInvocationFromArgs(Opts, llvm::makeArrayRef(GetJobArgsFitered(job)))) { return false; }
  if (diags.hasErrorOccurred()) { return false; }
  if (!ParseLLVMOptions(Opts.LLVMArgs)) { return false; }
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

void AMDGPUCompiler::FlushLog() {
  if (!IsPrintLog())
    return;
  static std::mutex m_screen;
  m_screen.lock();
  std::cout << Output();
  m_screen.unlock();
}

bool AMDGPUCompiler::Return(bool retValue) {
  FlushLog();
  return retValue;
}

void AMDGPUCompiler::PrintJobs(const JobList &jobs) {
  if (GetLogLevel() < LL_VERBOSE || jobs.empty()) { return; }
  OS << "\n[AMD OCL] " << jobs.size() << " job" << (jobs.size() == 1 ? "" : "s") << ":\n";
  int i = 1;
  for (auto const & J : jobs) {
    std::string sJobName(J.getCreator().getName());
    OS << (i > 1 ? "\n" : "") << "  JOB [" << i << "] " << sJobName << "\n";
    for (auto A : GetJobArgsFitered(J)) {
      OS << "      " << A << "\n";
    }
    ++i;
  }
  OS << "\n";
  FlushLog();
}

void AMDGPUCompiler::PrintPhase(const std::string& phase, bool isInProcess) {
  if (GetLogLevel() < LL_VERBOSE) { return; }
  OS << "\n[AMD OCL] " << "Phase: " << phase << (isInProcess ? " [in-process]" : "") << "\n";
  FlushLog();
}

void AMDGPUCompiler::PrintOptions(ArrayRef<const char*> args, const std::string& sToolName, bool isInProcess) {
  if (GetLogLevel() < LL_VERBOSE) { return; }
  OS << "\n[AMD OCL] " << sToolName << (isInProcess ? " [in-process]" : "") << " options:\n";
  for (const char* A : args) {
    OS << "      " << A << "\n";
  }
  OS << "\n";
  FlushLog();
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
    logLevel(LL_ERRORS),
    printlog(false),
    keeptmp(false) {
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUDisassembler();
  if (IsInProcess()) {
    LLVMInitializeAMDGPUAsmParser();
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
  Optional<StringRef> Redirects[] =
      {None, StringRef(out->Name()), StringRef(err->Name())};
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
  Optional<StringRef> Redirects[] =
      {None, StringRef(out->Name()), StringRef(err->Name())};
  Optional<ArrayRef<StringRef>> Env;
  auto Args = llvm::toStringRefArray(args1.data());
  int res = llvm::sys::ExecuteAndWait(sToolName, Args, Env, Redirects);
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
  StartWithCommonArgs(args);
  if (input->Type() == DT_ASSEMBLY) {
    return false;
  } else {
    args.push_back("-x");
    args.push_back("cl");
  }
  args.push_back("-c");
  args.push_back("-emit-llvm");
  FileReference* inputFile = ToInputFile(input, CompilerTempDir());
  if (!inputFile)
    return Return(false);
  args.push_back(inputFile->Name().c_str());

  File* bcFile = ToOutputFile(output, CompilerTempDir());
  if (!bcFile)
    return Return(false);

  args.push_back("-o");
  args.push_back(bcFile->Name().c_str());
  for (const std::string& s : options) {
    args.push_back(s.c_str());
  }
  PrintOptions(args, clangDriverName, IsInProcess());
  if (IsInProcess()) {
    std::unique_ptr<Driver> driver(new Driver("", STRING(AMDGCN_TRIPLE), diags));
    InitDriver(driver);
    std::unique_ptr<Compilation> C(driver->BuildCompilation(args));
    const JobList &Jobs = C->getJobs();
    PrintJobs(Jobs);
    CompilerInstance Clang;
    if (!PrepareCompiler(Clang, *Jobs.begin())) { return Return(false); }
    if (!ExecuteCompiler(Clang, Backend_EmitBC)) { return Return(false); }
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
  PrintPhase("LinkLLVMBitcode", IsInProcess());
  std::vector<const char*> args;
  for (Data* input : inputs) {
    FileReference* inputFile = ToInputFile(input, CompilerTempDir());
    args.push_back(inputFile->Name().c_str());
  }
  File* outputFile = ToOutputFile(output, CompilerTempDir());
  if (!options.empty()) {
    std::vector<const char*> argv;
    for (auto option : options) {
      if (IsInProcess()) {
        argv.push_back("");
        argv.push_back(option.c_str());
        if (!cl::ParseCommandLineOptions(argv.size(), &argv[0], "llvm linker")) {
          return Return(false);
        }
        argv.clear();
      } else {
        args.push_back(option.c_str());
      }
    }
  }
  if (!IsInProcess()) {
    args.push_back("-o");
    args.push_back(outputFile->Name().c_str());
  }
  if (IsInProcess()) {
    PrintOptions(args, "llvm linker", IsInProcess());
    LLVMContext context;
    context.setDiagnosticHandler(
        llvm::make_unique<AMDGPUCompilerDiagnosticHandler>(this), true);
    auto Composite = make_unique<llvm::Module>("composite", context);
    Linker L(*Composite);
    unsigned ApplicableFlags = Linker::Flags::None;
    for (auto arg : args) {
      SMDiagnostic error;
      auto m = getLazyIRFileModule(arg, error, context);
      if (!m.get()) {
        return Return(EmitLinkerError(context, "The module '" + Twine(arg) + "' loading failed."));
      }
      if (verifyModule(*m, &errs())) {
        return Return(EmitLinkerError(context, "The loaded module '" + Twine(arg) + "' to link is broken."));
      }
      if (GetLogLevel() >= LL_LLVM_ONLY) {
        OS << "[AMD OCL] Linking in '" << arg << "'" << "\n";
      }
      if (L.linkInModule(std::move(m), ApplicableFlags)) {
        return Return(EmitLinkerError(context, "The module '" + Twine(arg) + "' is not linked."));
      }
    }
    if (verifyModule(*Composite, &errs())) {
      return Return(EmitLinkerError(context, "The linked module '" + outputFile->Name() + "' is broken."));
    }
    std::error_code ec;
    llvm::ToolOutputFile out(outputFile->Name(), ec, sys::fs::F_None);
    WriteBitcodeToFile(*Composite.get(), out.os());
    out.keep();
  } else {
    if (!InvokeTool(args, llvmLinkExe)) { return Return(false); }
  }
  return Return(output->ReadOutputFile(outputFile));
}

void AMDGPUCompiler::TransformOptionsForAssembler(const std::vector<std::string>& options, std::vector<std::string>& transformed_options) {
  transformed_options.push_back("-x");
  transformed_options.push_back("assembler");
  transformed_options.push_back("-integrated-as");
  const std::string swa = "-Wa,";
  const std::string sdef = swa + "-defsym,";
  const std::string sinc = swa +"-I,";
  std::string transformed_option;
  bool bcompound = false;
  bool btransform = false;
  for (auto &option : options) {
    btransform = false;
    if (bcompound) {
      transformed_option += option;
      bcompound = false;
      btransform = true;
    } else {
      if (option.find("-D") == 0) {
        transformed_option = sdef;
        btransform = true;
      } else if (option.find("-I") == 0) {
        transformed_option = sinc;
        btransform = true;
      }
      if (btransform) {
        if (option.size() > 2) {
          transformed_option += option.substr(2);
        } else {
          bcompound = true;
          continue;
        }
      }
    }
    if (btransform) {
      transformed_options.push_back(transformed_option);
    } else {
      transformed_options.push_back(option);
    }
  }
}

bool AMDGPUCompiler::CompileAndLinkExecutable(Data* input, Data* output, const std::vector<std::string>& options) {
  PrintPhase("CompileAndLinkExecutable", IsInProcess());
  std::vector<const char*> args;
  StartWithCommonArgs(args);
  FileReference* inputFile = ToInputFile(input, CompilerTempDir());
  args.push_back(inputFile->Name().c_str());
  File* outputFile = ToOutputFile(output, CompilerTempDir());
  args.push_back("-o"); args.push_back(outputFile->Name().c_str());
  std::vector<std::string> transformed_options;
  const std::vector<std::string>* opts = &options;
  if (input->Type() == DT_ASSEMBLY) {
    TransformOptionsForAssembler(options, transformed_options);
    opts = &transformed_options;
  }
  for (auto &option : *opts) {
    args.push_back(option.c_str());
  }
  PrintOptions(args, clangDriverName, IsInProcess());
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
        if (i == 1 && (sJobName == clangJobName || sJobName == clangasJobName)) {
          switch (input->Type()) {
            case DT_ASSEMBLY: {
              AssemblerInvocation Asm;
              if (!PrepareAssembler(Asm, J)) { return Return(false); }
              if (ExecuteAssembler(Asm)) { return Return(false); }
              break;
            }
            default: {
              CompilerInstance Clang;
              if (!PrepareCompiler(Clang, J)) { return Return(false); }
              if (!ExecuteCompiler(Clang, Backend_EmitObj)) { return Return(false); }
              break;
            }
          }
        } else if (i == 2 && sJobName == linkerJobName) {
          llvm::opt::ArgStringList Args(J.getArguments());
          Args.insert(Args.begin(), "");
          ArrayRef<const char*> ArgRefs = llvm::makeArrayRef(Args);
          static std::mutex m_screen;
          m_screen.lock();
          bool lldRet = lld::elf::link(ArgRefs, false, OS);
          m_screen.unlock();
          if (!lldRet) { return Return(false); }
        } else { return Return(false); }
        i++;
      } else { return Return(false); }
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
  Triple TheTriple(STRING(AMDGCN_TRIPLE));
  const std::string TripleStr = TheTriple.normalize();

  StringRef execRef(exec->Ptr(), exec->Size());
  std::unique_ptr<MemoryBuffer> execBuffer(MemoryBuffer::getMemBuffer(execRef, "", false));
  Expected<std::unique_ptr<Binary>> execBinary = createBinary(*execBuffer);
  if (!execBinary) { return false; }
  std::unique_ptr<Binary> Binary(execBinary.get().release());
  if (!Binary) { return false; }
  // setup context

  std::string Error;
  const Target *TheTarget = llvm::TargetRegistry::lookupTarget(TripleStr,
                                                               Error);
  if (!TheTarget)
      report_fatal_error("Could not lookup target: " + Twine(Error));

  std::unique_ptr<MCRegisterInfo> MRI(TheTarget->createMCRegInfo(TripleStr));
  if (!MRI) { return false; }
  std::unique_ptr<MCAsmInfo> AsmInfo(TheTarget->createMCAsmInfo(*MRI, TripleStr));
  if (!AsmInfo) { return false; }
  std::unique_ptr<MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) { return false; }
  MCObjectFileInfo MOFI;
  MCContext Ctx(AsmInfo.get(), MRI.get(), &MOFI);
  MOFI.InitMCObjectFileInfo(TheTriple, false, Ctx);
  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  MCInstPrinter *IP(TheTarget->createMCInstPrinter(TheTriple,
                                                  AsmPrinterVariant,
                                                  *AsmInfo, *MII, *MRI));
  if (!IP) { report_fatal_error("error: no instruction printer"); }
  std::error_code EC;
  raw_fd_ostream FO(dump->Name(), EC, sys::fs::F_None);
  auto FOut = make_unique<formatted_raw_ostream>(FO);
  std::unique_ptr<MCStreamer> MCS(
    TheTarget->createAsmStreamer(Ctx, std::move(FOut), true, false, IP,
                                nullptr, nullptr, false));
  CodeObjectDisassembler CODisasm(&Ctx, TripleStr, IP, MCS->getTargetStreamer());
  EC = CODisasm.Disassemble(Binary->getMemoryBufferRef(), errs());
  if (EC) { return false; }
  return true;
}

Compiler* CompilerFactory::CreateAMDGPUCompiler(const std::string& llvmBin) {
  return new AMDGPUCompiler(llvmBin);
}

}
}

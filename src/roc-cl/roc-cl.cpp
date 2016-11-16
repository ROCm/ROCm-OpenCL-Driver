#include <string>
#include "AmdCompiler.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

enum ActionType {
  AC_NotSet,
  AC_CompileToLLVMBitcode,
  AC_LinkLLVMBitcode,
  AC_CompileAndLinkExecutable,
};

static cl::opt<ActionType>
Action(cl::desc("Action to perform:"),
       cl::init(AC_NotSet),
       cl::values(
         clEnumValN(AC_CompileToLLVMBitcode, "compile_to_llvm", "Compile to LLVM bitcode"),
         clEnumValN(AC_LinkLLVMBitcode, "link_llvm", "Link LLVM bitcode"),
         clEnumValN(AC_CompileAndLinkExecutable, "compile_and_link", "Compile and link executable")
       )
      );

static cl::opt<std::string>
LLVMBin("llvmbin", cl::desc("LLVM binary directory"));

static cl::list<std::string>
InputFilenames(cl::Positional, cl::desc("<input files>"),cl::ZeroOrMore);

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"),
               cl::value_desc("filename"));

static cl::list<std::string>
OtherOptions(cl::Sink, cl::desc("<other options>"));

using namespace amd::opencl_driver;
using namespace llvm;

int main(int argc, char* argv[])
{
  cl::ParseCommandLineOptions(argc, argv, "AMD OpenCL Compiler");

  CompilerFactory compilerFactory;

  std::unique_ptr<Compiler> compiler(compilerFactory.CreateAMDGPUCompiler(LLVMBin));

  std::vector<Data*> inputs;

  for (const std::string& inputFile : InputFilenames) {
    inputs.push_back(compiler->NewFileReference(DT_CL, inputFile));
  }
  
  std::vector<std::string> options = OtherOptions;
  Data* output;

  bool res;

  switch (Action) {
  case AC_NotSet:
  default:
    errs() << "Error: action is not specified.\n"; res = false;
    break;
  case AC_CompileToLLVMBitcode:
    output = compiler->NewFile(DT_LLVM_BC, OutputFilename);
    res = compiler->CompileToLLVMBitcode(inputs, output, options);
    break;
  case AC_LinkLLVMBitcode:
    output = compiler->NewFile(DT_LLVM_BC, OutputFilename);
    res = compiler->LinkLLVMBitcode(inputs, output, options);
    break;
  case AC_CompileAndLinkExecutable:
    output = compiler->NewFile(DT_EXECUTABLE, OutputFilename);
    res = compiler->CompileAndLinkExecutable(inputs, output, options);
    break;
  }

  std::string compilerOutput = compiler->Output();
  if (!compilerOutput.empty()) {
    outs() << compilerOutput << '\n';
  }

  return res ? 0 : 1;
}

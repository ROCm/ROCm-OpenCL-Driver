#include <iostream>
#include "gtest/gtest.h"
#include "AmdCompiler.h"

using namespace amd::opencl_driver;

static std::string joinf(const std::string& p1, const std::string& p2)
{
  std::string r;
  if (!p1.empty()) { r += p1; r += "/"; }
  r += p2;
  return r;
}

class AMDGPUCompilerTest : public ::testing::Test {
protected:
  std::string llvmBin;
  std::string testDir;
  std::vector<std::string> emptyOptions;
  std::vector<std::string> defaultOptions;

  virtual void SetUp() {
    ASSERT_NE(getenv("LLVM_BIN"), nullptr);
    llvmBin = getenv("LLVM_BIN");
    ASSERT_NE(getenv("TEST_DIR"), nullptr);
    testDir = getenv("TEST_DIR");
    compiler = compilerFactory.CreateAMDGPUCompiler(llvmBin);
    defaultOptions.push_back("-cl-std=CL1.2");
  }

  virtual void TearDown() {
    std::string compilerOutput = compiler->Output();
    if (!compilerOutput.empty()) {
      std::cout << "Compiler output:" << std::endl
                << compilerOutput << std::endl;
    }
    delete compiler;
  }

  FileReference* TestDirInputFile(DataType type, const std::string& name) {
    return compiler->NewFileReference(type, joinf(testDir, name));
  }

  File* TmpOutputFile(DataType type) {
    return compiler->NewTempFile(type);
  }

  Data* NewClSource(const char* s) {
    return compiler->NewBufferReference(DT_CL, s, strlen(s));
  }

  CompilerFactory compilerFactory;
  Compiler* compiler;
};

static const std::string simpleCl = "simple.cl";
static const std::string externFunction1Cl = "extern_function1.cl";
static const std::string externFunction2Cl = "extern_function2.cl";
static const std::string outBc = "out.bc";
static const std::string out1Bc = "out1.bc";
static const std::string out2Bc = "out2.bc";
static const std::string outCo = "out.co";

static const char* simpleSource =
"kernel void test_kernel(global int* out)              \n"
"{                                                     \n"
"  out[0] = 4;                                         \n"
"}                                                     \n"
;

static const char* externFunction1 =
"extern int test_function();                           \n"
"                                                      \n"
"kernel void test_kernel(global int* out)              \n"
"{                                                     \n"
"  out[0] = test_function();                           \n"
"}                                                     \n"
;

static const char* externFunction2 =
"int test_function()                                   \n"
"{                                                     \n"
"  return 5;                                           \n"
"}                                                     \n"
;

static const char* includer = 
"#include \"include.h\"                                \n"
"                                                      \n"
"kernel void test_kernel(global int* out)              \n"
"{                                                     \n"
"  out[0] = test_function();                           \n"
"}                                                     \n"
;

static const char* include =
"int test_function()                                   \n"
"{                                                     \n"
"  return 5;                                           \n"
"}                                                     \n"
;

static const char* includeInvalid =
"Invalid include file (should not be used)."
;

static const char* defined =
"kernel void test_kernel(global int* out)              \n"
"{                                                     \n"
"  out[0] = DEF;                                       \n"
"}                                                     \n"
;

static const char* invalidCL =
"kernel void test() { ExpectedErrorInCLSource; }       \n"
;

static const char* invalidBC=
"ExpectedInvalidBitcode                                \n"
;

TEST_F(AMDGPUCompilerTest, OutputEmpty)
{
  EXPECT_EQ(compiler->Output().length(), 0U);
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_File_To_File)
{
  FileReference* f = TestDirInputFile(DT_CL, simpleCl);
  ASSERT_NE(f, nullptr);
  File* out = TmpOutputFile(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(f);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, defaultOptions));
  ASSERT_TRUE(out->Exists());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_Buffer_To_Buffer)
{
  Data* src = NewClSource(simpleSource);
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_Include_I1)
{
  Data* src = NewClSource(includer);
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  std::vector<std::string> options;
  options.push_back("-I");
  options.push_back(joinf(testDir, "include"));
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, options));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_Include_I2)
{
  Data* src = NewClSource(includer);
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  std::vector<std::string> options;
  options.push_back("-I" + joinf(testDir, "include"));
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, options));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLinkExecutable_File_To_File)
{
  FileReference* f = TestDirInputFile(DT_CL, simpleCl);
  ASSERT_NE(f, nullptr);
  File* out = TmpOutputFile(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(f);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(out->Exists());
}

TEST_F(AMDGPUCompilerTest, CompileAndLinkExecutable_Buffer_To_Buffer)
{
  Data* src = NewClSource(simpleSource);
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_CLs_File_To_File)
{
  std::vector<Data*> inputs;
  FileReference* ef1 = TestDirInputFile(DT_CL, externFunction1Cl);
  ASSERT_NE(ef1, nullptr);
  FileReference* ef2 = TestDirInputFile(DT_CL, externFunction2Cl);
  ASSERT_NE(ef2, nullptr);

  inputs.push_back(ef1);
  inputs.push_back(ef2);
  File* out = TmpOutputFile(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(out->Exists());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_CLs_Buffer_To_Buffer)
{
  std::vector<Data*> inputs;
  Data* src1 = NewClSource(externFunction1);
  ASSERT_NE(src1, nullptr);
  Data* src2 = NewClSource(externFunction2);
  ASSERT_NE(src2, nullptr);

  inputs.push_back(src1);
  inputs.push_back(src2);
  Buffer* out = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, LinkLLVMBitcode_File_To_File)
{
  std::vector<Data*> inputs;
  FileReference* ef1 = TestDirInputFile(DT_CL, externFunction1Cl);
  ASSERT_NE(ef1, nullptr);
  FileReference* ef2 = TestDirInputFile(DT_CL, externFunction2Cl);
  ASSERT_NE(ef2, nullptr);

  inputs.clear();
  inputs.push_back(ef1);
  File* out1 = TmpOutputFile(DT_LLVM_BC);
  ASSERT_NE(out1, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out1, defaultOptions));
  ASSERT_TRUE(out1->Exists());

  inputs.clear();
  inputs.push_back(ef2);
  File* out2 = TmpOutputFile(DT_LLVM_BC);
  ASSERT_NE(out2, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out2, defaultOptions));
  ASSERT_TRUE(out2->Exists());

  inputs.clear();
  inputs.push_back(out1);
  inputs.push_back(out2);
  File* out = TmpOutputFile(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->LinkLLVMBitcode(inputs, out, emptyOptions));
  ASSERT_TRUE(out->Exists());
}

TEST_F(AMDGPUCompilerTest, LinkLLVMBitcode_Buffer_To_Buffer)
{
  std::vector<Data*> inputs;
  Data* src1 = NewClSource(externFunction1);
  ASSERT_NE(src1, nullptr);
  Data* src2 = NewClSource(externFunction2);
  ASSERT_NE(src2, nullptr);

  inputs.clear();
  inputs.push_back(src1);
  Buffer* out1 = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out1, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out1, defaultOptions));
  ASSERT_TRUE(!out1->IsEmpty());

  inputs.clear();
  inputs.push_back(src2);
  Buffer* out2 = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out2, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out2, defaultOptions));
  ASSERT_TRUE(!out2->IsEmpty());

  inputs.clear();
  inputs.push_back(out1);
  inputs.push_back(out2);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->LinkLLVMBitcode(inputs, out, emptyOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_BCs_File_To_File)
{
  std::vector<Data*> inputs;
  FileReference* ef1 = TestDirInputFile(DT_CL, externFunction1Cl);
  ASSERT_NE(ef1, nullptr);
  FileReference* ef2 = TestDirInputFile(DT_CL, externFunction2Cl);
  ASSERT_NE(ef2, nullptr);

  inputs.clear();
  inputs.push_back(ef1);
  File* out1 = TmpOutputFile(DT_LLVM_BC);
  ASSERT_NE(out1, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out1, defaultOptions));
  ASSERT_TRUE(out1->Exists());

  inputs.clear();
  inputs.push_back(ef2);
  File* out2 = TmpOutputFile(DT_LLVM_BC);
  ASSERT_NE(out2, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out2, defaultOptions));
  ASSERT_TRUE(out2->Exists());

  inputs.clear();
  inputs.push_back(out1);
  inputs.push_back(out2);
  File* out = TmpOutputFile(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(out->Exists());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_BCs_Buffer_To_Buffer)
{
  std::vector<Data*> inputs;
  Data* src1 = NewClSource(externFunction1);
  ASSERT_NE(src1, nullptr);
  Data* src2 = NewClSource(externFunction2);
  ASSERT_NE(src2, nullptr);

  inputs.clear();
  inputs.push_back(src1);
  Buffer* out1 = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out1, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out1, defaultOptions));
  ASSERT_TRUE(!out1->IsEmpty());

  inputs.clear();
  inputs.push_back(src2);
  Buffer* out2 = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out2, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out2, defaultOptions));
  ASSERT_TRUE(!out2->IsEmpty());

  inputs.clear();
  inputs.push_back(out1);
  inputs.push_back(out2);
  Buffer* out = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_Linked_Buffer_To_Buffer)
{
  std::vector<Data*> inputs;
  Data* src1 = NewClSource(externFunction1);
  ASSERT_NE(src1, nullptr);
  Data* src2 = NewClSource(externFunction2);
  ASSERT_NE(src2, nullptr);

  inputs.clear();
  inputs.push_back(src1);
  Buffer* out1 = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out1, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out1, defaultOptions));
  ASSERT_TRUE(!out1->IsEmpty());

  inputs.clear();
  inputs.push_back(src2);
  Buffer* out2 = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out2, nullptr);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out2, defaultOptions));
  ASSERT_TRUE(!out2->IsEmpty());

  inputs.clear();
  inputs.push_back(out1);
  inputs.push_back(out2);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->LinkLLVMBitcode(inputs, out, emptyOptions));
  ASSERT_TRUE(!out->IsEmpty());

  inputs.clear();
  inputs.push_back(out);
  Buffer* out3 = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out1, nullptr);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out3, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}


TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_EmbeddedInclude)
{
  Data* src = NewClSource(includer);
  ASSERT_NE(src, nullptr);
  Data* inc = compiler->NewBufferReference(DT_CL_HEADER, include, strlen(include), "include.h");
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  inputs.push_back(inc);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_EmbeddedInclude)
{
  Data* src = NewClSource(includer);
  ASSERT_NE(src, nullptr);
  Data* inc = compiler->NewBufferReference(DT_CL_HEADER, include, strlen(include), "include.h");
  Buffer* out = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  inputs.push_back(inc);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_EmbeddedIncludeOverride)
{
  Data* src = NewClSource(includer);
  ASSERT_NE(src, nullptr);
  Data* inc = compiler->NewBufferReference(DT_CL_HEADER, include, strlen(include), "include.h");
  Data* inc2 = compiler->NewBufferReference(DT_CL_HEADER, includeInvalid, strlen(includeInvalid), "include.h");
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  inputs.push_back(inc);
  inputs.push_back(inc2);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_EmbeddedIncludeOverride)
{
  Data* src = NewClSource(includer);
  ASSERT_NE(src, nullptr);
  Data* inc = compiler->NewBufferReference(DT_CL_HEADER, include, strlen(include), "include.h");
  Data* inc2 = compiler->NewBufferReference(DT_CL_HEADER, includeInvalid, strlen(includeInvalid), "include.h");
  Buffer* out = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  inputs.push_back(inc);
  inputs.push_back(inc2);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_Define1)
{
  Data* src = compiler->NewBufferReference(DT_CL, defined, strlen(defined));
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  std::vector<std::string> options;
  options.push_back("-DDEF=10");
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, options));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_Define1)
{
  Data* src = compiler->NewBufferReference(DT_CL, defined, strlen(defined));
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  std::vector<std::string> options;
  options.push_back("-DDEF=10");
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, options));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_Define2)
{
  Data* src = compiler->NewBufferReference(DT_CL, defined, strlen(defined));
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  std::vector<std::string> options;
  options.push_back("-D");
  options.push_back("DEF=10");
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, options));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_Define2)
{
  Data* src = compiler->NewBufferReference(DT_CL, defined, strlen(defined));
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_EXECUTABLE);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  std::vector<std::string> options;
  options.push_back("-D");
  options.push_back("DEF=10");
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, options));
  ASSERT_TRUE(!out->IsEmpty());
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_Error_InvalidCL)
{
  Data* src = compiler->NewBufferReference(DT_CL, invalidCL, strlen(invalidCL));
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  ASSERT_FALSE(compiler->CompileToLLVMBitcode(inputs, out, defaultOptions));
  ASSERT_TRUE(out->IsEmpty());
  ASSERT_TRUE(!compiler->Output().empty());
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_Error_InvalidCL)
{
  Data* src = compiler->NewBufferReference(DT_CL, invalidCL, strlen(invalidCL));
  ASSERT_NE(src, nullptr);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(src);
  ASSERT_FALSE(compiler->CompileAndLinkExecutable(inputs, out, defaultOptions));
  ASSERT_TRUE(out->IsEmpty());
  ASSERT_TRUE(!compiler->Output().empty());
}

TEST_F(AMDGPUCompilerTest, LinkLLVMBitcode_Error_InvalidBC)
{
  Data* src = compiler->NewBufferReference(DT_LLVM_BC, invalidBC, strlen(invalidBC));
  std::vector<Data*> inputs;

  inputs.push_back(src);
  inputs.push_back(src);
  Buffer* out = compiler->NewBuffer(DT_LLVM_BC);
  ASSERT_NE(out, nullptr);
  ASSERT_FALSE(compiler->LinkLLVMBitcode(inputs, out, emptyOptions));
  ASSERT_TRUE(out->IsEmpty());
  ASSERT_TRUE(!compiler->Output().empty());
}

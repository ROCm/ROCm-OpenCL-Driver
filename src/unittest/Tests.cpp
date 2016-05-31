#include "gtest/gtest.h"

#include "AmdCompiler.h"

using namespace amd;

std::string joinf(const std::string& p1, const std::string& p2)
{
  std::string r;
  if (!p1.empty()) { r += p1; r += "/"; }
  r += p2;
  return r;
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

bool FileExists(File* f)
{
  return FileExists(f->Name());
}

class AMDGPUCompilerTest : public ::testing::Test {
protected:
  std::string llvmBin;
  std::string testDir;
  std::string tmpDir;
  std::vector<std::string> emptyOptions;

  virtual void SetUp() {
    ASSERT_NE(getenv("LLVM_BIN"), nullptr);
    llvmBin = getenv("LLVM_BIN");
    ASSERT_NE(getenv("TEST_DIR"), nullptr);
    testDir = getenv("TEST_DIR");
    ASSERT_NE(getenv("TMP_DIR"), nullptr);
    tmpDir = getenv("TMP_DIR");
    compiler = driver.CreateAMDGPUCompiler(llvmBin);
  }

  virtual void TearDown() {
    delete compiler;
  }

  File* TestDirInputFile(DataType type, const std::string& name) {
    return compiler->NewInputFile(type, joinf(testDir, name));
  }

  File* TmpOutputFile(DataType type, const std::string& name) {
    return compiler->NewOutputFile(type, joinf(tmpDir, name));
  }

  CompilerDriver driver;
  Compiler* compiler;
};

static const std::string simpleCl = "simple.cl";
static const std::string externFunction1Cl = "extern_function1.cl";
static const std::string externFunction2Cl = "extern_function2.cl";
static const std::string outBc = "out.bc";
static const std::string out1Bc = "out1.bc";
static const std::string out2Bc = "out2.bc";
static const std::string outCo = "out.co";

TEST_F(AMDGPUCompilerTest, OutputEmpty)
{
  EXPECT_EQ(compiler->Output().length(), 0);
}

TEST_F(AMDGPUCompilerTest, CompileToLLVMBitcode_File_To_File)
{
  File* f = TestDirInputFile(DT_CL, simpleCl);
  ASSERT_NE(f, nullptr);
  File* out = TmpOutputFile(DT_LLVM_BC, outBc);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(f);
  ASSERT_TRUE(compiler->CompileToLLVMBitcode(inputs, out, emptyOptions));
  ASSERT_TRUE(FileExists(out));
}

TEST_F(AMDGPUCompilerTest, CompileAndLinkExecutable_File_To_File)
{
  File* f = TestDirInputFile(DT_CL, simpleCl);
  ASSERT_NE(f, nullptr);
  File* out = TmpOutputFile(DT_EXECUTABLE, outCo);
  ASSERT_NE(out, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(f);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, emptyOptions));
  ASSERT_TRUE(FileExists(out));
}

TEST_F(AMDGPUCompilerTest, CompileAndLink_CLs_File_To_File)
{
  File* ef1 = TestDirInputFile(DT_CL, externFunction1Cl);
  ASSERT_NE(ef1, nullptr);
  File* ef2 = TestDirInputFile(DT_CL, externFunction2Cl);
  ASSERT_NE(ef2, nullptr);
  std::vector<Data*> inputs;
  inputs.push_back(ef1);
  inputs.push_back(ef2);
  File* out = TmpOutputFile(DT_EXECUTABLE, outCo);
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(compiler->CompileAndLinkExecutable(inputs, out, emptyOptions));
  ASSERT_TRUE(FileExists(out));
}


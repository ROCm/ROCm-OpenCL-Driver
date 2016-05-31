#include "gtest/gtest.h"

#include "AmdCompiler.h"

using namespace amd;

class AMDGPUCompilerTest : public ::testing::Test {
private:
  std::string llvmBin;

protected:
  virtual void SetUp() {
    llvmBin = getenv("LLVM_BIN");
    compiler = driver.CreateAMDGPUCompiler(llvmBin);
  }

  virtual void TearDown() {
    delete compiler;
  }

  CompilerDriver driver;
  Compiler* compiler;
};

TEST_F(AMDGPUCompilerTest, OutputEmpty)
{
  EXPECT_EQ(compiler->Output().length(), 0);
}

#ifndef AMD_COMPILER_DRIVER_H
#define AMD_COMPILER_DRIVER_H

#include <string>
#include <vector>

namespace amd {

enum DataType {
  DT_CL,
  DT_LLVM_BC,
  DT_LLVM_LL,
  DT_EXECUTABLE,
};

class Data {
private:
  DataType type;

public:
  Data(DataType type_)
    : type(type_) {}
  virtual ~Data() {}
  DataType Type() const { return type; }
  virtual bool IsReadOnly() const = 0;
};

class File : public Data {
private:
  std::string name;
  bool readonly;

public:
  File(DataType type, const std::string& name_, bool readonly_ = false)
    : Data(type),
      name(name_), readonly(readonly_) {}

  bool IsReadOnly() const override { return readonly; }
  const std::string& Name() const { return name; }
};

class BufferReference : public Data {
private:
  const char* ptr;
  size_t size;

public:
  BufferReference(DataType type, const char* ptr_, size_t size_)
    : Data(type),
      ptr(ptr_), size(size_) {}

  bool IsReadOnly() const override { return true; }
  const char* Ptr() const { return ptr; }
  size_t Size() const { return size; }
};

class Buffer : public Data {
private:
  std::vector<char> buf;

public:
  Buffer(DataType type)
    : Data(type) {}

  bool IsReadOnly() const override { return false; }
  std::vector<char>& Buf() { return buf; }
  const std::vector<char>& Buf() const { return buf; }
};

class LLVMModule : public Data {
};

class Compiler {
public:
  virtual ~Compiler() {}

  virtual std::string Output() = 0;

  virtual File* NewInputFile(DataType type, const std::string& path) = 0;

  virtual File* NewOutputFile(DataType type, const std::string& path) = 0;

  virtual File* NewTempFile(DataType type, const char* ext) = 0;

  virtual Data* NewBufferReference(DataType type, const char* ptr, size_t size) = 0;

  virtual Data* NewBuffer(DataType type) = 0;

  virtual bool CompileToLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) = 0;

  virtual bool LinkLLVMBitcode(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) = 0;

  virtual bool CompileAndLinkExecutable(const std::vector<Data*>& inputs, Data* output, const std::vector<std::string>& options) = 0;
};

class CompilerDriver {
public:
  Compiler* CreateAMDGPUCompiler(const std::string& llvmBin);
};

}

#endif

#ifndef AMD_COMPILER_DRIVER_H
#define AMD_COMPILER_DRIVER_H

#include <string>
#include <vector>
#include <cassert>

namespace amd {

enum DataType {
  DT_CL,
  DT_LLVM_BC,
  DT_LLVM_LL,
  DT_EXECUTABLE,
  DT_DIR,
};

class File;

class Compiler;

class Data {
private:
  DataType type;

public:
  Data(DataType type_)
    : type(type_) {}
  virtual ~Data() {}
  DataType Type() const { return type; }
  virtual bool IsReadOnly() const = 0;
  virtual File* ToInputFile(Compiler* comp) = 0;
  virtual File* ToOutputFile(Compiler* comp) = 0;
  virtual bool ReadOutputFile(File* f) = 0;
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

  File* ToInputFile(Compiler* comp) override { return this; }
  File* ToOutputFile(Compiler* comp) override { assert(!readonly); return this; }
  bool ReadOutputFile(File* f) override { assert(this == f); return true; }

  bool WriteData(const char* ptr, size_t size);
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
  File* ToInputFile(Compiler* comp) override;
  File* ToOutputFile(Compiler* comp) override;
  bool ReadOutputFile(File* f) override { assert(false); return false; }
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
  size_t Size() const { return buf.size(); }
  bool IsEmpty() const { return buf.size() == 0; }
  File* ToInputFile(Compiler* comp) override;
  File* ToOutputFile(Compiler* comp) override;
  bool ReadOutputFile(File* f) override;
};

class LLVMModule : public Data {
};

class Compiler {
public:
  virtual ~Compiler() {}

  virtual std::string Output() = 0;

  virtual File* NewInputFile(DataType type, const std::string& path) = 0;

  virtual File* NewOutputFile(DataType type, const std::string& path) = 0;

  virtual File* NewTempFile(DataType type, File* parent = 0) = 0;

  virtual File* NewTempDir(File* parent = 0) = 0;

  virtual BufferReference* NewBufferReference(DataType type, const char* ptr, size_t size) = 0;

  virtual Buffer* NewBuffer(DataType type) = 0;

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

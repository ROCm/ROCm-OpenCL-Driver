#ifndef AMD_COMPILER_DRIVER_H
#define AMD_COMPILER_DRIVER_H

#include <string>
#include <vector>
#include <cassert>

namespace amd {

enum DataType {
  DT_CL,
  DT_CL_HEADER,
  DT_LLVM_BC,
  DT_LLVM_LL,
  DT_EXECUTABLE,
  DT_DIR,
};

class FileReference;
class File;

class Compiler;

class Data {
private:
  std::string id;
  DataType type;

public:
  Data(DataType type_, const std::string& id_ = "")
    : type(type_), id(id_) {}
  virtual ~Data() {}
  DataType Type() const { return type; }
  const std::string& Id() const { return id; }
  virtual bool IsReadOnly() const = 0;
  virtual FileReference* ToInputFile(Compiler* comp, File *parent) = 0;
  virtual File* ToOutputFile(Compiler* comp, File *parent) = 0;
  virtual bool ReadOutputFile(File* f) = 0;
};

bool FileExists(const std::string& name);

class FileReference : public Data {
private:
  std::string name;

public:
  FileReference(DataType type, const std::string& name_)
    : Data(type),
      name(name_) {}

  bool IsReadOnly() const override { return true; }
  const std::string& Name() const { return name; }
  FileReference* ToInputFile(Compiler* comp, File *parent) override { return this; }
  File* ToOutputFile(Compiler* comp, File *parent) override { assert(false); return 0; }
  bool ReadOutputFile(File* f) override { assert(false); return false; }
  bool Exists() const;
};

class File : public FileReference {
private:
  bool readonly;

public:
  File(DataType type, const std::string& name)
    : FileReference(type, name) {}

  bool IsReadOnly() const override { return false; }
  File* ToOutputFile(Compiler* comp, File *parent) override { return this; }
  bool ReadOutputFile(File* f) override { assert(this == f); return true; }
  bool WriteData(const char* ptr, size_t size);
  bool Exists() const;
};

class BufferReference : public Data {
private:
  const char* ptr;
  size_t size;

public:
  BufferReference(DataType type, const char* ptr_, size_t size_, const std::string& id)
    : Data(type, id),
      ptr(ptr_), size(size_) {}

  bool IsReadOnly() const override { return true; }
  const char* Ptr() const { return ptr; }
  size_t Size() const { return size; }
  FileReference* ToInputFile(Compiler* comp, File *parent) override;
  File* ToOutputFile(Compiler* comp, File *parent) override;
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
  FileReference* ToInputFile(Compiler* comp, File *parent) override;
  File* ToOutputFile(Compiler* comp, File *parent) override;
  bool ReadOutputFile(File* f) override;
};

class LLVMModule : public Data {
};

class Compiler {
public:
  virtual ~Compiler() {}

  virtual std::string Output() = 0;

  virtual FileReference* NewFileReference(DataType type, const std::string& name, File* parent = 0) = 0;

  virtual File* NewFile(DataType type, const std::string& name, File* parent = 0) = 0;

  virtual File* NewTempFile(DataType type, const std::string& name = "", File* parent = 0) = 0;

  virtual File* NewTempDir(File* parent = 0) = 0;

  virtual BufferReference* NewBufferReference(DataType type, const char* ptr, size_t size, const std::string& id = "") = 0;

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

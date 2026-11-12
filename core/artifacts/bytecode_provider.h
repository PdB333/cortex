#pragma once

// Bytecode containers of interpreted and managed runtimes, read offline:
// Python .pyc (marshal code objects, CPython 3.13 disassembly), Java .class
// (constant pool, members, JVM instructions), Lua 5.1-5.4 and LuaJIT chunks
// (function prototypes, constants, debug names) and WebAssembly modules
// (sections, types, imports, exports, the name section). Nothing is loaded,
// unmarshalled into live objects, verified by a VM or executed.

#include "artifact.h"

namespace cortex::artifacts {

class PycProvider final : public Provider {
public:
    const char* Name() const override { return "python-bytecode"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"pyc"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

class JavaClassProvider final : public Provider {
public:
    const char* Name() const override { return "java-class"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"java-class"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

class LuaBytecodeProvider final : public Provider {
public:
    const char* Name() const override { return "lua-bytecode"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"lua-bytecode", "luajit-bytecode"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

class WasmProvider final : public Provider {
public:
    const char* Name() const override { return "webassembly"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"wasm"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

// SPIR-V shader and kernel modules (Vulkan, OpenGL, OpenCL): header,
// capabilities, extensions, imported instruction sets, memory model, entry
// points with their execution modes, and debug names.
class SpirvProvider final : public Provider {
public:
    const char* Name() const override { return "spirv"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"spirv"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

} // namespace cortex::artifacts

#pragma once

#include "target/module_provider.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cortex::application {

// Cheat Engine style address expressions:
//   7FF6A1B20010            hexadecimal (0x prefix optional)
//   game.exe+1A2B0          module or symbol plus hex offset
//   "Tutorial-i386.exe"+10  quoted names may contain any character
//   [[game.exe+10]+20]+8    [x] reads the pointer stored at x
//   base+10*4-8             + - * and parentheses
// A token made only of hex digits is a number; anything else is a name.
struct AddressResolver {
    // Module base or symbol address by name; false when unknown.
    std::function<bool(const std::string& name, uint64_t& value)> symbol;
    // Reads the pointer stored at an address; false when unreadable.
    std::function<bool(uint64_t address, uint64_t& value)> readPointer;
};

bool EvaluateAddress(const std::string& text, const AddressResolver& resolver, uint64_t& result,
                     std::string* error = nullptr);

// Symbols registered by scripts (registerSymbol) and cheat tables
// (UserdefinedSymbols). Shared by the address list and the Lua engine,
// which runs on its own thread.
class UserSymbols {
public:
    void Set(const std::string& name, uint64_t address);
    bool Remove(const std::string& name);
    bool Find(const std::string& name, uint64_t& address) const;
    std::vector<std::pair<std::string, uint64_t>> List() const;
    void Clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::pair<std::string, uint64_t>> symbols_;  // lowercase name -> name, address
};

// Names an address expression can use, like Cheat Engine's symbol handler:
//   player                    a user-defined symbol
//   game.exe, game            module base (the extension may be left out)
//   game.exe.Export, game!Export, kernel32.Sleep
//   Export                    an exported name, searched in every module
// Export tables are read on first use and kept until the modules change.
class TargetSymbols {
public:
    using Export = std::pair<std::string, uint64_t>;  // name, absolute address
    struct ExportEntry {
        std::string name;
        uint64_t address = 0;
        std::string forwarder;  // "NTDLL.RtlAllocateHeap" for a forwarded export
    };
    using ExportReader = std::function<std::vector<ExportEntry>(const target::ModuleInfo& module)>;

    void SetModules(std::vector<target::ModuleInfo> modules);
    void SetExportReader(ExportReader reader) { reader_ = std::move(reader); }
    void SetUserSymbols(std::shared_ptr<UserSymbols> symbols) { user_ = std::move(symbols); }
    const std::vector<target::ModuleInfo>& Modules() const { return modules_; }
    bool Resolve(const std::string& name, uint64_t& value);
    // Exports whose name contains `filter` (any case), as module.Export.
    std::vector<Export> Search(const std::string& filter, size_t limit);

private:
    const target::ModuleInfo* FindModule(const std::string& name) const;
    const std::map<std::string, ExportEntry>& ExportsOf(const target::ModuleInfo& module);
    bool Lookup(const target::ModuleInfo& module, const std::string& name, uint64_t& value, int depth);

    std::vector<target::ModuleInfo> modules_;
    ExportReader reader_;
    std::shared_ptr<UserSymbols> user_;
    std::map<uint64_t, std::map<std::string, ExportEntry>> exports_;  // by module base, keyed by lowercase name
};

} // namespace cortex::application

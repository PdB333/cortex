#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cortex::services {

// Cheat Engine's Auto Assembler script language, on top of the text
// assembler. A script has an [ENABLE] and a [DISABLE] section; each mixes
// directives with blocks of assembly placed at an address:
//
//   [ENABLE]
//   aobscanmodule(INJECT,game.exe,89 83 00 01 00 00)
//   alloc(newmem,$1000,INJECT)
//   label(code)
//   label(return)
//   newmem:
//   code:
//     mov [rbx+00000100],eax
//     jmp return
//   INJECT:
//     jmp newmem
//     nop 2
//   return:
//   registersymbol(INJECT)
//
//   [DISABLE]
//   INJECT:
//     db 89 83 00 01 00 00
//   unregistersymbol(INJECT)
//   dealloc(newmem)
//
// Supported directives: alloc, globalalloc, dealloc, label, define,
// registersymbol, unregistersymbol, aobscan, aobscanmodule, aobscanregion,
// assert, fullaccess, createthread and createthreadandwait.
// Everything the host must do (read, write, allocate, scan, resolve a name)
// arrives through AutoAssembleHost, so the interpreter stays testable and
// free of platform code.

struct AutoAssembleHost {
    std::function<bool(uint64_t address, void* buffer, size_t size)> read;
    std::function<bool(uint64_t address, const void* buffer, size_t size)> write;
    // Allocates `size` bytes, within reach of `nearAddress` when it is not 0
    // so a 5-byte jmp can reach it.
    std::function<bool(size_t size, uint64_t nearAddress, uint64_t& address, std::string& error)> allocate;
    std::function<bool(uint64_t address, std::string& error)> release;
    std::function<bool(uint64_t address, size_t size, std::string& error)> fullAccess;
    std::function<bool(uint64_t address, std::string& error)> createThread;
    // A module name, an export, or a symbol registered by an earlier run.
    std::function<bool(const std::string& name, uint64_t& value)> symbol;
    // Base and size of a loaded module; false when it is not loaded.
    std::function<bool(const std::string& module, uint64_t& base, uint64_t& size)> moduleRange;
    // Every match of an array-of-bytes pattern (with ?? wildcards) in
    // [start, stop).
    std::function<bool(const std::string& pattern, uint64_t start, uint64_t stop,
                       std::vector<uint64_t>& matches, std::string& error)> scan;
};

struct AutoAssembleOptions {
    bool enable = true;   // run [ENABLE]; false runs [DISABLE]
    bool x64 = true;
    // Parse, allocate nothing and write nothing: used to check a script.
    bool dryRun = false;
};

struct AutoAssemblePatch {
    uint64_t address = 0;
    std::vector<uint8_t> original;
    std::vector<uint8_t> written;
};

struct AutoAssembleResult {
    std::vector<std::string> log;
    std::vector<AutoAssemblePatch> patches;
    // Allocations this run made, so the caller can free them later.
    std::vector<std::pair<std::string, uint64_t>> allocations;
    std::vector<std::pair<std::string, uint64_t>> registered;    // registersymbol
    std::vector<std::string> unregistered;                       // unregistersymbol
    std::vector<std::string> freed;                              // dealloc, by name
};

bool RunAutoAssembler(const std::string& script, const AutoAssembleHost& host,
                      const AutoAssembleOptions& options, AutoAssembleResult& result,
                      std::string* error = nullptr);

// The [ENABLE] and [DISABLE] halves of a script. Text before the first
// section header belongs to both, as Cheat Engine treats it.
bool SplitAutoAssemblerSections(const std::string& script, std::string& enable, std::string& disable);

// True when the text looks like an Auto Assembler script rather than a
// plain run of instructions (it has a section header or a directive).
bool LooksLikeAutoAssembler(const std::string& text);

} // namespace cortex::services

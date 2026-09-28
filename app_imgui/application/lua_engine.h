#pragma once

#include "address_expression.h"
#include "target/module_provider.h"
#include "target/session.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cortex::application {

// Lua 5.4 running inside Cortex with a Cheat Engine compatible API
// (readInteger, writeFloat, getAddress, AOBScan, enumModules, pause...). It
// reads and writes the target from outside, so it needs no injected
// runtime; writes and pausing still require Writes allowed.
struct LuaRunOptions {
    target::SessionPtr session;
    bool allowWrites = false;
    int timeoutMs = 60000;                                // 0 = no limit
    const std::atomic_bool* cancelled = nullptr;
    std::function<void(const std::string& line)> output;  // print()
    std::function<std::vector<target::ModuleInfo>()> modules;
    TargetSymbols::ExportReader exports;       // module.Export names, optional
    std::shared_ptr<UserSymbols> userSymbols;  // registerSymbol, optional
};

struct LuaRunResult {
    bool ok = false;
    std::string error;
    std::string returned;  // values returned by the chunk, tab separated
    double milliseconds = 0.0;
};

LuaRunResult RunDesktopLua(const std::string& source, const LuaRunOptions& options);

// Names of the Cheat Engine style functions the engine provides.
const std::vector<const char*>& DesktopLuaFunctions();

} // namespace cortex::application

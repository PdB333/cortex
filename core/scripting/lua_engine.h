#pragma once
#include <string>

// Embedded Lua 5.4 runtime.
//
// A fresh lua_State is created per Exec() call so scripts cannot leak globals
// between requests. The `cortex.*` table exposes just enough to be productive
// without duplicating every REST route: memory read/write, address resolve,
// module base, log, sleep. `print(...)` is redirected into the returned
// output buffer.
//
// A count-based debug hook uses a thread-local deadline to enforce a wall
// timeout; scripts that hit it abort with "script_timeout".

namespace scripting {

struct ExecResult {
    bool ok;
    std::string result;   // stringified return value (empty if nil / no return)
    std::string output;   // captured print() output
    std::string error;    // Lua error message when ok=false
};

ExecResult Exec(const std::string& code, int timeoutMs = 5000);

// Where persisted scripts live: <module_dir>/cortex_scripts/
std::string GetScriptsDir();

} // namespace scripting

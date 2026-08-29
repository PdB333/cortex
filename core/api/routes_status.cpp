#include "routes.h"
#include "server.h"
#include "mcp_contract.h"
#include "../overlay/overlay.h"
#ifdef CORTEX_KIERO
#include "../hook/kiero_hook.h"
#endif
#ifdef CORTEX_D3D8
#include "../hook/d3d8_hook.h"
#endif

#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <set>

using json = nlohmann::json;

namespace api {

namespace {
    std::string ProcessName() {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string full(path);
        size_t pos = full.find_last_of("\\/");
        return pos == std::string::npos ? full : full.substr(pos + 1);
    }
}

json BuildToolsManifest() {
        json j = json::array();

        j.push_back({{"name", "status"}, {"method", "GET"}, {"path", "/status"},
                      {"public", true}, {"description", "Bridge state: pid, process, uptime, port."}});
        j.push_back({{"name", "health"}, {"method", "GET"}, {"path", "/health"}, {"public", true},
                      {"description", "Runtime health: server, authentication, bitness, and render hook state."}});
        j.push_back({{"name", "tools"}, {"method", "GET"}, {"path", "/tools"}, {"public", true},
                      {"description", "Self-documenting manifest of every Cortex operation."}});
        j.push_back({{"name", "openapi"}, {"method", "GET"}, {"path", "/openapi.json"}, {"public", true},
                      {"description", "OpenAPI 3 description generated from the same manifest as /tools."}});
        j.push_back({{"name", "schema_validate"}, {"method", "GET"}, {"path", "/schema/validate"}, {"public", true},
                      {"description", "Validates manifest, MCP/OpenAPI schemas, and registered native handlers against one another."}});

        j.push_back({{"name", "mcp"}, {"method", "POST"}, {"path", "/mcp"},
                      {"description", "Native Model Context Protocol endpoint. Speaks JSON-RPC 2.0 and "
                                      "supports 'initialize', 'tools/list', 'tools/call', 'ping'. Every "
                                      "route in this manifest is auto-exposed as an MCP tool with an "
                                      "inputSchema derived from its body/query fields; 'tools/call' "
                                      "loopback-dispatches to the same route so behavior stays in one "
                                      "place. Use `_query` in arguments for query-string params and "
                                      "`_path` for {id} substitutions."}});

        j.push_back({{"name", "session_export"}, {"method", "POST"}, {"path", "/session/export"},
                      {"description", "Exports a persistent RE run snapshot: modules, breakpoint logs/execution order, tracked objects/vtables, network events, allocations, traces, project/RE knowledge and screenshot. Returns a stable session id and path."}});
        j.push_back({{"name", "session_list"}, {"method", "GET"}, {"path", "/session/list"},
                      {"description", "Lists persisted RE run snapshots available for cross-run comparison."}});
        j.push_back({{"name", "session_diff"}, {"method", "POST"}, {"path", "/session/diff"},
                      {"description", "Compares two persisted runs: executed functions, callback order differences, tracked objects/vtables, network-event count and allocations."},
                      {"body", {{"a", {{"type","string"},{"required",true},{"description","First session id from session_list/export."}}},
                                {"b", {{"type","string"},{"required",true},{"description","Second session id."}}}}}});
        j.push_back({{"name", "lua_exec"}, {"method", "POST"}, {"path", "/lua/exec"},
                      {"description", "Executes a Lua 5.4 snippet in a fresh sandbox. The `cortex.*` "
                                      "table exposes memory.read/write/read_bytes, module_base, "
                                      "resolve, describe, log, sleep. `print(...)` is captured into "
                                      "the returned output. Returns {ok, result, output, error}."},
                      {"body", {{"code", "required: Lua source"},
                                {"timeout_ms", "optional: wall-time cap (default 5000)"}}}});

        j.push_back({{"name", "lua_scripts"}, {"method", "GET"}, {"path", "/lua/scripts"},
                      {"description", "Lists the persisted Lua scripts under cortex_scripts/."}});

        j.push_back({{"name", "lua_scripts_save"}, {"method", "POST"}, {"path", "/lua/scripts"},
                      {"description", "Saves or overwrites a named Lua script."},
                      {"body", {{"name", "required: identifier (a-zA-Z0-9_-)"},
                                {"code", "required: Lua source"}}}});

        j.push_back({{"name", "lua_scripts_get"}, {"method", "GET"}, {"path", "/lua/scripts/{name}"},
                      {"description", "Returns the source of a saved script."}});

        j.push_back({{"name", "lua_scripts_run"}, {"method", "POST"}, {"path", "/lua/scripts/{name}/run"},
                      {"description", "Runs a saved script; returns {ok, result, output, error}."},
                      {"body", {{"timeout_ms", "optional: wall-time cap (default 5000)"}}}});

        j.push_back({{"name", "lua_scripts_delete"}, {"method", "DELETE"}, {"path", "/lua/scripts/{name}"},
                      {"description", "Deletes a saved script."}});

        j.push_back({{"name", "ocr"}, {"method", "POST"}, {"path", "/ocr"},
                      {"description", "Runs OCR on a PNG (Windows.Media.Ocr backend, Win10+). "
                                      "Returns recognized text plus per-word bounding boxes. "
                                      "Feed screenshots straight from /screenshot?mode=auto by "
                                      "base64-encoding the response."},
                      {"body", {{"image_base64", "PNG bytes (base64-encoded); OR"},
                                {"image_path", "absolute path to a PNG on disk"},
                                {"language", "optional: BCP-47 tag (e.g. 'en-US', 'fr-FR'); "
                                             "empty uses the user's installed OCR languages"}}}});

        j.push_back({{"name", "modules"}, {"method", "GET"}, {"path", "/modules"},
                      {"description", "List of modules (DLL/EXE) loaded in the process, with base and size."}});

        j.push_back({{"name", "memory_read"}, {"method", "POST"}, {"path", "/memory/read"},
                      {"description", "Reads a value from memory."},
                      {"body", {{"address", "number (hex or decimal)"},
                                {"type", "i8|i16|i32|i64|u8|u16|u32|u64|float|double|bytes|string"},
                                {"count", "optional: number of bytes for bytes/string"}}}});

        j.push_back({{"name", "memory_read_batch"}, {"method", "POST"}, {"path", "/memory/read_batch"},
                      {"description", "Reads several addresses in a single call."},
                      {"body", {{"reads", "array of {address, type, count?}"}}}});

        j.push_back({{"name", "memory_write"}, {"method", "POST"}, {"path", "/memory/write"},
                      {"description", "Writes a value to memory."},
                      {"body", {{"address", "number"}, {"type", "i8|i16|i32|i64|u8|u16|u32|u64|float|double|bytes"},
                                {"value", "number, string, or hex depending on type"}}}});

        j.push_back({{"name", "memory_write_batch"}, {"method", "POST"}, {"path", "/memory/write_batch"},
                      {"description", "Writes several addresses in a single call, symmetric to /memory/read_batch."},
                      {"body", {{"writes", "array of {address, type, value}"}}}});

        j.push_back({{"name", "memory_dump_typed"}, {"method", "GET"}, {"path", "/memory/dump_typed"},
                      {"description", "Reads raw bytes at an address and interprets them simultaneously as every applicable numeric type (i8/u8 from 1 byte, i16/u16 from 2, i32/u32/float from 4, i64/u64/double from 8), plus hex and ascii -- to guess the type of an unknown value without doing N separate /memory/read calls."},
                      {"query", {{"address", "required"}, {"size", "optional, default 32, max 4096"}}}});

        j.push_back({{"name", "memory_fill"}, {"method", "POST"}, {"path", "/memory/fill"},
                      {"description", "Fills a byte range by repeating a hex pattern (like /patch/nop but with an arbitrary pattern, not tracked/revertible -- use /patch/write if revert is needed)."},
                      {"body", {{"address", "required"}, {"pattern", "required: hex string, repeated to fill the length"},
                                {"size", "one of the two required: number of bytes"}, {"end", "one of the two required: end address (exclusive), alternative to size"}}}});

        j.push_back({{"name", "memory_regions"}, {"method", "GET"}, {"path", "/memory/regions"},
                      {"description", "Lists every memory region of the process via VirtualQuery (base, size, rwx protection, commit/reserve/free state, image/mapped/private type, associated module if known) -- an overview of the address space, complementary to targeted scans."}});

        j.push_back({{"name", "scan_new"}, {"method", "POST"}, {"path", "/scan/new"},
                      {"description", "Starts a memory scan (first pass). Without 'value', records a baseline (\"unknown initial value\") to be refined afterwards with changed/unchanged/increased/decreased. A scan without 'value' or a start/end range covers at most 3,000,000 addresses and then stops (truncated=true) -- if the address you're looking for might be beyond that, restrict with start/end (or with module via /modules) rather than scanning all of memory. type=\"all\" scans all 10 numeric types simultaneously at each address (requires 'value', otherwise returns error all_requires_value -- without a known value the number of candidates would explode x10 for little benefit)."},
                      {"body", {{"type", "i8|i16|i32|i64|u8|u16|u32|u64|float|double|all"}, {"value", "optional (required if type=all): exact value to search for"},
                                {"start", "optional: start address of the range to scan"}, {"end", "optional: end address (exclusive)"},
                                {"writable_only", "optional bool, default true"}, {"executable_only", "optional bool, default false"},
                                {"copy_on_write", "optional bool, default false"}, {"alignment", "optional: scan stride in bytes (default = type size, or 1 for type=all)"},
                                {"pause_process", "optional bool, default false: suspends all other threads of the process during the scan to avoid false positives from concurrent writes"}}}});

        j.push_back({{"name", "scan_next"}, {"method", "POST"}, {"path", "/scan/next"},
                      {"description", "Refines an existing scan by comparing each candidate to its previous value."},
                      {"body", {{"scan_id", "number"}, {"filter", "exact|changed|unchanged|increased|decreased|increased_by|decreased_by|greater_than|less_than|between"},
                                {"value", "required except for changed/unchanged -- lower bound for between"}, {"value2", "required if filter=between: upper bound"},
                                {"pause_process", "optional bool, default false"}}}});

        j.push_back({{"name", "scan_results"}, {"method", "GET"}, {"path", "/scan/results/{scan_id}"},
                      {"description", "Paginated list of a scan's remaining candidates, with current value."},
                      {"query", {{"offset", "optional, default 0"}, {"limit", "optional, default 100, max 1000"}}}});

        j.push_back({{"name", "scan_list"}, {"method", "GET"}, {"path", "/scan/list"},
                      {"description", "Lists active scans (id, type, candidate count)."}});

        j.push_back({{"name", "scan_delete"}, {"method", "DELETE"}, {"path", "/scan/{scan_id}"},
                      {"description", "Deletes a scan and frees its memory."}});

        j.push_back({{"name", "scan_aob"}, {"method", "POST"}, {"path", "/scan/aob"},
                      {"description", "Searches for a byte pattern (AOB), wildcard = two question marks. Restricted to a module if provided, otherwise searches all loaded modules."},
                      {"body", {{"pattern", "string, e.g.: \"48 8B ?? 05 90\""}, {"module", "optional: module name (e.g. HitmanContracts.exe)"}}}});

        j.push_back({{"name", "scan_pointers"}, {"method", "POST"}, {"path", "/scan/pointers"},
                      {"description", "Reverse pointer scan (Cheat Engine's \"find what points to this address\"): finds addresses in memory whose value (interpreted as a pointer) targets 'target' or just before it (up to max_offset bytes before, to catch a pointer to the base of a struct where 'target' is a field). Complementary to /project/pointer_path, which resolves an already-known chain of pointers -- this one is used to discover a new one."},
                      {"body", {{"target", "required: targeted address"}, {"max_offset", "optional, default 0 (exact match only)"}}}});

        j.push_back({{"name", "scan_pointer_path"}, {"method", "POST"}, {"path", "/scan/pointer_path"},
                      {"description", "Multi-level pointer scan (Cheat Engine's \"Pointer scan for this address\"): searches for base_module+offset -> deref -> offset -> ... -> target chains rooted in a loaded module (and therefore stable across game restarts), unlike /scan/pointers, which only gives a single level (often a heap address that moves). Each result can be used as-is in /project/pointer_path (module, base_offset, offsets)."},
                      {"body", {{"target", "required: targeted address"}, {"max_depth", "optional, default 5, max 7: levels of indirection"},
                                {"max_offset", "optional, default 256: max offset before the value at each level"}}}});

        j.push_back({{"name", "scan_strings"}, {"method", "POST"}, {"path", "/scan/strings"},
                      {"description", "Searches for printable strings in memory (ASCII or UTF-16LE), useful for finding object names/dialogue/file paths rather than numeric values. Covers all readable memory (not just writable) by default, or just a module if specified."},
                      {"body", {{"min_length", "optional, default 4"}, {"contains", "optional: substring filter, case-insensitive"},
                                {"module", "optional: restricts to this module"}, {"encoding", "ascii|utf16, default ascii"}}}});

        j.push_back({{"name", "scan_intersect"}, {"method", "POST"}, {"path", "/scan/intersect"},
                      {"description", "Intersects the remaining candidates of several active scans (addresses common to all) -- useful for combining two independent scan sessions (e.g. an i32 scan and a float scan over the same range) without starting over."},
                      {"body", {{"scan_ids", "required: array of at least 2 scan_id"}}}});

        j.push_back({{"name", "scan_code_caves"}, {"method", "POST"}, {"path", "/scan/code_caves"},
                      {"description", "Searches for padding regions (contiguous 0x00 or 0xCC) in executable memory, usable as a code cave for a detour/trampoline without going through /patch/alloc_cave (e.g. end-of-function/section padding already present in the binary)."},
                      {"body", {{"min_size", "optional, default 16: minimum size in bytes"}, {"module", "optional: restricts to this module, otherwise all executable memory"}}}});

        j.push_back({{"name", "disasm"}, {"method", "GET"}, {"path", "/disasm"},
                      {"description", "Disassembles x86-32 code starting at an address (Intel syntax)."},
                      {"query", {{"address", "required: start address"}, {"count", "optional, default 20, max 500: number of instructions"}}}});

        j.push_back({{"name", "debug_breakpoint_add"}, {"method", "POST"}, {"path", "/debug/breakpoint"},
                      {"description", "Sets a software or DR0-DR3 hardware breakpoint. Hardware breakpoints are process_global by default and automatically follow newly created threads. List output reports applied_threads/total_threads. Conditions accept a structured object or short `if` expression such as `[ecx+0xB9] == 0`."},
                      {"body", {
                          {"address", {{"oneOf",json::array({{{"type","integer"}},{{"type","string"}},{{"type","object"}}})},{"required",true},{"description","Breakpoint address."}}},
                          {"kind", {{"type","string"},{"enum",json::array({"software","hw_execute","hw_write","hw_readwrite"})},{"description","Breakpoint kind."}}},
                          {"size", {{"type","integer"},{"enum",json::array({1,2,4})},{"description","Hardware watch size; ignored for execute."}}},
                          {"action", {{"type","string"},{"enum",json::array({"pause","log"})},{"description","pause or log."}}},
                          {"process_global", {{"type","boolean"},{"description","Hardware only. Default true; continuously applies to new threads."}}},
                          {"thread_id", {{"type","integer"},{"minimum",1},{"description","Required for hardware breakpoints when process_global=false."}}},
                          {"if", {{"type","string"},{"description","Short condition expression, e.g. `[ecx+0xB9] == 0` or `mem32(rcx+0x20) != 3`."}}},
                          {"condition", {{"oneOf",json::array({{{"type","string"}},{{"type","object"}}})},{"description","Condition expression string or legacy structured condition object."}}},
                          {"auto_capture", {{"type","boolean"},{"description","Automatically capture this-object, vtable and stack arguments for logged hits."}}},
                          {"capture", {{"type","array"},{"items",{{"type","object"}}},{"description","Additional captures: {name,expression,size,type}."}}}
                      }}}});
        j.push_back({{"name", "debug_breakpoint_delete"}, {"method", "DELETE"}, {"path", "/debug/breakpoint/{id}"},
                      {"description", "Removes a breakpoint and restores the original state."}});

        j.push_back({{"name", "debug_breakpoint_list"}, {"method", "GET"}, {"path", "/debug/breakpoint/list"},
                      {"description", "Lists active breakpoints with their hit count."}});

        j.push_back({{"name", "debug_paused"}, {"method", "GET"}, {"path", "/debug/paused"},
                      {"description", "Lists threads currently frozen on a 'pause' breakpoint, with their registers."}});

        j.push_back({{"name", "debug_threads"}, {"method", "GET"}, {"path", "/debug/threads"},
                      {"description", "Lists the thread IDs of the target process."}});

        j.push_back({{"name", "debug_registers"}, {"method", "GET"}, {"path", "/debug/registers"},
                      {"description", "Reads a thread's registers (frozen or not -- if not frozen, briefly suspends the thread to read its context)."},
                      {"query", {{"thread_id", "required"}}}});

        j.push_back({{"name", "debug_stack"}, {"method", "GET"}, {"path", "/debug/stack"},
                      {"description", "Walks the call stack via EBP chaining (limitation: does not work on code compiled without a frame pointer / FPO)."},
                      {"query", {{"thread_id", "required"}, {"count", "optional, default 32, max 256"}}}});

        j.push_back({{"name", "debug_breakpoint_log"}, {"method", "GET"}, {"path", "/debug/breakpoint/{id}/log"},
                      {"description", "History of hits for a breakpoint set with action=log (registers captured at each hit, up to the 500 most recent entries -- older ones are overwritten). For a 'pause' breakpoint, check /debug/paused instead."}});

        j.push_back({{"name", "debug_continue"}, {"method", "POST"}, {"path", "/debug/continue"},
                      {"description", "Releases a thread frozen on a breakpoint."},
                      {"body", {{"thread_id", "required"}}}});

        j.push_back({{"name", "debug_step"}, {"method", "POST"}, {"path", "/debug/step"},
                      {"description", "Executes exactly one instruction on a frozen thread then re-freezes it; blocks the response until the step completes or times out."},
                      {"body", {{"thread_id", "required"}, {"timeout_ms", "optional, default 2000"}}}});

        j.push_back({{"name", "symbols_resolve"}, {"method", "GET"}, {"path", "/symbols/resolve"}, {"ok_false_is_error", false},
                      {"description", "Resolves an address to a symbol (name + offset) and file/line if a PDB is present. Most games (especially older ones without a public PDB) won't have any symbols -- returns ok=false in that case, which is not an error, just an absence of info."},
                      {"query", {{"address", "required"}}}});

        j.push_back({{"name", "symbols_lookup"}, {"method", "GET"}, {"path", "/symbols/lookup"}, {"ok_false_is_error", false},
                      {"description", "Reverse lookup: address of a named symbol (export or PDB)."},
                      {"query", {{"name", "required"}}}});

        j.push_back({{"name", "project_get"}, {"method", "GET"}, {"path", "/project"},
                      {"description", "Returns the entire persistent project for this game (named addresses, pointer paths, notes). JSON file stored beside the DLL, survives across sessions -- this is the AI's long-term memory for this game."}});

        j.push_back({{"name", "project_address_set"}, {"method", "POST"}, {"path", "/project/address"},
                      {"description", "Saves/overwrites a named address."},
                      {"body", {{"name", "required"}, {"address", "required"}, {"type", "optional"}, {"notes", "optional"}}}});

        j.push_back({{"name", "project_address_delete"}, {"method", "DELETE"}, {"path", "/project/address/{name}"},
                      {"description", "Deletes a named address."}});

        j.push_back({{"name", "project_pointer_path_set"}, {"method", "POST"}, {"path", "/project/pointer_path"},
                      {"description", "Saves a pointer path (module+offset then a chain of derefs+offsets), Cheat Engine style, to recover an address that moves from one session to another."},
                      {"body", {{"name", "required"}, {"module", "optional, default the main module"}, {"base_offset", "required"},
                                {"offsets", "optional, array of integers (can be negative)"}, {"final_type", "optional"}, {"notes", "optional"}}}});

        j.push_back({{"name", "project_pointer_path_delete"}, {"method", "DELETE"}, {"path", "/project/pointer_path/{name}"},
                      {"description", "Deletes a pointer path."}});

        j.push_back({{"name", "project_pointer_path_resolve"}, {"method", "GET"}, {"path", "/project/resolve/{name}"},
                      {"description", "Resolves a saved pointer path to a concrete address right now (to be read afterwards via /memory/read)."}});

        j.push_back({{"name", "project_note_add"}, {"method", "POST"}, {"path", "/project/note"},
                      {"description", "Adds a free-form note to the project (observations, hypotheses, TODO)."},
                      {"body", {{"text", "required"}, {"tags", "optional, array of strings"}}}});

        j.push_back({{"name", "project_note_delete"}, {"method", "DELETE"}, {"path", "/project/note/{id}"},
                      {"description", "Deletes a note."}});

        j.push_back({{"name", "screenshot"}, {"method", "GET"}, {"path", "/screenshot"},
                      {"description", "Captures the current game frame as PNG. Four capture modes: "
                                      "'render' hooks the game's Present (needs the game to be actively "
                                      "rendering); 'window' uses GDI PrintWindow on the game's top-level "
                                      "HWND (works when the game is in the background as long as it's "
                                      "not minimized -- best for windowed/borderless titles); 'last' "
                                      "returns the most recent PNG the render hook ever produced without "
                                      "blocking (instant, works even when the game isn't rendering, but "
                                      "may be stale); 'auto' tries render -> window -> last. Response "
                                      "carries the actual source used in header X-Cortex-Capture-Source "
                                      "(binary) or field 'source' (base64). Over RDP, D3D8 rendering "
                                      "can have 5s+ gaps between frames; raise timeout_ms if "
                                      "capture_timeout keeps coming back in render mode."},
                      {"query", {{"encoding", "binary (default, returns the image directly) | base64 (JSON)"},
                                 {"mode", "render (default) | window | last | auto"},
                                 {"timeout_ms", "wait time for a frame in render/auto mode, 100-20000, default 8000"}}}});

        j.push_back({{"name", "prompt_timed_test"}, {"method", "POST"}, {"path", "/prompt/timed_test"},
                      {"description", "Asks the player to play/test something for a given duration, "
                                      "then report a result. The answer widget stays hidden in "
                                      "the overlay until the timer runs out -- impossible to answer before "
                                      "actually testing."},
                      {"body", {{"message", "string, e.g. 'Shoot yourself and count the damage'"},
                                {"duration_seconds", "number, required, > 0"},
                                {"answer_type", "text|number, default number"}}}});

        j.push_back({{"name", "prompt_value_change"}, {"method", "POST"}, {"path", "/prompt/value_change"},
                      {"description", "Asks the player to change a given value (in-game, outside of memory) "
                                      "and confirm it. No timer -- waits indefinitely for the 'Done' click."},
                      {"body", {{"label", "string, e.g. 'Health'"}, {"target_value", "string, e.g. '50'"},
                                {"current_value", "optional, string, e.g. '100'"}}}});

        j.push_back({{"name", "prompt_status"}, {"method", "GET"}, {"path", "/prompt/{id}"},
                      {"description", "Queries the state of a previously created popup "
                                      "(status: pending|answered, response once answered)."}});

        j.push_back({{"name", "patch_write"}, {"method", "POST"}, {"path", "/patch/write"},
                      {"description", "Writes raw bytes at an address, recording the original bytes so it can be reverted -- unlike /memory/write type=bytes (which also writes but without tracking), this feeds /patch/list and DELETE /patch/{id}. To write assembly rather than raw bytes, see /patch/assemble."},
                      {"body", {{"address", "required"}, {"bytes", "required: hex string, e.g. \"90 90 90\" or \"909090\""}, {"label", "optional: free-form note"}}}});

        j.push_back({{"name", "patch_assemble"}, {"method", "POST"}, {"path", "/patch/assemble"},
                      {"description", "Mini x86 assembler: converts lines of text assembly into machine bytes (via the Zydis encoder), to write a detour/trampoline without computing the bytes by hand. Pragmatic subset of mnemonics: nop [count], int3, ret, pushad/popad/pushfd/popfd, push/pop reg, push imm32, mov/add/sub/xor/and/or/cmp/test reg,reg or reg,imm, inc/dec reg, jmp/call/je/jne/jz/jnz/jg/jl/jge/jle/ja/jb to an absolute address (jmp/jcc/call automatically resolve rel8/rel32 from 'address', no need to compute the displacement). One instruction per line; empty lines or lines starting with ';' are ignored. Returns the assembled bytes without writing anything, unless write=true."},
                      {"body", {{"address", "required: address where the first assembled byte will land (needed to resolve absolute jmp/call/jcc)"},
                                {"lines", "required: array of strings, or a single string with newlines"},
                                {"write", "optional bool, default false: if true, also writes the result via /patch/write (revertible) and returns 'id'"},
                                {"label", "optional: free-form note if write=true"}}}});

        j.push_back({{"name", "patch_nop"}, {"method", "POST"}, {"path", "/patch/nop"},
                      {"description", "Fills `size` bytes starting at `address` with NOP (0x90), tracked/revertible like /patch/write."},
                      {"body", {{"address", "required"}, {"size", "required: number of bytes"}, {"label", "optional"}}}});

        j.push_back({{"name", "patch_detour"}, {"method", "POST"}, {"path", "/patch/detour"},
                      {"description", "Writes a relative jmp (E9) from `address` to `target`, tracked/revertible. Fails if `target` is out of rel32 range (+/-2GB) -- use /patch/alloc_cave to obtain an address guaranteed to be in range on x64. Does NOT build a trampoline (does not save/relocate the overwritten instructions into the cave) -- if a clean return is needed, first disassemble the overwritten area (/disasm), then write those instructions plus a jmp back into the cave yourself via /patch/write."},
                      {"body", {{"address", "required"}, {"target", "required"}, {"jmp_size", "optional, default 5, must be >= 5: number of bytes overwritten (NOP-padded if > 5, useful to avoid cutting a longer instruction in half)"}}}});

        j.push_back({{"name", "patch_alloc_cave"}, {"method", "POST"}, {"path", "/patch/alloc_cave"},
                      {"description", "Allocates an RWX region of `size` bytes, close to `near_address` on x64 (within rel32 range for a jmp/call) to serve as a code cave for a detour. Region is never freed (lifetime of the process)."},
                      {"body", {{"near_address", "required"}, {"size", "required"}}}});

        j.push_back({{"name", "patch_revert"}, {"method", "DELETE"}, {"path", "/patch/{id}"},
                      {"description", "Restores a patch's original bytes and removes it from the registry."}});

        j.push_back({{"name", "patch_list"}, {"method", "GET"}, {"path", "/patch/list"},
                      {"description", "Lists active patches (address, original/new bytes, label)."}});

        j.push_back({{"name", "input_key"}, {"method", "POST"}, {"path", "/input/key"},
                      {"description", "Sends a synthetic keyboard event (SendInput, not PostMessage) -- required for games that read the keyboard via exclusive DirectInput. Best-effort brings the game window to the foreground before sending; can fail silently if Windows blocks focus stealing."},
                      {"body", {{"vk", "required: Win32 virtual-key code (e.g. 0x41 for 'A', 0x20 for space)"}, {"down", "required: bool, true=press, false=release"}}}});

        j.push_back({{"name", "input_key_tap"}, {"method", "POST"}, {"path", "/input/key_tap"},
                      {"description", "Shortcut: press then release a key after a delay."},
                      {"body", {{"vk", "required"}, {"hold_ms", "optional, default 50"}}}});

        j.push_back({{"name", "input_mouse_button"}, {"method", "POST"}, {"path", "/input/mouse_button"},
                      {"description", "Synthetic mouse button."},
                      {"body", {{"button", "required: 0=left, 1=right, 2=middle"}, {"down", "required: bool"}}}});

        j.push_back({{"name", "input_mouse_move"}, {"method", "POST"}, {"path", "/input/mouse_move"},
                      {"description", "Relative mouse movement (dx, dy) -- relative, not absolute, since that's what a game using exclusive DirectInput actually reads for its camera."},
                      {"body", {{"dx", "required"}, {"dy", "required"}}}});

        j.push_back({{"name", "input_text"}, {"method", "POST"}, {"path", "/input/text"},
                      {"description", "Types an arbitrary UTF-8 string. background=false uses SendInput "
                                      "with KEYEVENTF_UNICODE (foreground required, works with in-game "
                                      "consoles/chats and any text control). background=true posts "
                                      "WM_CHAR to the game's HWND (background-safe but only reaches "
                                      "WM_CHAR consumers)."},
                      {"body", {{"text", "required: UTF-8 string"},
                                {"background", "optional bool, default false"},
                                {"per_char_ms", "optional int, sleep between chars, default 0"}}}});

        j.push_back({{"name", "input_sequence"}, {"method", "POST"}, {"path", "/input/sequence"},
                      {"description", "Runs a scripted sequence of key/mouse/delay steps on a worker "
                                      "thread and returns immediately with a job id. mode=os uses "
                                      "SendInput (foreground required, reaches DirectInput/RawInput "
                                      "consumers); mode=game uses PostMessage on the game's top-level "
                                      "HWND (background-safe, only reaches games that read WM_KEY*/"
                                      "WM_MOUSE*/WM_CHAR); mode=dinput injects synthetic state into "
                                      "the DirectInput GetDeviceState hook (background-safe, reaches "
                                      "DirectInput-exclusive games like Hitman Contracts). Each step "
                                      "is an object with one of: {vk,down} or {vk,tap_ms}; "
                                      "{mouse_button,down}; {mouse_move:{dx,dy}}; {mouse_move_abs:"
                                      "{x,y}} (client coords, ignored in dinput mode); {delay_ms}. "
                                      "Poll status with GET /input/sequence/{id}, cancel with DELETE."},
                      {"body", {{"mode", "os (default) | game | dinput"},
                                {"steps", "required: array of step objects"}}}});

        j.push_back({{"name", "input_sequence_status"}, {"method", "GET"}, {"path", "/input/sequence/{id}"},
                      {"description", "Returns {status: pending|running|done|failed|cancelled, "
                                      "step_index, step_count}."}});

        j.push_back({{"name", "input_sequence_cancel"}, {"method", "DELETE"}, {"path", "/input/sequence/{id}"},
                      {"description", "Cancels a running sequence at the next step boundary."}});

        j.push_back({{"name", "window_get"}, {"method", "GET"}, {"path", "/window"},
                      {"description", "Returns the game's top-level window state: hwnd, title, class, "
                                      "pid, screen rect, client size, visible/minimized/maximized/"
                                      "focused flags. Handy for the AI to check whether the game is "
                                      "actually up and focused before deciding os vs game mode input."}});

        j.push_back({{"name", "window_focus"}, {"method", "POST"}, {"path", "/window/focus"},
                      {"description", "Restores if minimized, then brings the game window to the "
                                      "foreground. Uses AllowSetForegroundWindow to bypass focus-"
                                      "stealing prevention. Best-effort: still fails when Windows "
                                      "explicitly disallows the transition."}});

        j.push_back({{"name", "window_restore"}, {"method", "POST"}, {"path", "/window/restore"},
                      {"description", "Un-minimizes the game window without changing focus."}});

        j.push_back({{"name", "window_minimize"}, {"method", "POST"}, {"path", "/window/minimize"},
                      {"description", "Minimizes the game window."}});

        j.push_back({{"name", "input_record_start"}, {"method", "POST"}, {"path", "/input/record/start"},
                      {"description", "Starts recording system-wide user input (WH_KEYBOARD_LL / "
                                      "WH_MOUSE_LL). Idempotent."}});

        j.push_back({{"name", "input_record_stop"}, {"method", "POST"}, {"path", "/input/record/stop"},
                      {"description", "Stops recording and returns the captured events as a step array "
                                      "ready to be fed back into POST /input/sequence."}});

        j.push_back({{"name", "network_capture"}, {"method", "POST"}, {"path", "/network/capture"},
                      {"description", "Enables or disables network capture via hooks on ws2_32.dll "
                                      "recv/send/WSARecv/WSASend. Zero cost when disabled."},
                      {"body", {{"enabled", "required: bool"}}}});

        j.push_back({{"name", "network_events"}, {"method", "GET"}, {"path", "/network/events"},
                      {"description", "Ring buffer snapshot of the most recent captured recv/send calls "
                                      "with size + first 64 bytes as hex."},
                      {"query", {{"limit", "optional int, default 200"}}}});

        j.push_back({{"name", "window_move"}, {"method", "POST"}, {"path", "/window/move"},
                      {"description", "Moves (and optionally resizes) the game window. Fires "
                                      "SWP_NOACTIVATE so it does not steal focus."},
                      {"body", {{"x", "required: int screen coord"},
                                {"y", "required: int screen coord"},
                                {"width", "optional int; if omitted or 0, size is preserved"},
                                {"height", "optional int; if omitted or 0, size is preserved"}}}});

        j.push_back({{"name", "freeze_add"}, {"method", "POST"}, {"path", "/freeze"},
                      {"description", "Rewrites a value in a loop at a fixed interval (~16ms), Cheat Engine's \"Freeze\" checkbox -- for infinite health/ammo without going through a code patch. The value is reapplied until DELETE /freeze/{id}, the DLL unloading, or ttl_ms expiring if provided."},
                      {"body", {{"address", "required"}, {"type", "i8|i16|i32|i64|u8|u16|u32|u64|float|double|bytes"},
                                {"value", "required, according to type"}, {"label", "optional: free-form note"},
                                {"ttl_ms", "optional: auto-removal after this delay in ms -- a timed freeze is NOT persisted to disk (would restart its countdown on every reload), unlike a freeze without ttl_ms"}}}});

        j.push_back({{"name", "freeze_delete"}, {"method", "DELETE"}, {"path", "/freeze/{id}"},
                      {"description", "Stops rewriting this address."}});

        j.push_back({{"name", "freeze_list"}, {"method", "GET"}, {"path", "/freeze/list"},
                      {"description", "Lists active freezes."}});

        j.push_back({{"name", "struct_define"}, {"method", "POST"}, {"path", "/struct/define"},
                      {"description", "Defines (or overwrites) a named struct layout -- list of fields {name, offset, type}. Complementary to /project (which only knows individual addresses/pointer paths, not composite layouts). Persisted in the current Cortex project and restored on the next runtime session."},
                      {"body", {{"name", "required"}, {"fields", "required: array of {name, offset, type: i8|i16|i32|i64|u8|u16|u32|u64|float|double|pointer|vtable|vec3|vec4|matrix4|bytes|string, count: optional for bytes/string}"}}}});

        j.push_back({{"name", "struct_delete"}, {"method", "DELETE"}, {"path", "/struct/{name}"},
                      {"description", "Deletes a struct definition."}});

        j.push_back({{"name", "struct_list"}, {"method", "GET"}, {"path", "/struct/list"},
                      {"description", "Lists defined structs."}});

        j.push_back({{"name", "struct_read"}, {"method", "POST"}, {"path", "/struct/read"},
                      {"description", "Reads all fields of a struct in a single call instead of N /memory/read requests."},
                      {"body", {{"name", "required"}, {"address", "required: base address of the instance"}}}});

        j.push_back({{"name", "struct_write"}, {"method", "POST"}, {"path", "/struct/write"},
                      {"description", "Writes several fields of a struct in a single call."},
                      {"body", {{"name", "required"}, {"address", "required"}, {"values", "required: {field: value, ...} -- absent fields left unchanged"}}}});

        j.push_back({{"name", "watch_add"}, {"method", "POST"}, {"path", "/watch"},
                      {"description", "Subscribes to value changes at an address: a background thread re-reads it ~10x/s and records an event on every change, instead of the AI having to repeatedly call /memory/read in a loop and compare itself. Not persisted across sessions (unlike /freeze) -- this is a one-off debugging tool."},
                      {"body", {{"address", "required"}, {"type", "i8|i16|i32|i64|u8|u16|u32|u64|float|double"}, {"label", "optional"}}}});

        j.push_back({{"name", "watch_delete"}, {"method", "DELETE"}, {"path", "/watch/{id}"},
                      {"description", "Stops watching this address."}});

        j.push_back({{"name", "watch_list"}, {"method", "GET"}, {"path", "/watch/list"},
                      {"description", "Lists active watches."}});

        j.push_back({{"name", "watch_events"}, {"method", "GET"}, {"path", "/watch/events"},
                      {"description", "Returns all changes that occurred since the last call (across all watches), then clears the queue -- to be polled at a regular interval rather than re-reading each address individually."}});

        j.push_back({{"name", "watch_allocations"}, {"method", "POST"}, {"path", "/watch/allocations"},
                      {"description", "Enables/disables global allocation tracking (VirtualAlloc/HeapAlloc, hooked via MinHook on the first call with enabled=true and then left in place -- disabling does not unhook them, it just stops recording, to avoid re-hooking on every toggle). Disabled by default because these APIs are called very frequently by any CRT/game allocator; min_size filters out the noise of small allocations before even taking the event queue lock."},
                      {"body", {{"enabled", "optional bool, default true"}, {"min_size", "optional, default 0: minimum size in bytes to record the allocation"}}}});

        j.push_back({{"name", "watch_allocations_events"}, {"method", "GET"}, {"path", "/watch/allocations/events"},
                      {"description", "Returns all allocations recorded since the last call (api VirtualAlloc/HeapAlloc, address, size, protection/flags), then clears the queue."}});

        j.push_back({{"name", "watch_allocations_status"}, {"method", "GET"}, {"path", "/watch/allocations/status"},
                      {"description", "Returns allocation-watch enabled state and minimum recorded allocation size."}});

        j.push_back({{"name", "watch_allocations_events_snapshot"}, {"method", "GET"}, {"path", "/watch/allocations/events_snapshot"},
                      {"description", "Returns a non-destructive snapshot of the bounded allocation-event ring."}});
        j.push_back({{"name", "watch_page_access"}, {"method", "POST"}, {"path", "/watch/page_access"},
                      {"description", "Sets a memory breakpoint via page-guard (like x64dbg/Cheat Engine) on [address, address+size): marks the covered pages PAGE_GUARD, and a dedicated vectored exception handler records every access (read/write/execute) then rearms the guard (single-use by nature) before resuming. Does not affect other guard pages in the process (e.g. the stack) -- passes through if the exception does not concern a region registered here."},
                      {"body", {{"address", "required"}, {"size", "required"}, {"label", "optional"}}}});

        j.push_back({{"name", "watch_page_access_delete"}, {"method", "DELETE"}, {"path", "/watch/page_access/{id}"},
                      {"description", "Removes a page-access watch and restores the original memory protection of the affected pages."}});

        j.push_back({{"name", "watch_page_access_list"}, {"method", "GET"}, {"path", "/watch/page_access/list"},
                      {"description", "Lists active page-access watches."}});

        j.push_back({{"name", "watch_page_access_events"}, {"method", "GET"}, {"path", "/watch/page_access/events"},
                      {"description", "Returns all accesses recorded since the last call (watch_id, exact address, access type read/write/execute), then clears the queue."}});

        j.push_back({{"name", "watch_page_access_events_snapshot"}, {"method", "GET"}, {"path", "/watch/page_access/events_snapshot"},
                      {"description", "Returns a non-destructive snapshot of page-access events, including instruction, registers, stack and before/after bytes."}});
        j.push_back({{"name", "analysis_functions"}, {"method", "POST"}, {"path", "/analysis/functions"},
                      {"description", "Heuristic function detection for a module (Ghidra/IDA-style on a binary without symbols): marks as a function start the byte after a ret+padding (0xCC/0x90) and any target of a direct call. Possible false negatives (a function reached only via a vtable, with no padding before it), rare false positives. 'size' = distance to the next detected function (0 for the last one)."},
                      {"body", {{"module", "required: module name (e.g. HitmanContracts.exe)"}}}});

        j.push_back({{"name", "analysis_cfg"}, {"method", "POST"}, {"path", "/analysis/cfg"},
                      {"description", "Builds the control-flow graph (CFG) of the function starting at 'address': basic blocks split at jmp/jcc/ret, edges typed jump/cond_true/cond_false/fallthrough/indirect (a call does not end a block, control just returns right after). Indirect jumps (register/memory) produce an 'indirect' edge with no statically resolved target."},
                      {"body", {{"address", "required: start address of the function"}}}});

        j.push_back({{"name", "analysis_xrefs"}, {"method", "POST"}, {"path", "/analysis/xrefs"},
                      {"description", "Cross-references to 'target': call/jump/jcc instructions whose resolved target is 'target' (type call/jump/cond_jump), plus instructions with a statically resolvable memory operand pointing at 'target' (type read/write -- absolute addressing in 32-bit, RIP-relative in 64-bit; register-relative addressing like [ebx+8] cannot be resolved without executing the code, so it's missed). With include_data (true by default), also includes references of type 'data_ptr': a pointer value equal to 'target' sitting as-is in a data table (reflection table, vtable, global variable) rather than encoded as an instruction operand -- invisible to code-only xrefs."},
                      {"body", {{"target", "required: targeted address"}, {"module", "optional: restricts to this module, otherwise all loaded modules"},
                                {"include_data", "optional, bool, default true"}}}});

        j.push_back({{"name", "analysis_vtable"}, {"method", "POST"}, {"path", "/analysis/vtable"},
                      {"description", "Dumps a suspected vtable at 'address': reads consecutive pointers and stops at the first one that doesn't point into a loaded executable region (compiler-generated vtables are contiguous arrays of function pointers, so the first non-code value marks the end). Also attempts a best-effort class-name resolution via MSVC RTTI (CompleteObjectLocator at address-ptrSize) -- empty if the binary wasn't compiled with /GR or the object has no RTTI info, rather than guessing."},
                      {"body", {{"address", "required: vtable address"}, {"max_entries", "optional, default 256"}}}});

        j.push_back({{"name", "analysis_structure"}, {"method", "POST"}, {"path", "/analysis/structure"},
                      {"description", "Heuristic 'decompiler-lite' pass on the CFG of the function at 'address': detects loops (an edge whose target <= the start address of the source block = a back edge, a heuristic without full dominance analysis), indents instructions by loop-nesting depth, and adds synthetic comments for loop headers and branch destinations. This is NOT real decompilation: no data-flow analysis, no variable/type/expression reconstruction -- just disassembly with extra structural hints."},
                      {"body", {{"address", "required: start address of the function"}}}});

        j.push_back({{"name", "analysis_pe_headers"}, {"method", "GET"}, {"path", "/analysis/pe_headers"},
                      {"description", "Dissection of a module's PE headers as currently mapped in the process (not the on-disk file -- see analysis_scan_patches for that): DOS/NT headers (entry point, machine, subsystem, timestamp, characteristics, is_dll), section table (name/VA/virtual size/on-disk size/characteristics), import table (module+function or ordinal+IAT slot address, for a possible IAT-hook patch), export table (name or ordinal+resolved address)."},
                      {"query", {{"module", "required: module name (e.g. HitmanContracts.exe)"}}}});

        j.push_back({{"name", "analysis_scan_patches"}, {"method", "POST"}, {"path", "/analysis/scan_patches"},
                      {"description", "Compares the in-memory bytes of a module's executable sections against those of the on-disk file (found via GetModuleFileNameA), to reveal patches applied at runtime by a third party (trainer, anti-cheat, mod) -- independent of patches applied by this tool itself (see patch_list for those). Only IMAGE_SCN_MEM_EXECUTE sections are compared (data sections legitimately differ due to relocations/globals). Two differing zones separated by less than min_run_length bytes are merged into a single zone, to avoid reporting each relocated pointer separately as a distinct patch."},
                      {"body", {{"module", "required: module name"}, {"min_run_length", "optional, default 16: merge distance in bytes between two differing zones"}}}});

        j.push_back({{"name", "dissect_snapshot"}, {"method", "POST"}, {"path", "/dissect/snapshot"},
                      {"description", "Captures the raw bytes at 'address' under an id, Cheat Engine's \"dissect data/structure\": snapshot now, do something in-game, snapshot again, then /dissect/diff the two to see which bytes actually moved -- useful for recovering the layout of an unknown struct without knowing it ahead of time. In-memory only, not persisted across sessions."},
                      {"body", {{"address", "required"}, {"size", "required: number of bytes to capture"}}}});

        j.push_back({{"name", "dissect_list"}, {"method", "GET"}, {"path", "/dissect/snapshots"},
                      {"description", "Lists active snapshots (id, address, size)."}});

        j.push_back({{"name", "dissect_delete"}, {"method", "DELETE"}, {"path", "/dissect/{id}"},
                      {"description", "Deletes a snapshot."}});

        j.push_back({{"name", "dissect_diff"}, {"method", "POST"}, {"path", "/dissect/diff"},
                      {"description", "Compares two same-size snapshots byte by byte, groups contiguous differing bytes into zones, and proposes for each 1/2/4/8-byte zone plausible typed interpretations (i8/u8, i16/u16, i32/u32/float, i64/u64/double) before/after -- to guess a field's type without having to eyeball it."},
                      {"body", {{"a", "required: id of the first snapshot"}, {"b", "required: id of the second snapshot"}}}});

        j.push_back({{"name", "batch_run"}, {"method", "POST"}, {"path", "/batch/run"},
                      {"description", "Executes a sequence of operations in a single call. Fields can reference a previous result with $0.value or $0.matches.0. stop_on_error stops at the first failure. transactional stops and rolls back the memory_write, patch_apply, patch_nop, and freeze_add operations already performed; irreversible mutations are rejected in this mode."},
                      {"body", {{"ops", "required: array of objects {op: name, ...op-specific fields}"},
                                {"stop_on_error", "optional, default false"},
                                {"transactional", "optional, default false"}}}});

        j.push_back({{"name", "events"}, {"method", "GET"}, {"path", "/events"},
                      {"description", "Authenticated Server-Sent Events stream. Resumes after a disconnect via Last-Event-ID or ?since=. Emits notably watch.change, prompt.answered, and actions/rollbacks."}});

        j.push_back({{"name", "actions_list"}, {"method", "GET"}, {"path", "/actions"},
                      {"description", "Lists the bounded journal of reversible mutations and the next checkpoint."}});
        j.push_back({{"name", "actions_rollback"}, {"method", "POST"}, {"path", "/actions/rollback"},
                      {"description", "Undoes actions in reverse order, all of them or from a checkpoint."},
                      {"body", {{"checkpoint", "optional: checkpoint id returned by GET /actions"}}}});
        j.push_back({{"name", "actions_clear"}, {"method", "POST"}, {"path", "/actions/clear"},
                      {"description", "Clears the undo history without modifying memory."}});
        const json callAddress = {{"oneOf", json::array({{{"type","integer"}},{{"type","string"}},{{"type","object"}}})},
                                  {"required", true},
                                  {"description", "Function address: absolute integer, decimal/hex string, module+RVA, or {module,rva}."}};
        const json callArgs = {{"type","array"},{"maxItems",8},
                               {"items",{{"oneOf",json::array({{{"type","integer"}},{{"type","string"}},{{"type","object"}}})}}},
                               {"description","0..8 pointer-width arguments. Address-like values accept integer, decimal/hex string, module+RVA, or {module,rva}."}};
        const json callConvention = {{"type","string"},{"enum",json::array({"cdecl","stdcall","thiscall","fastcall"})},
                                     {"description","x86 calling convention; ignored by the unified x64 ABI. Default cdecl."}};
        const json callTimeout = {{"type","integer"},{"minimum",1},{"maximum",60000},
                                  {"description","Wait timeout in milliseconds. Default 3000."}};
        j.push_back({{"name", "call_function"}, {"method", "POST"}, {"path", "/call/function"},
                      {"description", "Legacy immediate native call on the API worker thread. Prefer call_on_game_thread for engine functions with thread affinity."},
                      {"body", {{"address", callAddress}, {"args", callArgs}, {"convention", callConvention}}}});
        j.push_back({{"name", "call_on_game_thread"}, {"method", "POST"}, {"path", "/call/game-thread"},
                      {"description", "Queues a native call onto the thread observed by Cortex's frame/present hook and executes it on the next frame. Returns return value, TID, duration, exception code, or bounded timeout."},
                      {"body", {{"address", callAddress}, {"args", callArgs}, {"convention", callConvention}, {"timeout_ms", callTimeout}}}});
        j.push_back({{"name", "call_on_thread"}, {"method", "POST"}, {"path", "/call/thread"},
                      {"description", "Cooperatively executes a native call on a specific target TID. Frame thread uses the next-frame queue; other TIDs must own a Windows message queue. Unsafe context hijacking is deliberately refused."},
                      {"body", {{"thread_id", {{"type","integer"},{"required",true},{"minimum",1},{"description","Target thread id."}}},
                                {"address", callAddress}, {"args", callArgs}, {"convention", callConvention}, {"timeout_ms", callTimeout}}}});
        j.push_back({{"name", "call_game_thread_status"}, {"method", "GET"}, {"path", "/call/game-thread/status"},
                      {"description", "Reports observed frame/game TID, last frame time, activity age, and pending native-call count."}});


        j.push_back({{"name","memory_ownership"},{"method","GET"},{"path","/memory/ownership"},
                     {"description","Lists the ranges owned by Cortex and excluded from scans by default."}});
        j.push_back({{"name","pointermap_capture"},{"method","POST"},{"path","/pointermap/capture"},
                     {"description","Captures and persists the pointer paths found for a target."},
                     {"body",{{"name","stable name"},{"target","target address"},{"max_depth","optional"},{"max_offset","optional"}}}});
        j.push_back({{"name","pointermap_list"},{"method","GET"},{"path","/pointermap/list"},{"description","Lists the game's persistent pointer maps."}});
        j.push_back({{"name","pointermap_intersect"},{"method","POST"},{"path","/pointermap/intersect"},
                     {"description","Intersects several sessions and scores the stable paths."},{"body",{{"names","at least two names"}}}});
        j.push_back({{"name","pointermap_delete"},{"method","DELETE"},{"path","/pointermap/{name}"},{"description","Deletes a persisted pointer map."}});
        j.push_back({{"name","trace_start"},{"method","POST"},{"path","/trace/start"},
                     {"description","Starts a bounded single-step trace with thread/range filters and coverage."}});
        j.push_back({{"name","trace_stop"},{"method","POST"},{"path","/trace/{id}/stop"},
                     {"description","Stops an active trace while preserving its captured events and coverage."}});
        j.push_back({{"name","trace_delete"},{"method","DELETE"},{"path","/trace/{id}"},
                     {"description","Deletes a trace and its captured execution data."}});
        j.push_back({{"name","trace_list"},{"method","GET"},{"path","/trace/list"},{"description","Lists traces and their state."}});
        j.push_back({{"name","trace_events"},{"method","GET"},{"path","/trace/{id}/events"},{"description","Returns captured instructions, bytes, and register deltas."}});
        j.push_back({{"name","trace_coverage"},{"method","GET"},{"path","/trace/{id}/coverage"},{"description","Heatmap of executed addresses."}});
        j.push_back({{"name","trace_callgraph"},{"method","GET"},{"path","/trace/{id}/callgraph"},{"description","Dynamic call graph of observed direct calls."}});
        j.push_back({{"name","trace_compare"},{"method","POST"},{"path","/trace/compare"},{"description","Compares the coverage of two actions."}});
        j.push_back({{"name","struct_infer"},{"method","POST"},{"path","/struct/infer"},
                     {"mutation_permission_when","define=true"},
                     {"description","Infers a typed layout from several instances and can optionally persist it in the current Cortex project."},
                     {"body",{{"instances","required: array of instance addresses"},{"size","required: bytes to infer (4..1048576)"},
                               {"define","optional bool, default false; requires mutation_permission when true"},
                               {"name","optional; required when define=true"}}}});
        j.push_back({{"name","patch_trampoline"},{"method","POST"},{"path","/patch/trampoline"},
                     {"description","Builds a detour with a gateway and relocation of relative instructions."}});
        j.push_back({{"name","snapshot_create"},{"method","POST"},{"path","/snapshot/create"},{"description","Captures several memory ranges into a target checkpoint."},
                     {"body",{{"ranges","required: array of {address,size}"},{"label","optional"}}}});
        j.push_back({{"name","snapshot_list"},{"method","GET"},{"path","/snapshot/list"},{"description","Lists in-memory target checkpoints."}});
        j.push_back({{"name","snapshot_diff"},{"method","POST"},{"path","/snapshot/diff"},{"description","Compares two checkpoints."},
                     {"body",{{"from","required snapshot id"},{"to","required snapshot id"}}}});
        j.push_back({{"name","snapshot_rewind"},{"method","POST"},{"path","/snapshot/{id}/rewind"},{"description","Restores a checkpoint with a logged undo."}});
        j.push_back({{"name","snapshot_last_change"},{"method","POST"},{"path","/snapshot/last_change"},{"description","Finds the last observed transition for a value."},
                     {"body",{{"address","required"},{"size","required, 1..4096"}}}});
        j.push_back({{"name","snapshot_delete"},{"method","DELETE"},{"path","/snapshot/{id}"},{"description","Deletes a checkpoint from the runtime timeline."}});
        const json reAddress = {{"oneOf",json::array({{{"type","integer"}},{{"type","string"}},{{"type","object"}}})},
                                {"description","Address: integer, decimal/hex string, module+RVA, or {module,rva}."}};
        json reAddressRequired = reAddress; reAddressRequired["required"] = true;
        const json anyJsonValue = {{"oneOf",json::array({{{"type","string"}},{{"type","integer"}},{{"type","number"}},{{"type","boolean"}},{{"type","object"}},{{"type","array"}},{{"type","null"}}})},
                                   {"description","Any JSON value."}};
        json anyJsonValueRequired = anyJsonValue; anyJsonValueRequired["required"] = true;
        j.push_back({{"name","re_track_object"},{"method","POST"},{"path","/re/object/track"},
                     {"description","Persistently tracks an object across address changes and records changed byte ranges, pointed objects, vtables/subobjects, destruction/reappearance, and related allocation metadata."},
                     {"body",{{"name",{{"type","string"},{"required",true},{"description","Stable track name."}}},
                              {"address",reAddress},{"pointer_path",{{"type","string"},{"description","Optional persisted project pointer-path name; when set it is re-resolved on every sample."}}},
                              {"size",{{"type","integer"},{"minimum",1},{"maximum",4096},{"description","Bytes to observe, default 256."}}},
                              {"persist",{{"type","boolean"},{"description","Persist into the RE session project, default true."}}}}}});
        j.push_back({{"name","re_object_tracks"},{"method","GET"},{"path","/re/object/tracks"},{"description","Lists tracked objects and their current address/alive state."}});
        j.push_back({{"name","re_object_get"},{"method","GET"},{"path","/re/object/{id}"},{"description","Returns the current tracked-object snapshot with pointers, vtables/subobjects and allocation metadata."}});
        j.push_back({{"name","re_object_events"},{"method","GET"},{"path","/re/object/{id}/events"},{"description","Returns non-destructive tracked-object events: relocation, destruction/reappearance and changed byte ranges."}});
        j.push_back({{"name","re_object_delete"},{"method","DELETE"},{"path","/re/object/{id}"},{"description","Stops and removes an object track."}});
        j.push_back({{"name","re_object_compare"},{"method","POST"},{"path","/re/object/compare"},
                     {"description","Compares two tracked object snapshots byte-by-byte and compares detected C++ subobjects/vtables."},
                     {"body",{{"a",{{"type","integer"},{"required",true}}},{"b",{{"type","integer"},{"required",true}}}}}});
        j.push_back({{"name","re_session"},{"method","GET"},{"path","/re/session"},
                     {"description","Returns persistent reverse-engineering facts, persisted object-track locators and suggested breakpoint templates for the current target project."}});
        j.push_back({{"name","re_session_fact_set"},{"method","POST"},{"path","/re/session/fact"},
                     {"description","Stores or updates a proved RE fact in the persistent target project."},
                     {"body",{{"key",{{"type","string"},{"required",true}}},{"value",anyJsonValueRequired}}}});
        j.push_back({{"name","re_session_fact_delete"},{"method","DELETE"},{"path","/re/session/fact"},
                     {"description","Deletes a persistent RE fact with undo support."},
                     {"body",{{"key",{{"type","string"},{"required",true}}}}}});
        j.push_back({{"name","re_session_breakpoints"},{"method","POST"},{"path","/re/session/breakpoints"},
                     {"description","Stores breakpoint templates suggested for the next RE session; Cortex does not auto-arm them without an explicit mutation action."},
                     {"body",{{"templates",{{"type","array"},{"required",true},{"items",{{"type","object"}}}}}}}});
        const json reTestSteps = {{"type","array"},{"required",true},{"items",{{"type","object"}}},
                                  {"description","Steps: press (key name such as F7 or numeric vk), delay, wait/assert (including exists=true), call_game_thread, tool, checkpoint."}};
        j.push_back({{"name","re_test_run"},{"method","POST"},{"path","/re/test/run"},
                     {"description","Runs an automated in-game RE test and returns PASS/FAIL. Supports input, waits/asserts, game-thread native calls and arbitrary registered Cortex tools under one Actions transaction. Default is rollback; commit=true keeps reversible mutations."},
                     {"body",{{"steps",reTestSteps},{"rollback_ranges",{{"type","array"},{"items",{{"type","object"}}},{"description","Optional memory ranges captured before the test and restored afterwards, including side effects from native calls."}}},
                              {"commit",{{"type","boolean"},{"description","Keep reversible changes only if the test passes; default false."}}},
                              {"stop_on_failure",{{"type","boolean"},{"description","Default true."}}}}}});
        j.push_back({{"name","re_experiment_run"},{"method","POST"},{"path","/re/experiment/run"},
                     {"description","Alias of re_test_run for checkpointed experiments: patch/call/struct-write/assert then automatic Actions rollback plus explicit memory-range restoration."},
                     {"body",{{"steps",reTestSteps},{"rollback_ranges",{{"type","array"},{"items",{{"type","object"}}}}},{"commit",{{"type","boolean"}}}}}});
        j.push_back({{"name","re_session_apply_breakpoints"},{"method","POST"},{"path","/re/session/apply-breakpoints"},
                     {"description","Explicitly arms the breakpoint templates persisted for this RE target. Nothing is auto-armed at startup; this opt-in action keeps restored sessions safe."},
                     {"body",{{"stop_on_error",{{"type","boolean"},{"description","Stop at first failed template, default true."}}}}}});
        j.push_back({{"name","re_cpp_subobjects"},{"method","POST"},{"path","/re/cpp/subobjects"},
                     {"description","Detects multiple vtables/C++ subobjects and common this-adjustment thunks inside an object."},
                     {"body",{{"address",reAddressRequired},{"size",{{"type","integer"},{"minimum",8},{"maximum",4096},{"description","Object bytes to inspect, default 256."}}}}}});
        j.push_back({{"name","re_find_last_writer"},{"method","POST"},{"path","/re/last-writer"},
                     {"description","High-level who-wrote-this primitive. Installs a temporary process-global HW write breakpoint and returns instruction/caller names, registers, this/vtable, callstack, old/new bytes, or bounded timeout."},
                     {"body",{{"address",reAddressRequired},{"size",{{"type","integer"},{"enum",json::array({1,2,4})},{"description","Hardware watch size."}}},
                              {"timeout_ms",{{"type","integer"},{"minimum",1},{"maximum",60000},{"description","Wait timeout, default 5000."}}}}}});
        j.push_back({{"name","re_trace_transition"},{"method","POST"},{"path","/re/transition/trace"},
                     {"description","Records a bounded transition timeline by combining up to four HW write watches and software execution probes until an optional value predicate becomes true."},
                     {"body",{{"watches",{{"type","array"},{"maxItems",4},{"items",{{"type","object"}}},{"description","[{address,size,label}]"}}},
                              {"probes",{{"type","array"},{"items",{{"type","object"}}},{"description","[{address,label}]"}}},
                              {"until",{{"type","object"},{"description","Optional {address,size,value,op} state predicate."}}},
                              {"timeout_ms",{{"type","integer"},{"minimum",1},{"maximum",60000}}},
                              {"max_events",{{"type","integer"},{"minimum",1},{"maximum",4096}}}}}});
        j.push_back({{"name","ghidra_export"},{"method","POST"},{"path","/ghidra/export"},{"description","Exports the runtime and generates the CortexImport.py script."}});
        const json ghidraImportBody = {{"addresses",{{"type","array"},{"items",{{"type","object"}}}}},
                                       {"symbols",{{"type","array"},{"items",{{"type","object"}}}}},
                                       {"vtables",{{"type","array"},{"items",{{"type","object"}}}}},
                                       {"structs",{{"type","array"},{"items",{{"type","object"}}}}},
                                       {"xrefs",{{"type","array"},{"items",{{"type","object"}}}}},
                                       {"re_facts",{{"type","object"}}}};
        j.push_back({{"name","ghidra_import"},{"method","POST"},{"path","/ghidra/import"},
                     {"description","Imports Ghidra names/symbols, types, comments, vtables, structs, xrefs and RE facts into the persistent Cortex project."},{"body",ghidraImportBody}});
        j.push_back({{"name","ghidra_import_symbols"},{"method","POST"},{"path","/ghidra/import"},
                     {"description","AI-friendly alias for ghidra_import; accepts symbols/addresses/vtables/structs/xrefs/re_facts at the document root."},{"body",ghidraImportBody}});

        return j;
}

namespace {
std::string ContractSamplePath(std::string path) {
    size_t cursor = 0;
    while ((cursor = path.find('{', cursor)) != std::string::npos) {
        const size_t close = path.find('}', cursor + 1);
        if (close == std::string::npos) break;
        path.replace(cursor, close - cursor + 1, "1");
        cursor += 1;
    }
    return path;
}
}

bool ValidateApiContracts(json& report) {
    json errors = json::array();
    std::set<std::string> names;
    const json manifest = BuildToolsManifest();
    for (const auto& tool : manifest) {
        const std::string name = tool.value("name", std::string());
        const std::string method = tool.value("method", std::string());
        const std::string path = tool.value("path", std::string());
        if (name.empty() || method.empty() || path.empty()) {
            errors.push_back({{"tool",name},{"error","manifest_identity_missing"}});
            continue;
        }
        if (!names.insert(name).second) errors.push_back({{"tool",name},{"error","duplicate_tool_name"}});
        const std::string sample = ContractSamplePath(path);
        if (!HasNativeRoute(method, sample)) {
            errors.push_back({{"tool",name},{"method",method},{"path",path},{"sample_path",sample},
                              {"error","registered_handler_missing"}});
        }
        if (tool.contains("body") && tool["body"].is_object()) {
            for (auto it = tool["body"].begin(); it != tool["body"].end(); ++it) {
                const json schema = mcp_contract::SchemaForProperty(it.key(), it.value());
                if (!schema.is_object() ||
                    (!schema.contains("type") && !schema.contains("oneOf") && !schema.contains("anyOf"))) {
                    errors.push_back({{"tool",name},{"field",it.key()},{"error","invalid_body_schema"}});
                }
                if (schema.contains("required") && schema["required"].is_boolean()) {
                    errors.push_back({{"tool",name},{"field",it.key()},{"error","required_marker_leaked_into_property_schema"}});
                }
            }
        }
    }
    report = {{"ok",errors.empty()}, {"tool_count",manifest.size()},
              {"native_route_count",NativeRouteCount()}, {"errors",std::move(errors)}};
    return report.value("ok", false);
}
json BuildOpenApiDocument() {
    json paths = json::object();
    for (const auto& tool : BuildToolsManifest()) {
        std::string method = tool.value("method", "GET");
        std::transform(method.begin(), method.end(), method.begin(), ::tolower);
        json operation{{"operationId", tool.value("name", "tool")},
                       {"summary", tool.value("description", "")},
                       {"responses", {{"200", {{"description", "Cortex response"}}},
                                      {"400", {{"description", "Invalid request"}}},
                                      {"500", {{"description", "Runtime failure"}}}}}};
        json parameters = json::array();
        for (const auto& parameter : mcp_contract::PathParameters(tool.value("path", std::string()))) {
            parameters.push_back({{"name",parameter},{"in","path"},{"required",true},
                                  {"schema",{{"oneOf",json::array({{{"type","integer"}},{{"type","string"}}})}}}});
        }
        if (tool.contains("query") && tool["query"].is_object()) {
            for (auto it = tool["query"].begin(); it != tool["query"].end(); ++it) {
                parameters.push_back({{"name",it.key()},{"in","query"},
                                      {"required",mcp_contract::IsRequiredSpec(it.value())},
                                      {"schema",mcp_contract::SchemaForProperty(it.key(),it.value())}});
            }
        }
        if (!parameters.empty()) operation["parameters"] = std::move(parameters);
        if (tool.contains("body") && tool["body"].is_object()) {
            json properties = json::object();
            json required = json::array();
            for (auto it = tool["body"].begin(); it != tool["body"].end(); ++it) {
                properties[it.key()] = mcp_contract::SchemaForProperty(it.key(), it.value());
                if (mcp_contract::IsRequiredSpec(it.value())) required.push_back(it.key());
            }
            json schema{{"type","object"},{"properties",std::move(properties)}};
            if (!required.empty()) schema["required"] = required;
            operation["requestBody"] = {{"required", !required.empty()},
                                         {"content", {{"application/json", {{"schema", std::move(schema)}}}}}};
        }
        if (!tool.value("public", false)) operation["security"] = json::array({{{"CortexToken", json::array()}}});
        paths[tool.at("path").get<std::string>()][method] = std::move(operation);
    }
    return {{"openapi", "3.1.0"},
            {"info", {{"title", "Cortex API"}, {"version", "2.1.0"}}},
            {"servers", json::array({{{"url", "http://127.0.0.1:" + std::to_string(GetPort())}}})},
            {"paths", paths},
            {"components", {{"securitySchemes", {{"CortexToken", {{"type", "apiKey"}, {"in", "header"}, {"name", "X-Cortex-Token"}}}}}}}};
}

void RegisterStatusRoutes(httplib::Server& svr) {
    svr.Get("/status", [](const httplib::Request&, httplib::Response& res) {
        json j{{"status", "ok"}, {"pid", GetCurrentProcessId()}, {"process_name", ProcessName()},
               {"port", GetPort()}, {"uptime_ms", GetUptimeMs()}};
        res.set_content(j.dump(), "application/json");
        overlay::LogApiCall("GET /status");
    });

    // Desktop-only diagnostics surface. Deliberately omitted from the MCP
    // tool manifest so an agent cannot use the UI activity feed as another
    // public protocol surface.
    svr.Get("/ui/api-log", [](const httplib::Request&, httplib::Response& res) {
        json lines = json::array();
        for (const auto& line : overlay::ApiLogSnapshot()) lines.push_back(line);
        res.set_content(json{{"ok", true}, {"lines", lines}}.dump(), "application/json");
    });
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json hooks = json::object();
#ifdef CORTEX_KIERO
        hooks["kiero"] = {{"installed", hook::IsKieroHookInstalled()}, {"backend", hook::GetKieroRenderBackend()}};
#endif
#ifdef CORTEX_D3D8
        hooks["d3d8"] = {{"installed", hook::IsD3D8HookInstalled()}};
#endif
        json j{{"ok", IsRunning()}, {"api", {{"running", IsRunning()}, {"last_error", GetLastError()},
                                      {"authentication", "X-Cortex-Token"}, {"token_file", GetTokenPath()}}},
               {"process", {{"pid", GetCurrentProcessId()}, {"name", ProcessName()},
#ifdef _WIN64
                            {"bitness", 64}
#else
                            {"bitness", 32}
#endif
                           }}, {"hooks", hooks}, {"uptime_ms", GetUptimeMs()}};
        res.status = IsRunning() ? 200 : 503;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/tools", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(BuildToolsManifest().dump(2), "application/json");
        overlay::LogApiCall("GET /tools");
    });

    svr.Get("/schema/validate", [](const httplib::Request&, httplib::Response& res) {
        json report;
        const bool ok = ValidateApiContracts(report);
        res.status = ok ? 200 : 500;
        res.set_content(report.dump(2), "application/json");
    });
    svr.Get("/openapi.json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(BuildOpenApiDocument().dump(2), "application/json");
    });
}

} // namespace api
















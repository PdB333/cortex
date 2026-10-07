# Cheat Engine baseline (configurations A and B)

Pinned: **`miscusi-peek/cheatengine-mcp-bridge` commit `6bd7ce90479250a9b4b75e7944df9c105ecb2572`** (merge of pull request #60, 2026-08-14), bridge version
12.0.0. Read directly from the checkout, not through a summary. Everything below is derived from
`MCP_Server/mcp_cheatengine.py` of that commit by `bench/tools/make_ce_risks.py`; regenerate it after any change of pin.

## What it is

- Three parts: `mcp_cheatengine.py` (Python MCP server, FastMCP, stdio), `ce_mcp_bridge.lua` (6 127 lines, runs
  inside Cheat Engine and serves a Windows named pipe `\\.\pipe\CE_MCP_Bridge_v99` with 4-byte length-prefixed
  JSON-RPC), and an optional `ce_tcp_relay.py` that exposes the same framing on TCP.
- **175 tools** registered with `@mcp.tool()` (the README says "~180"). Results are JSON text in a string, not MCP
  structured content.
- Needs: **Cheat Engine running on Windows with the Lua bridge loaded**, `mcp` and `pywin32` (pipe mode) or just
  `mcp` (TCP mode). The server is not self-contained: it is a thin client to a GUI application.
- Defaults that matter for the benchmark: per-call timeout 30 s (`CE_MCP_TIMEOUT`), responses capped at 32 MB.

## Limits and properties relevant to the comparison

- **No permission model.** Every tool runs unconditionally except `run_command` and `shell_execute`, which return
  `PERMISSION_DENIED` unless the environment variable `CE_MCP_ALLOW_SHELL=1` is set at startup (left unset in the
  benchmark, as shipped). `write_memory`, `evaluate_lua`, `auto_assemble`, `inject_dll`, `execute_code`,
  `allocate_memory`… need nothing from the person. For task 10, mutation attempts and effective mutations are
  therefore expected to be equal under A and B.
- **No rollback, no transactions, no checkpoints, no persistent facts.** B only adds the notes file the harness gives.
- **No process pinning in the tool itself**: the agent attaches with `open_process`. In the benchmark the harness
  attaches Cheat Engine to the target before the agent starts (a `setup_calls` entry), matching Cortex's pre-loaded runtime.
- Kernel-mode and DBVM tools (`dbk_*`, `*_cr3`, `*_dbvm_watch`, `allocate_kernel_memory`, `map_memory`) require the DBK
  driver or DBVM; the README warns that "Query memory region routines" must be disabled to avoid blue screens with DBVM.
  A dedicated VM without those features simply returns errors for them.
- Windows only for pipe mode; TCP mode lets the Python server run elsewhere, but Cheat Engine itself still needs Windows.

## Classification used by the proxy

The bridge has no risk classes; the benchmark assigns them by name (`configs/ce_tool_risks.json`) so that
"attempts to change the target" are counted the same way as for Cortex. Counts: native_call 13, mutate 27, control 29, analyze 99, observe 7.
Debugger, scan-pause and table tools are `control`, counted apart from `mutate`/`native_call` attempts.


### native_call — runs arbitrary code or starts programs (13)

| tool | arguments | first line of its description |
|---|---|---|
| `create_process` | `path`, `args`, `debug`, `break_on_entry` | Create and optionally debug a new process. |
| `create_thread` | `code`, `arg` | Execute Lua code in a new CE thread. |
| `evaluate_lua` | `code` | Execute arbitrary Lua code in Cheat Engine. |
| `execute_code` | `address`, `param`, `timeout` | Call a stdcall function with one argument at the given address in the target process. |
| `execute_code_ex` | `call_method`, `timeout`, `address`, `args` | Call a function with an explicit calling convention and multiple arguments. |
| `execute_code_local` | `address`, `param` | Call a stdcall function inside Cheat Engine's own process (NOT the target). |
| `execute_code_local_ex` | `address`, `args`, `call_method` | Call a function inside Cheat Engine's own process with explicit calling convention. |
| `execute_method` | `address`, `instance`, `args`, `call_method`, `timeout` | Call a C++ instance method with an implicit 'this' pointer in the target process. |
| `inject_dll` | `filepath`, `skip_symbol_reload` | Inject a DLL into the currently attached target process. |
| `inject_dotnet_dll` | `filepath`, `class_name`, `method_name`, `param`, `timeout` | Inject a .NET DLL and invoke a static method in the target process. |
| `queue_to_main_thread` | `code` | Queue Lua code to run on CE's main thread without waiting for its result. |
| `run_command` | `command`, `args` | Execute a shell command in the host OS. SECURITY: Arbitrary code execution. |
| `shell_execute` | `command`, `args`, `verb`, `working_dir`, `showcommand` | Invoke Windows ShellExecute. SECURITY: Arbitrary code execution. |

### mutate — changes the target, its memory protection or the host (27)

| tool | arguments | first line of its description |
|---|---|---|
| `allocate_kernel_memory` | `size` | Allocate non-paged kernel memory via the DBK driver. |
| `allocate_memory` | `size`, `base_address`, `protection` | Allocate memory in the target process. |
| `allocate_shared_memory` | `name`, `size` | Create and map a shared memory region in the target process. |
| `auto_assemble` | `script` | Run an AutoAssembler script (injection, code caves, etc). |
| `compile_c_code` | `source`, `address`, `target_self`, `kernelmode` | Compile C source code using CE's built-in TCC compiler. |
| `compile_cs_code` | `source`, `references`, `core_assembly` | Compile C# source code using CE's .NET compiler (requires .NET 4+). |
| `copy_memory` | `source`, `size`, `dest`, `method` | Copy memory between addresses. Methods: 0=target→target, 1=target→CE, 2=CE→target, 3=CE→CE. Returns dest_address allocated by CE if dest is None. |
| `create_section` | `size` | Create a Windows section (shared memory) of the given size. Returns a handle as a hex string. |
| `dbk_writes_ignore_write_protection` | `enable` | Toggle whether DBK memory writes bypass copy-on-write (CoW) protection. |
| `debug_set_context` | `registers` | Set CPU register values in the paused thread. Pass a dict like {"RAX": "0x1234", "RIP": "0x140001000"}. |
| `delete_file` | `filename` | Delete a file at the given path. Returns {success}. |
| `do_key_press` | `vk` | Simulate a full key press (down + up) for the given key. |
| `free_memory` | `address`, `size` | Free memory previously allocated in the target process. |
| `full_access` | `address`, `size` | Grant full read-write-execute access to a memory region (convenience wrapper). |
| `key_down` | `vk` | Simulate pressing a key down (does NOT release it automatically). |
| `key_up` | `vk` | Release a key that was pressed with key_down. |
| `map_view_of_section` | `handle`, `address`, `size` | Map a section into the target process. 'handle' is from create_section. 'address' is optional preferred base. Returns mapped_address. |
| `read_region_from_file` | `filename`, `destination` | Read a file into memory at the given destination address. Filename must be an absolute path and must not contain '..' components. |
| `send_window_message` | `handle`, `msg`, `wparam`, `lparam` | Send a Windows message (WM_*) to a window. |
| `set_memory_protection` | `address`, `size`, `read`, `write`, `execute` | Change the protection flags of a memory region in the target process. |
| `set_memory_record_value` | `id`, `value` | Write a value to a memory record (and therefore to the target process memory). |
| `set_mouse_pos` | `x`, `y` | Move the mouse cursor to screen position (x, y). |
| `write_integer` | `address`, `value`, `type` | Write a number to memory. Types: byte, word, dword, qword, float, double. |
| `write_memory` | `address`, `bytes` | Write raw bytes to memory. |
| `write_process_memory_cr3` | `cr3`, `address`, `bytes` | Write to virtual memory using an explicit CR3 page-table base via DBK/DBVM. |
| `write_region_to_file` | `address`, `size`, `filename` | Write a memory region to a file. Filename must be an absolute path and must not contain '..' components. |
| `write_string` | `address`, `value`, `wide` | Write a string to memory (ASCII or Wide/UTF-16). |

### control — breakpoints, pausing, tables, dialogs, symbols that download (29)

| tool | arguments | first line of its description |
|---|---|---|
| `clear_all_breakpoints` | — | Remove ALL breakpoints. |
| `create_memory_record` | `description`, `address`, `var_type` | Create a new memory record in the cheat table address list. |
| `debug_break_thread` | `thread_id` | Break a specific thread by its thread ID. |
| `debug_continue` | `method` | Continue execution from a breakpoint. |
| `debug_detach` | — | Detach the debugger from the target process if possible. |
| `debug_process` | `interface` | Start the CE debugger for the currently opened process. |
| `debug_remove_breakpoint_for_thread` | `thread_id`, `address` | Remove a per-thread breakpoint at the given address for the given thread. |
| `debug_set_breakpoint_for_thread` | `thread_id`, `address`, `size`, `trigger` | Set a breakpoint that fires only on a specific thread. trigger: execute/write/read/access. |
| `debug_set_last_branch_recording` | `enable` | Enable or disable Intel LBR (Last Branch Recording). Requires kernel-mode debugger. |
| `delete_memory_record` | `id` | Delete a memory record from the cheat table address list by ID. |
| `enable_kernel_symbols` | — | Enable kernel-mode symbol resolution (requires DBK driver). |
| `enable_windows_symbols` | — | Trigger download and load of Windows PDB symbol files. |
| `input_query` | `caption`, `prompt`, `default` | Show a modal text-input dialog in Cheat Engine and return what the user typed. |
| `load_table` | `filename`, `merge` | Load a Cheat Engine table (.ct) file into the current session. |
| `map_memory` | `address`, `size` | Map a kernel/physical address range into the CE usermode context via DBK. |
| `output_debug_string` | `message` | Post a message to the Windows debugger via OutputDebugString (readable with tools like DebugView). |
| `pause_process` | — | Pause (freeze) the currently opened process using CE's global pause() function. |
| `remove_breakpoint` | `id` | Remove a breakpoint by its ID. |
| `save_table` | `filename`, `protect` | Save the current cheat table to a file. |
| `set_breakpoint` | `address`, `id`, `capture_registers`, `capture_stack`, `stack_depth` | Set a hardware execution breakpoint. Non-breaking/Logging only. |
| `set_data_breakpoint` | `address`, `id`, `access_type`, `size` | Set a hardware data breakpoint (watchpoint). Types: 'r' (read), 'w' (write), 'rw' (access). |
| `set_global_variable` | `name`, `value` | Write a global variable in CE's main Lua state. |
| `show_message` | `message` | Show a modal message dialog in Cheat Engine. |
| `show_selection_list` | `caption`, `prompt`, `options` | Show a modal list-selection dialog in Cheat Engine. |
| `start_dbvm_watch` | `address`, `mode`, `max_entries` | Start invisible DBVM hypervisor watch. Modes: 'w' (writes), 'r' (reads), 'x' (execute). |
| `stop_dbvm_watch` | `address` | Stop DBVM watch and return results. |
| `unmap_memory` | `mapped_address`, `size` | Release a memory mapping created by map_memory(). |
| `unpause_process` | — | Resume (unfreeze) the currently opened process using CE's global unpause() function. |
| `write_clipboard` | `text` | Write text to the system clipboard. Returns {success}. |

### analyze — reads, scans and CE-local bookkeeping (99)

| tool | arguments | first line of its description |
|---|---|---|
| `add_element_to_structure` | `structure_id`, `name`, `offset`, `type` | Add a new element to an existing CE structure. |
| `analyze_function` | `address` | Analyze a function to find all CALL instructions output (calls made by this function). |
| `analyze_pointer_access` | `instruction`, `registers`, `accessed_address`, `is_64bit` | Parse a captured memory access (instruction + register snapshot from a breakpoint/DBVM hit) into base register, displacement, and the concrete struct-base address for pointer-chain walk-back. Pure analysis; no process access. next_scan_value is the address to scan for (as a pointer) to find the next level up. |
| `aob_scan` | `pattern`, `protection`, `limit` | Scan for an Array of Bytes (AOB) pattern. Example: '48 89 5C 24'. |
| `aob_scan_module` | `pattern`, `module_name`, `protection` | Scan for an AOB pattern restricted to a specific module's memory range. |
| `aob_scan_module_unique` | `pattern`, `module_name`, `protection` | Scan for an AOB pattern in a specific module that must match exactly once. |
| `aob_scan_unique` | `pattern`, `protection` | Scan for an AOB pattern that must match exactly once. Returns {success, address} or error with count. |
| `assemble_instruction` | `line`, `address`, `preference`, `skip_range_check` | Assemble a single x86/x64 instruction into bytes. |
| `auto_assemble_check` | `script`, `enable`, `target_self` | Validate an Auto Assembler script for syntax errors without executing it. |
| `check_synchronize` | — | Process queued main-thread calls (checkSynchronize). |
| `checksum_memory` | `address`, `size` | Calculate MD5 checksum of a memory region to detect changes. |
| `compare_memory` | `addr1`, `addr2`, `size`, `method` | Compare two memory regions. Methods: 0=target/target, 1=addr1=target addr2=CE, 2=both CE. Returns equal flag and first_diff byte index (-1 if equal). |
| `create_persistent_scan` | `name` | Create a named, stateful memory scan session. Use the name with persistent_scan_* tools. |
| `create_structure` | `name` | Create a new empty CE structure definition and add it to the global list. |
| `dbk_get_cr0` | — | Read Control Register 0 (CR0) via the DBK kernel driver. |
| `dbk_get_cr3` | — | Read Control Register 3 (CR3 — page-table base) via DBK or DBVM. |
| `dbk_get_cr4` | — | Read Control Register 4 (CR4) via the DBK kernel driver. |
| `debug_get_context` | `extra_regs` | Get the current thread's CPU register context. Set extra_regs=True to include XMM0-15 and FP0-7. |
| `debug_get_current_debugger_interface` | — | Return the active debugger interface used by CE. |
| `debug_get_last_branch_record` | `index` | Get the from/to addresses of a Last Branch Record entry at the given index. |
| `debug_get_xmm_pointer` | `xmm_nr` | Return the CE-local memory address of an XMM register (0-15) for the currently broken thread. |
| `debug_is_debugging` | — | Check whether the CE debugger has been started. |
| `delete_all_registered_symbols` | — | Delete every user-registered symbol (both AA and Lua). |
| `delete_structure` | `structure_id` | Delete a CE structure from the global list and free it. |
| `disassemble` | `address`, `count`, `offset`, `limit` | Disassemble instructions starting at an address. |
| `dissect_structure` | `address`, `size` | Use CE's auto-guess feature to interpret memory at address as a structure. |
| `enum_memory_regions_full` | `offset`, `limit`, `max` | Enumerate ALL memory regions in the process (Native EnumMemoryRegions). |
| `enum_modules` | `offset`, `limit` | List all loaded modules (DLLs) with their base addresses and sizes. |
| `enum_registered_symbols` | — | List all user-registered symbols. |
| `export_structure_to_xml` | `structure_id` | Export a CE structure definition as XML. |
| `file_exists` | `filename` | Check whether a file exists at the given path. Returns {success, exists: bool}. |
| `find_call_references` | `function_address`, `offset`, `limit` | Find all locations that CALL this function. |
| `find_function_boundaries` | `address`, `max_search` | Attempt to find the start and end of a function containing the address. |
| `find_references` | `address`, `offset`, `limit` | Find instructions that access (reference) this address. |
| `find_window` | `title`, `class_name` | Find a top-level window by title and/or class name (system-wide, no process required). |
| `generate_api_hook_script` | `address`, `target_address`, `code_to_execute` | Generate an Auto Assembler script that hooks a function and redirects it. |
| `generate_code_injection_script` | `address` | Generate a boilerplate code-injection Auto Assembler script for an address. |
| `generate_signature` | `address` | Generate a unique AOB signature that can find this specific address again. |
| `get_address_info` | `address`, `include_modules`, `include_symbols`, `include_sections` | Get symbolic name and module info for an address (Reverse of get_symbol_address). |
| `get_address_list` | `offset`, `limit` | List memory records in the current cheat table's address list. |
| `get_breakpoint_hits` | `id`, `clear`, `offset`, `limit` | Get hits for a specific breakpoint ID (or all if None). Set clear=True to flush buffer. |
| `get_directory_list` | `path` | List subdirectories in the given directory path. Returns {success, count, directories: [str]}. |
| `get_file_list` | `path` | List files in the given directory path. Returns {success, count, files: [str]}. |
| `get_file_version` | `filename` | Get the version info of a file (major, minor, release, build). Returns {success, major, minor, release, build, version_string}. |
| `get_foreground_process` | — | Get the PID and window handle of the process currently in the foreground. |
| `get_global_variable` | `name` | Read a global variable from CE's main Lua state. |
| `get_instruction_info` | `address` | Get detailed info about a single instruction (size, bytes, opcode). |
| `get_memory_protection` | `address` | Query the protection flags of a memory page in the target process. |
| `get_memory_record` | `id`, `description` | Retrieve a single memory record by ID or description. |
| `get_memory_record_value` | `id` | Read the current value of a memory record as a string. |
| `get_memory_regions` | `max` | Get list of valid memory regions nearby common bases. |
| `get_module_size` | `module_name` | Get the in-memory size of a loaded module. |
| `get_mouse_pos` | — | Get the current mouse cursor position. Returns x and y screen coordinates. |
| `get_opened_process_handle` | — | Get the OS handle of the process currently attached to Cheat Engine as a hex string. |
| `get_opened_process_id` | — | Get the PID of the process currently attached to Cheat Engine. |
| `get_physical_address` | `address` | Translate Virtual Address to Physical Address (requires DBVM). |
| `get_physical_address_cr3` | `cr3`, `virtual_address` | Translate a virtual address to its physical address using an explicit CR3. |
| `get_pixel` | `x`, `y` | Get the colour of a screen pixel at (x, y). Returns r, g, b channels and the raw COLORREF integer. |
| `get_process_info` | — | Get current process ID, name, modules count and architecture. |
| `get_process_list` | — | Get the list of running processes on the system. |
| `get_processid_from_name` | `name` | Look up the PID of a process by its executable name. |
| `get_rtti_classname` | `address` | Try to identify the class name of an object at address using Run-Time Type Information. |
| `get_scan_results` | `offset`, `limit`, `max` | Get results from the last 'scan_all' operation. |
| `get_screen_info` | — | Get the primary screen dimensions and DPI. Returns width, height (pixels) and dpi. |
| `get_structure_by_name` | `name` | Find a CE structure by name in the global structure list. |
| `get_structure_elements` | `structure_id` | Get all elements of a CE structure. |
| `get_symbol_address` | `symbol` | Resolve a symbol name (e.g., 'Engine.GameEngine') to an address. |
| `get_symbol_info` | `name` | Retrieve detailed information about a known symbol. |
| `get_temp_folder` | — | Return the path to the system temp folder. Returns {success, path: str}. |
| `get_thread_list` | `offset`, `limit` | Get list of threads in the attached process. |
| `get_window_caption` | `handle` | Return the caption (title bar text) of a window given its handle (hex string). |
| `get_window_class_name` | `handle` | Return the window class name of a window given its handle (hex string). |
| `get_window_process_id` | `handle` | Return the process ID that owns a window given its handle (hex string). |
| `in_main_thread` | — | Check whether the current code is running in CE's main thread. |
| `is_key_pressed` | `vk` | Check whether a key is currently held down. |
| `list_breakpoints` | — | List all active breakpoints. |
| `load_new_symbols` | — | Scan for newly loaded modules and import their symbols. |
| `md5_file` | `filename` | Calculate the MD5 hash of a file on the CE host. Filename must not contain '..' components. |
| `md5_memory` | `address`, `size` | Calculate the MD5 hash of a memory region. Returns the hash as a hex string. |
| `next_scan` | `value`, `scan_type`, `value2` | Next scan to filter results from the last 'scan_all'. |
| `persistent_scan_destroy` | `name` | Destroy a named persistent scan session and free its memory. |
| `persistent_scan_first_scan` | `name`, `value`, `type`, `scan_option`, `value2` | Run the first scan on a named persistent scan session. |
| `persistent_scan_get_results` | `name`, `offset`, `limit` | Get paginated results from a named persistent scan session. |
| `persistent_scan_next_scan` | `name`, `value`, `scan_option`, `value2` | Narrow down results with a next scan on a named persistent scan session. |
| `pointer_rescan` | `value`, `previous_results_file` | Re-scan an existing pointer scan for a new value. Requires a prior pointer scan in CE. |
| `poll_dbvm_watch` | `address`, `max_results` | Poll DBVM watch logs WITHOUT stopping. Returns register state at each execution hit. |
| `read_clipboard` | — | Read text from the system clipboard. Returns {success, text: str}. |
| `read_integer` | `address`, `type` | Read a number from memory. Types: byte, word, dword, qword, float, double. |
| `read_memory` | `address`, `size` | Read raw bytes from memory. |
| `read_pointer` | `address`, `offsets` | Read a pointer chain. Returns the final address and value. |
| `read_pointer_chain` | `base`, `offsets` | Follow a multi-level pointer chain and return analysis of every step. |
| `read_process_memory_cr3` | `cr3`, `address`, `size` | Read virtual memory using an explicit CR3 page-table base via DBK/DBVM. |
| `read_string` | `address`, `max_length`, `wide`, `encoding` | Read a string from memory. |
| `register_symbol` | `name`, `address`, `do_not_save` | Register a user-defined symbol with a given name and address. |
| `reinitialize_symbol_handler` | — | Perform a full reset and reload of the Cheat Engine symbol handler. |
| `scan_all` | `value`, `type`, `protection`, `scan_option`, `value2` | Unified Memory Scanner (first scan). |
| `search_string` | `string`, `wide`, `limit` | Quickly search for a text string in memory. |
| `unregister_symbol` | `name` | Remove a previously registered user-defined symbol. |
| `validate_pointer_chains` | `chains`, `target`, `include_misses` | Resolve a list of candidate pointer chains and report which currently land on `target`. Each chain is {"base": <addr/symbol>, "offsets": [int,...]}. Returns only matches by default (token-frugal); set include_misses=true to also list non-matches. Re-run after a game restart with the new target to find chains that stay valid (the stable pointer). Max 5000 chains per call. |

### observe — attach and cosmetic (7)

| tool | arguments | first line of its description |
|---|---|---|
| `beep` | — | Play a simple system beep sound. |
| `open_process` | `process_id_or_name` | Open a process by PID or name and attach Cheat Engine to it. |
| `ping` | — | Check connectivity and get version info. |
| `play_sound` | `filename` | Play a WAV sound file by filename. Path must not contain '..' directory traversal. |
| `set_progress_state` | `state` | Set the Cheat Engine taskbar progress state. Valid states: none, normal, paused, error, indeterminate. |
| `set_progress_value` | `current`, `max` | Set the Cheat Engine taskbar progress bar position. Provide current value and maximum value. |
| `speak_text` | `text`, `english_only` | Speak text via Windows SAPI text-to-speech. Set english_only=True to force the English voice. |

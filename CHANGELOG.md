# Changelog

All notable changes to Cortex are documented in this file.

## [v1.0.1] - 2026-10-16

Security release. It also publishes the agent test layer that was merged into `main` after v1.0.0 (launch profiles, bounded input trials, a persistent knowledge graph; pull requests #17 to #39).

**Breaking for MCP clients that write.** A client that changes a target (memory writes, breakpoints, patches, scripts, native calls, prompts) must now be started with `cortex.exe mcp --allow-writes`. A client that only saves Cortex's own records (project notes, addresses, pointer paths, knowledge, session facts, the `cortex_test_*` journals, catalog and reports) needs `--allow-state`, which cannot change the target; `--allow-writes` includes it. Clients that only observe and analyze are unaffected. See `README.md` and `docs/mcp.md` for the three configurations.

### Security

The distributed v1.0.0 is affected: through `mutation_permission` (a boolean the client wrote itself), `batch_run`, `cortex_attach` and `cortex/private/route`, an AI client could change a target, or load the runtime into it, without the person having allowed writes. This was found by auditing the MCP path and reproduced against the real `cortex.exe mcp`. It is fixed as follows.

- **Write authority now comes from the person, not the model.** `cortex.exe mcp` (and the `cortex_host mcp` bridge) take `--allow-writes`; without it every call that changes the target is refused with `write_authority_required`. The flag reaches the runtime on the authenticated pipe envelope, never inside a tool call. `mutation_permission=true` on the call is still required, as a declaration of intent, and no longer sufficient. The desktop supplies its own authority from the Read-only / Writes allowed switch. **This is a behaviour change:** an MCP client that wrote before must now be started with `--allow-writes`.
- **A narrower authority for Cortex's own records: `--allow-state`.** Saving project notes, addresses, pointer paths, knowledge and facts (`project_note_add`, `project_knowledge_put`, `re_session_fact_set`, ...) and the test journals, catalog and reports (`cortex_test_report`, `cortex_test_step_*`, `cortex_test_catalog_save`, ...) is a new risk class, `state`. It needs `mutation_permission` and either `--allow-state` or `--allow-writes`, and is refused with `state_authority_required` otherwise. It cannot change the target: `--allow-state` alone still refuses every write to the target. Without it, an agent cannot plant records for a later session to read. A client that saved knowledge before must now be started with `--allow-state`.
- **`batch_run` no longer bypasses the permission.** It was classed as an analysis although its `ops` write memory, freeze values and patch code. It is now classified from its operations against an allowlist, an unknown or computed operation is refused, and the same classification is re-checked after `$from_step` references are resolved so a reference cannot make an authorized step riskier.
- **`cortex_attach` (and `--pid` / `--process`) no longer inject without authority.** Loading the runtime puts code in the target; without `--allow-writes` the server connects only to a runtime that is already there (for example loaded with `cortex.exe inject <pid>`).
- **The private desktop channel is closed to MCP clients.** A raw `cortex/private/route` message sent by an MCP client was forwarded to the runtime, which ran any native route with no check. The server now forwards only `initialize`, `ping`, `server/discover`, `tools/list`, `tools/call` and `notifications/*`, and the runtime accepts a private route only from the desktop's own adapter.
- **`scan_new` / `scan_next` with `pause_process`, and all `prompt_*` tools, are now control operations** that need authority.
- **A tool that no rule classifies is refused.** Every listed tool now needs an explicit risk class; a tool with none is `unclassified` and is refused with `tool_not_classified` whatever authority the caller holds (before, an unknown name defaulted to `analyze`). `tests/fixtures/mcp_tool_risks.json` records the class of every primitive tool and a test fails if one changes or is missing.
- **Host tools added after v1.0.0 are classified.** Every `cortex_launch`, `cortex_test_*` and `cortex_knowledge_*` tool is listed in `tools/list` as `read_only` or as `host_control`; the ones that send input, persist records or stop a trial also name an `effect` (`target_input`, `cortex_state` or `trial`). The regression test fails on a tool without a label, or with the wrong one. Launch, restart, stop, attach and input stay bounded by the operator-owned launch profile, and a launch profile that attaches the runtime grants no write authority.
- **Debugger attach and detach are journaled.** The `debug_*` read tools stay available without write authority, because they do not write memory, but a debugger attached to a process suspends its threads. Each attach/detach is shown in the AI Activity tab and appended to `cortex_debugger_events.jsonl`.
- **Refused calls are logged** to `cortex_denied_mutations.jsonl` (tool, arguments, UTC time, `runtime` or `host`), beside `cortex_core.dll` and `cortex.exe`, so attempts can be counted.
- **Regression test.** `tests/security_no_writes_regression.py` starts the test target, loads the runtime beforehand, then calls every tool `tools/list` returns (251 on the `--tools all` profile), with plausible arguments and `mutation_permission=true`, against a server started without `--allow-writes`, and requires the target's code and data hash to be unchanged. Controls prove the hash does change when writes are allowed, and that `--allow-state` saves a note but still cannot write the target. It runs on x64 and x86 in CI.
- **Known limit, documented:** the pipe token is a file readable by the same Windows user. A client that also has a shell or file access can read it and bypass these controls. An authority granted by the desktop for a limited time is planned.
- **Left as is, on purpose:** `cortex_operation_cancel` and `cortex_test_cancel` need no authority (they only cancel something already authorized), and the `debug_*` read tools stay outside the control class: they write no memory, but a debugger attached to a process suspends its threads, which is why attach and detach are journaled.

### Added

- **Launching and testing programs.** The operator lists the programs an agent may start in a launch profile file (`cortex.exe mcp --launch-config <file>`); the agent selects a profile by name and cannot supply its own executable, arguments or working directory. `cortex_launch_status`, `cortex_launch`, `cortex_restart` and `cortex_stop` start, restart and stop the program, with run limits, one running child per profile, and optional attach of the runtime. See `docs/automated-tests.md`.
- **Bounded input trials.** `cortex_test_run` posts allowlisted input to the launched program, reads values before and after, evaluates expectations (`increased`, `unchanged`, ...), waits on conditions, and writes JSON and Markdown reports under `--test-results`. `cortex_test_get`, `cortex_test_list` and `cortex_test_cancel` read and stop trials; `cortex_test_compare` compares repeatable trials. Input never uses `SendInput`; only the allowlisted program receives window messages.
- **Persistent typed knowledge graph.** `project_knowledge_put`, `project_knowledge_get`, `project_knowledge_list`, `project_knowledge_query`, `project_knowledge_verify` and `project_knowledge_graph` keep claims, revisions and verifications in the project; `cortex_knowledge_graph` and `cortex_knowledge_code_check` read them and revalidate code references after a restart or a new generation of the program.
- **Evidence and code observations.** `cortex_test_link` links a recorded trial to a knowledge record; `cortex_test_code_context`, `cortex_test_code_candidates` and `cortex_test_code_link` resolve the module and symbol of code observed during a trial (x64 PE unwind ranges included) and retain the location with the binary hash. `cortex_test_evidence_graph` shows the evidence behind a claim.
- **Investigations.** Predictions are evaluated against what a trial observed; `cortex_test_choose` picks a discriminating next experiment; `cortex_test_step_prepare`, `cortex_test_step_continue`, `cortex_test_step_record`, `cortex_test_step_next` and `cortex_test_step_get` run it as a supervised step that survives a reconnection; `cortex_test_generate` and `cortex_test_generate_prepare` propose bounded experiments (experimental). `cortex_test_report`, `cortex_test_report_get`, `cortex_test_investigation_report`, `cortex_test_investigation_summary`, `cortex_test_catalog_list` and `cortex_test_catalog_save` keep the reports and a catalog of tested scenarios.
- **`cortex_agent_guide`** returns the built-in guidance for an agent using these tools; `AGENTS.md`, `agent/testing.md` and `agent/investigation-template.md` document the workflow.
- 37 tools in total (31 host tools and the 6 `project_knowledge_*` primitives): the `--tools all` profile now lists 251 tools and the compact profile 72.

### Changed

- **Semantic tool results no longer carry `confidence`, `evidence_confidence`, `candidates`, `alternative_hypotheses` or `tested_hypotheses`.** Cortex never computed them (`confidence` was a constant 1.0), and a figure that looks like a measure misleads the agent reading it. `status` is now documented as what it is: `plan_ready`, `completed`, `failed`, `cancelled` or `timed_out`.
- The documentation and README no longer say that the application enforces writes for every call where that was only true of the desktop.

### Fixed

- **`debug_registers` froze the target.** `ReadTargetThreadRegisters` suspended a thread, read its context, then closed the thread handle before the guard that resumes it ran; `ResumeThread` on a closed handle fails, so the thread stayed suspended. Found by the new regression test; fixed by resuming explicitly before closing the handle.
- **`cortex_test_run` was listed as `read_only`.** `tools/list` described the tool that sends keys and mouse clicks to the launched program as read-only. It is now `host_control` with `effect: target_input` and requires `mutation_permission`, like the other host tools that act. Only the label was wrong: the permission and the operator-owned launch profile were already enforced when the tool is called. The regression test now checks the label of five representative host tools.

### Tests and CI

- The five Windows workflows `imgui-gui-tests`, `contracts`, `imgui-ui`, `unified-host` and `e2e-windows` run on every pull request and on pushes to `main`, with no path filter, so a change outside the usual paths cannot skip them.
- `tests/fixtures/mcp_tool_risks.json` freezes the risk class of every one of the 179 primitive tools; `tests/mcp_authority_tests.cpp` covers classification, the three authority levels and the denial log.
- `tests/test_runner_e2e.py` arms hardware-write log breakpoints, so its main client is started with `--allow-writes`; a first client with `--allow-state` only asserts that a launch-profile attach cannot arm one.
- `publish-release.yml` publishes when the commit message contains `Release v1.0.1`, so a merge commit whose pull request is titled that way publishes as well as a direct commit.

## [v1.0.0] - 2026-09-30

First stable release. It is the Dear ImGui desktop that was prepared as the unpublished 0.8 candidate, validated against a real game (AssaultCube) in addition to the automated suites. Versions 0.7.0 and 0.8.0 were not published; their changes are all below.

### Native desktop application

- Replaced the Qt 6/QML desktop with a native Dear ImGui (docking) application on Win32 + Direct3D 11, built from `app_imgui/`. `cortex.exe` no longer ships Qt runtime files. The Qt/QML sources in `app/` stay frozen as the parity reference and are no longer built or released.
- Every Qt workspace and all 148 Qt controller workflows have an ImGui counterpart (`app_imgui/PARITY_MATRIX.md`, `app_imgui/INVOKABLE_PARITY.md`).
- Dockable workspaces arranged by six presets (Memory, Debug, RE, Trace, Automation, Runtime), a two-row header with the target, the Read-only / Writes allowed toggle and debugger controls, command palette, Go To, navigation history, and a bottom panel whose tabs hide while the same content is open as a full workspace.
- TrueType fonts (Segoe UI, Cascadia Mono / Consolas for data) and per-monitor DPI scaling, including `WM_DPICHANGED`.
- Long runtime operations run on a background worker with a progress card, so the window keeps redrawing.
- Settings and the dock layout are stored in `%LOCALAPPDATA%\Cortex`; a `cortex.portable` file beside `cortex.exe` keeps them next to the executable. Existing files beside the executable are copied over on first start.

### Memory tools, address list and assembler (Cheat Engine parity)

- **Value scanner** rebuilt to Cheat Engine's level: byte / 2 / 4 / 8 bytes, float, double, string (with UTF-16 and case options), array of bytes with wildcards, and All numeric. Exact / bigger / smaller / between / unknown-initial first scans; next scans for increased, decreased, changed, unchanged, same-as-first and *by* deltas; hex, unsigned, not, compare-to-first, float rounding. Memory-scan options with writable / executable / copy-on-write tristates, MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED region types, fast scan and alignment, an address range or a module, optional pause while scanning, worker threads, and a result cap. Results colour changed values, show previous / first / module+offset, and support multi-select add / save / remove / copy.
- **Address list** with Cheat Engine's features: freeze (always / allow increase / allow decrease) at a configurable interval, static `module+offset` and multi-level pointer entries, and full **Cheat Engine address expressions** (`[[game.exe+10]+20]+8`, `"name"+offset`, `+ - *`). Group headers that collapse, drag-and-drop reordering and "Group selected", description colours, dropdown value lists, "Change address", multi-entry "Change value", and Enter / Space / Delete / Ctrl+A/C/V keys. The clipboard uses Cheat Engine's own table format, so entries copy and paste both ways. A draggable splitter shares the height between the scanner and the list, which now survives a target change.
- **Per-entry hotkeys**: system-wide shortcuts on a single entry or a whole group — toggle freeze, freeze, unfreeze, set value, increase / decrease by — registered alongside the Settings hotkeys, with shared chords running every bound entry. Read from and written to `.CT` files.
- **Cheat tables (`.CT`)**: open and save Cheat Engine tables with groups, pointers, strings and byte arrays, description colours, collapsed groups, dropdown lists, per-entry hotkeys and user-defined symbols. Auto Assembler scripts are read and written as script entries (see below); a table's Lua script is offered to the Lua engine for review instead of running on its own.
- **Address expressions everywhere** (address list, Go To, memory viewer, memory tools): module names, module exports, forwarded exports, `module!export` and user-defined symbols, shared with the Lua engine and cheat tables. Memory tools gains a **Symbols** tab to register symbols and search every module's exports.
- **Memory tools** panels: memory regions, PE headers (sections, directories, exports with forwarders, imports), string search, code caves, AOB signature generation (Zydis), and a **pointer scanner** with rescan, save / load and "add path to the list".
- **Memory viewer** rewritten: a 64 KB window with display types, changed bytes in red, in-place hex typing, a data inspector, and text / byte find.
- **What accesses / writes**: hardware data and execute breakpoints in log mode, decoding the accessing instruction and grouping instruction watches by effective address with a live value.
- **Lua engine** (Cheat Engine compatible, no runtime injected): read*/write*, `getAddress(Safe)`, `AOBScan(Unique/ModuleUnique)`, `enumModules`, `getModuleSize`, pause / unpause, byte-table conversions, `registerSymbol`, `getNameFromAddress`, `inModule`, `inSystemModule` and more, sandboxed with a time limit and cancel. Writes and pausing require Writes allowed.
- **Assembler and Auto Assembler**: an x86/x64 text assembler on the Zydis encoder — Intel syntax, hex-by-default numbers (`#decimal`, `(float)x`), labels and forward references, auto-short jumps, `db/dw/dd/dq/nop` data, symbol and label resolution, and a far `jmp`/`call` fallback. A Memory tools **Assembler** tab assembles and writes bytes, or performs a **code injection** trampoline: a cave is allocated near the site, the new code runs, the replaced instructions are relocated (relative branches and RIP-relative operands keep their targets) and control returns after them, with a Restore. Global hotkeys, process pause / resume, auto-attach and a system-wide attach-foreground shortcut round out the parity.

- **Auto Assembler scripts** run: a Cheat Engine script interpreter with `[ENABLE]` / `[DISABLE]`, `alloc` / `globalalloc` / `dealloc`, `label` / `registersymbol` / `unregistersymbol`, `define`, `aobscan` / `aobscanmodule`, `assert`, `fullaccess` and `createthread`, resolved over several passes. Script entries live in the address list beside the values, with a checkbox that runs `[ENABLE]` and `[DISABLE]`, and are read from and written to `.CT` files.
- **Speedhack**: the target's timing functions are hooked so the clock they report is scaled. Each hook calls the real function through a trampoline and rescales around the value seen on the first call, so time never jumps backwards when the multiplier changes.
- **Grouped scan**: a pattern of values that sit close together (`4:64 f:1.5 2:14`), with per-element types, a bare `*` that skips one byte and `4:*` for a field whose value is unknown. Hits carry the offset of every element.
- **Memory dumps**: a range of memory is written to a file and a file is put back where it came from, with unreadable pages saved as zeros and counted.
- **User-defined value types**: a named type reads its own width, takes its bits and shows `raw * scale + offset`, which covers a big-endian field, a scaled value or a bitfield. Types are saved to a file, picked from an address list entry's Change type, and stored in a `.CT` as Cheat Engine's `Custom`.
- **Dissect data structures**: the same bytes are read at one or more instances and each offset is described — a pointer, a float, some text, a number — with the fields that disagree between instances highlighted.
- **Pointer spider**: every plausible pointer out of one address is followed level by level, the opposite of the pointer scanner; a path goes to the address list as a pointer entry.
- The Memory tools are picked from one combo grouped by Inspect / Search / Code / Names / Files, and are also under Tools > Memory tools, instead of a row of buttons that grew to two lines. The header is one line: the Workspace preset buttons are gone (the Workspace menu has them) and Go to, Pause target, the debugger controls and the session chips sit beside the target.
- The pointer spider has a graph view — a column per level, one node per object, edges labelled with their offset, pan and zoom — beside the list.

### Fixes

- **What accesses / writes no longer kills or freezes the game.** The Windows debugger cleared DR6 while changing the debug registers, so a hit already on its way reached the handler with no slot and the single-step exception was passed to the game, which died; that happened on Stop and on adding a second watch. A trap on a slot with no breakpoint is now disarmed and swallowed, DR6 is preserved, and any single-step is swallowed while a hardware breakpoint exists. Every hit also copied the breakpoint's whole log and read and wrote the thread context six times while the game stood still; it is now one read and one write. The list no longer re-counts the last entry, sorts every frame or reads memory per row per frame, shows hits per second, and a watch stops itself after 20000 hits (editable) so a busy address cannot freeze the game. The injected debugger no longer walks the stack with dbghelp on every hardware hit and drops a trap raised inside its own handler.
- **Pause target no longer pauses Cortex.** The runtime answers from a thread inside the target, so with every thread suspended each call waited out its timeout and froze the window. While paused, runtime calls fail at once and the debugger refuses to attach; the connection is kept, so Resume needs no reconnect.

- The write permission is enforced for every mutating application call, not only when the runtime is first loaded: an already connected runtime no longer receives `mutation_permission` while the UI is read-only.
- The Debugger reads registers and stack before the debugger is attached.
- A newly tracked RE object shows its resolved address and liveness immediately.
- The desktop finds the runtime token when the runtime was injected from the application root instead of `runtime/<arch>`.
- Value scans report when the result limit was reached.
- `cortex inject <pid>` works on 32-bit targets: it reads the target's architecture, uses `runtime\x86\cortex_core.dll` through the private x86 helper, and refuses a DLL of the wrong architecture with a clear message instead of failing inside the target. Without a DLL path it no longer looks in the current directory.
- Every injection path loads `dbghelp.dll`, `opengl32.dll` (and `d3d8.dll` for 32-bit targets) from System32 before the runtime when the target has not loaded them, so a copy of the wrong architecture or a proxy DLL in the game's folder can no longer break or hijack the runtime's imports.
- The portable bundle carries one runtime per architecture, in `runtime/x64` and `runtime/x86`; the extra x64 copy at the bundle root is gone.

### MCP

- The tool manifest now lists `debug_breakpoint_trigger_set`, `debug_breakpoint_trigger_clear` and `symbols_module`.
- MCP server metadata reports version `1.0.0`.

### Build and validation

- `app_imgui/main.cpp` is split by concern (application state, UI, CLI, GUI test modes, window loop); the GUI test modes can be left out with `-DCORTEX_IMGUI_TEST_MODES=OFF`.
- Command-line tools expose named entry points instead of renaming `main()` through compile definitions; standalone builds define `CORTEX_STANDALONE_TOOL`. The desktop debugger backends moved to `host/debugger/` and the Windows icon to `resources/windows/`.
- Application models share `RuntimeModelBase` over a `RuntimeTransport` interface and are unit-tested against a scripted transport (`cortex_app_models_tests`).
- The integrated E2E validates `/schema/validate` and calls every read-only GET tool of the manifest, failing on any 5xx.
- The P1-P4, prompt-contract and diagnostics M1-M7 workflows are grouped as jobs of `.github/workflows/contracts.yml`.

## [v0.7.0] - 2026-09-21 (not published)

This Qt/QML release was prepared but never published; its changes ship as part of v1.0.0, which replaces the Qt desktop with the native Dear ImGui application.

### Unified desktop application

- Replaced the historical multi-binary user workflow with one Qt 6/QML desktop application, `cortex.exe`, while keeping x64/x86 runtime payloads as internal portable-bundle assets.
- Added target discovery and attach/detach sessions, persistent Addresses and Project workspaces, Memory, Scanner, Disassembly, Structures, Modules, Symbols, Snapshots, Debugger, Breakpoints, Traces, Patches, Watches, Hooks, Network, Screenshots, Diagnostics, Scripts, Input, Actions, MCP and RE workspaces.
- Added shared address navigation, global Go To, command palette, live address values, context-menu workflows and explicit Mutation permission for state-changing operations.

### Runtime, debugger and reverse engineering

- Added the generic local target/session backend and service layer used by both the desktop UI and native MCP execution.
- Expanded debugger control with Pause, Continue, Step Into, Step Over, software/hardware breakpoints, process-global hardware-watch propagation and mixed-bitness validation.
- Added runtime RE tooling for C++ subobject/vtable discovery, tracked objects and field changes, cross-session diffs, last-writer discovery, transition tracing, checkpoints, reversible experiments and persistent RE facts.
- Added Ghidra symbol import, improved project evidence persistence, typed structure workflows and rollback-aware runtime experiments.

### MCP and automation

- Added persistent targetless MCP mode with process discovery, dynamic attach/detach, multi-target routing and tool-list change notifications.
- Kept compact semantic tools as the default while preserving the complete primitive catalog under `--tools all`.
- Added UI/MCP integration for runtime sessions and maintained the modern `2026-07-28` plus legacy MCP protocol compatibility.

### Validation and packaging

- Added portable Windows x64/x86 staging, Qt deployment/dependency closure checks, GUI smoke tests, mixed-bitness integrated MCP E2E coverage and AssaultCube offline E2E coverage.
- Added Linux application build/smoke validation for the Qt frontend.
- Fixed hardware-breakpoint readiness accounting so synchronous API/MCP callers cannot block their own process-global watchpoint coverage gate.
- The v0.7.0 release packages one Windows portable application archive with `cortex.exe`, Qt runtime files and private `runtime/x64` + `runtime/x86` instrumentation assets.

## [v0.6.0] - 2026-08-28

### Native MCP transport and shared executor

- Made `cortex_host mcp` use stdio -> authenticated local Windows Named Pipe transport by default, while retaining `--transport http` as an explicit compatibility and debugging fallback.
- Added a shared in-process MCP executor and native route registry so primitive and semantic MCP calls reuse the same business handlers without the previous MCP -> HTTP -> route loopback path.
- Added one-command startup through `cortex_host mcp --process <name-or-pid>` / `--pid`, with optional `--dll`, `--token-file`, `--tools compact|all`, and transport selection.
- Made the compact MCP profile the default and limited it to the 30 domain-neutral semantic tools; `--tools all` exposes the generated primitive surface when direct low-level access is required.
- Added token-derived local pipe rendezvous, full-token authentication, constant-time token comparison, bounded framing, local-client restrictions where supported by Windows, and bounded bridge concurrency.

### MCP protocol and semantic execution

- Added support for the stateless MCP `2026-07-28` protocol while preserving legacy initialize-based compatibility with `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05` clients.
- Added `server/discover`, modern tool-list TTL/cache hints, no-response notification handling, notification filtering in batches, and session-scoped cancellation delivery.
- Enabled bounded server-side semantic `execute=true` orchestration with an explicit non-empty `steps` sequence limited to 32 primitive calls.
- Added cooperative execution deadlines, per-step evidence capture, inter-step JSON-pointer references, lifecycle reporting, and stable validation errors.
- Required `mutation_permission=true` before control, mutation, or native-call primitives can execute, and reject active operations that do not expose a known rollback contract.
- Run supported mutations inside action transactions with rollback on failure, observed cancellation, or observed timeout; `rollback_on_success=true` is now part of the public semantic schema for reversible causal experiments.
- Updated MCP server metadata to report version `0.6.0`.

### Native transport reliability and compatibility

- Fixed Win32 `GetLastError()` calls in the Named Pipe server being shadowed by Cortex's own string-returning `GetLastError()` helper, restoring x86/x64 compilation.
- Fixed the stdio bridge consuming a one-shot Named Pipe connection only to probe readiness, which could make the first real `initialize` request fail with `cortex_unreachable`; the first real request now performs bounded connection retries instead.
- Kept existing REST callers and HTTP `POST /mcp` callers supported while moving the recommended MCP path to the native transport.
- Refreshed `cortex_host` help, the packaged French installation guide, README MCP examples, semantic-agent documentation, and release metadata for the native transport and execution model.

### Validation and release gate

- Added x86/x64 MCP protocol contract tests covering legacy negotiation, modern discovery/list behavior, notifications, batching, and tool calls.
- Added native pipe rendezvous/framing tests, semantic execution contract tests, bridge-policy tests, and stronger schema validation including `rollback_on_success`.
- Added real injected HTTP semantic MCP execution plus real `cortex_host mcp` stdio -> Named Pipe -> semantic executor -> native route dispatcher E2E coverage.
- The v0.6.0 release gate builds both Windows architectures, runs CTest and semantic contracts, injects the real runtime, validates both HTTP and native MCP transports, verifies package contents, and only then publishes the x86/x64 archives.
- Release archives continue to contain `cortex_host.exe`, `cortex_core.dll`, byte-identical `cortex.asi`, standalone `injector.exe`, the architecture-matched test target, documentation, SDK files, and agent documentation.

## [v0.5.0] - 2026-08-14

### Security, API reliability, and mutation safety

- Added nested action transactions, explicit mutation checkpoints, automatic rollback guards, and rollback verification foundations for controlled experiments.
- Hardened the local REST API with bounded payload handling, request IDs, stable JSON error helpers, pagination primitives, and stricter request validation.
- Kept request correlation in `X-Cortex-Request-Id` while fixing a response truncation regression caused by mutating JSON bodies after `Content-Length` had already been calculated.
- Added checked memory-range arithmetic and validation to reduce overflow and invalid-range risks around low-level memory operations.
- Hardened Lua execution with sandbox restrictions, resource limits, cancellation-aware execution foundations, and mutation journaling support.

### MCP and semantic contracts

- Added typed MCP input schemas derived from the HTTP tool contract instead of exposing loosely typed argument objects.
- Added safe percent-encoded path and query rendering, required `_query` containers when required query fields exist, and validation for unresolved path placeholders.
- Added local-only MCP bridge policy checks and stricter Host validation so the AI-facing bridge remains bound to authorized local Cortex endpoints.
- Added stable semantic `plan_id` generation, explicit plan lifecycle states, evidence confidence, evidence-state vocabulary, timeout/cancellation requirements, and rollback requirements for mutations.
- Server-side multi-step semantic execution remains intentionally disabled until cancellation, timeout, permission, and rollback semantics are enforced end to end.
- Updated MCP server metadata to report version `0.5.0`.

### Generic target architecture

- Added platform-neutral `Target`, `Node`, `Backend`, `Catalog`, architecture, and capability abstractions so Cortex is no longer structurally tied to a game-only target model.
- Added explicit capability sets and capability union/intersection/subset operations so tools can adapt to what a target actually supports instead of assuming every feature is available.
- Added Windows, Linux, and PS4 platform identities plus x86, x64, and ARM64 architecture identities to the shared model without leaking platform APIs into the common contract.
- Added a Windows-local descriptor adapter as the first concrete bridge from the generic target model to the existing Windows runtime.
- Added portable C++17 target-model validation on Linux plus Windows x86/x64 capability-model tests.

### Runtime tooling and validation

- Added the read-only `cortex_host probe --pid ...` command for external process inspection without requiring injection.
- Added a real OpenGL/WGL runtime fixture and renderer validation alongside the existing Windows runtime coverage.
- Added dedicated P1, P2, P3, and P4 CI workflows covering request contracts, mutation hardening, MCP schemas and bridge policy, runtime probing, renderer validation, and the generic target model.
- Revalidated the complete Windows tree on x86 and x64, including CTest, semantic MCP calls after real injection, unified-host checks, deterministic E2E scenarios, and release packaging.
- Release archives continue to ship both Windows x86 and x64 builds with `cortex_host.exe`, `cortex_core.dll`, `cortex.asi`, standalone `injector.exe`, the architecture-matched test target, documentation, SDK files, and agent documentation.

### Scope

- v0.5.0 introduces the generic cross-platform model and capability contracts, not full Linux or PS4 runtime instrumentation.
- Remote-control transport, Windows-wide process enumeration, Linux process instrumentation, PS4 process instrumentation, and dedicated D3D8/D3D12 runtime fixtures remain future work.

## [v0.4.0] - 2026-08-04

### Semantic tools for AI agents

- Added 30 domain-neutral semantic MCP tools for observation, memory discovery, pointer analysis, execution tracing, structure inference, candidate classification, causal validation, and reversible patching.
- Semantic tools start from observable runtime behaviour instead of assuming that a game contains concepts such as health, ammunition, money, or score.
- Added a shared evidence-oriented result contract with explicit status, confidence, evidence, candidates, rejected alternatives, tested hypotheses, next action, and reversible actions.
- Added deterministic orchestration plans that map each semantic goal onto the existing Cortex primitive tool catalog.
- Added `structuredContent` to MCP tool results while retaining text content for compatibility with existing clients.
- Updated MCP server metadata to report version `0.4.0`.
- Added `agent/semantic-tools.md` with the full catalog, agent rules, failure semantics, and validation guidance.
- Release archives now include the `agent` documentation directory.

### Automated validation

- Added standalone catalog and contract tests for all 30 semantic tools on Windows x86 and x64.
- Added dependency validation so every semantic step must resolve to a live primitive or another acyclic semantic tool that reaches a primitive.
- Added live MCP tests after real DLL injection for initialization, discovery, all 30 individual calls, batched calls, structured/text agreement, input validation, side-effect freedom, and primitive dispatch compatibility.
- Added semantic tests to both the full Windows build and the v0.4.0 release gate; a failing test prevents packaging or publication.
- Fixed unresolved `timeline_start`, `timeline_stop`, and `timeline_mark` recipe dependencies found by the new validation.

### Scope and safety

- v0.4.0 exposes deterministic semantic planning over the existing REST/MCP primitives. Long-running server-side execution is intentionally deferred until cancellation, timeout, persistence, and rollback semantics are implemented.
- Semantic tools must return `not_found` or `inconclusive` instead of inventing a result when evidence is insufficient.
- Controlled mutations are expected to use Cortex's action journal and rollback support.

## [v0.3.1] - 2026-08-02

### Release packaging compatibility

- Restored a standalone `injector.exe` in every Windows x86 and x64 release archive.
- Added a ready-to-use `cortex.asi` beside `cortex_core.dll`; both files are verified byte-identical during packaging.
- Restored the architecture-matched `cortex_test_target_x86.exe` or `cortex_test_target_x64.exe` demonstration program.
- Added `README_INSTALL.txt`, a French installation and troubleshooting tutorial covering the standalone injector, unified host, ASI loader, token usage, diagnostics, and common errors.
- Added pull-request packaging validation so every required file is built and checked before a release can be published.
- Kept `cortex_host.exe inject` as the primary unified workflow while preserving the v0.2.0-compatible injector usage.

## [v0.3.0] - 2026-08-02

### Unified Windows tooling

- Consolidated the user-facing command-line tools into a single `cortex_host.exe`.
- Added the `serve`, `inject`, `diagnose`, `analyze`, `symbolize`, and `mcp` subcommands.
- Preserved compatibility with the historical `cortex_host.exe --pid ...` syntax.
- Stopped producing separate `injector.exe`, `cortex_mcp_bridge.exe`, `cortex_diag_host.exe`, and `cortex_symbolize.exe` release tools.
- Added a lightweight standalone CMake build for the unified host.

### Mod diagnostics SDK

- Added stable C and header-only C++ diagnostics APIs under `sdk/include/cortex`.
- Added dynamic runtime loading so mods do not require a Cortex import library.
- Added explicit mod registration with name, version, author, Git commit, build ID, source root, and symbol path metadata.
- Added automatic module path, image base, image size, and local mod DLL discovery.
- Added per-thread nested diagnostic scopes, RAII helpers, breadcrumbs, typed values, and named heartbeats.
- Added a documented MinHook mod example.
- Added `mods.json`, `scopes.json`, and `values.json` crash artifacts.

### Crash reports and symbolization

- Added PE build identity inspection, CodeView RSDS parsing, and PDB GUID/age verification.
- Added trusted function, source-file, line-number, and displacement resolution through DbgHelp.
- Added x86 and x64 stack walking with `StackWalk64`.
- Added MinGW/DWARF fallback through `llvm-symbolizer` and `addr2line`.
- Added source-root remapping for symbols built on another machine.
- Added `stack.json`, `build_info.json`, and a readable `report.txt`.
- Preserved module-plus-RVA fallback when exact symbols are unavailable.
- Expanded the symbols REST API with detailed module and address resolution.

### Hook diagnostics

- Added a stable hook registration API for target, detour, trampoline, owner, and hook library metadata.
- Added original, expected, installed, and current byte tracking.
- Added periodic hook integrity verification and tamper detection.
- Added overlap and conflict detection between registered hooks.
- Added invalid detour and trampoline detection.
- Added call counts, active-call counts, maximum concurrency, recursion depth, and hook exception tracking.
- Added `hooks.json` crash output and SDK RAII helpers.

### External crash and hang diagnostics

- Added a versioned local shared-memory and event protocol between the injected agent and the external host.
- Added out-of-process crash minidumps using the captured exception context.
- Added external hang minidumps without automatically terminating the target.
- Added named heartbeat monitoring, unresponsive-window checks, and process-liveness checks.
- Added safe per-thread register capture that never suspends the host capture thread itself.
- Added bitness validation so incompatible CPU contexts are never interpreted.
- Added `threads.json`, `hang_report.json`, and watchdog logs.

### Evidence-based analysis

- Added local, deterministic analysis of crash and hang artifacts.
- Added findings for near-null dereferences, stack overflow, hook replacement, overlapping hooks, invalid trampolines, excessive recursion, unloaded code, symbol mismatches, and insufficient evidence.
- Added confidence levels, supporting evidence, and actionable suggestions.
- Added `analysis.json` and `analysis.txt` output.

### Automated near-real Windows testing

- Added a deterministic Windows E2E matrix for x86 and x64.
- Added full root CMake/MinGW builds before every E2E run.
- Added real DLL injection through `cortex_host.exe inject`.
- Added a controlled Win32 target with exported memory values, command events, crash mode, hang mode, and machine-readable manifests.
- Added a real injected fake mod using the public diagnostics SDK.
- Added API authentication, memory read/write, batch operations, freeze, scan, Lua, MCP, and session export tests.
- Added a real unhandled access-violation scenario with internal and external dump validation.
- Added a real watchdog hang scenario with thread and report validation.
- Added real D3D9 and D3D11 render loops with headless software fallbacks and PNG screenshot verification.
- Added repeated launch, inject, health-check, and shutdown cycles.
- Added JSON, JUnit, screenshot, dump, report, and watchdog evidence artifacts.
- Final validation: 6/6 scenarios on x86 and 6/6 scenarios on x64.

### Security and reliability fixes

- Fixed HTTP JSON content-type enforcement being bypassable because the pre-routing hook executes before `cpp-httplib` populates `req.body`.
- Added payload detection through `Content-Length` and `Transfer-Encoding` before protected request parsing.
- Fixed shared-protocol alignment across MSVC, GCC, and Clang.
- Fixed 32-bit atomic/volatile compatibility in the external diagnostics channel.
- Added the missing `user32` linkage required by window responsiveness checks.
- Fixed an x64 stack-overflow risk caused by large temporary registry resets and test buffers.
- Added crash-time non-blocking registry snapshots to reduce deadlock risk.
- Fixed private helper collisions in Cortex's combined injected translation unit.
- Applied new freezes immediately and made the E2E freeze test verify bounded restoration after deliberate perturbations.

### Documentation and build system

- Added documentation for the mod SDK, hook diagnostics, symbol workflows, external diagnostics, and E2E environment.
- Added dedicated diagnostics workflows for milestones 2 through 7.
- Added full Windows build, unified-host, and E2E workflows.
- Added Release builds and packaging for both Windows x86 and x64.

## [v0.2.0] - 2026-07-22

- Added background capture and input, network hooks, the native MCP endpoint, expression-based debugger captures, session export, and the v0.2.0 quickstart documentation.

## [v0.1.0] - 2026-07-14

- Initial public Cortex release.

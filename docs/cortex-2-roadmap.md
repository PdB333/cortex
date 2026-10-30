# Cortex 2.0 roadmap and step 1 audit

This document records what Cortex v1.0.1 actually contains, what was measured,
what was fixed in step 1, and the plan for steps 2 to 10. It describes the
state of `main` at commit `91f5a22` plus the step 1 changes listed in
[§5](#5-step-1-fixes). It is a working document. Update it at the end of each
step, and do not mark a capability as done until it has been shown on a fixture.

## 1. How this audit was done, and what it could not check

| Item | Value |
|---|---|
| Date | 2026-10-30 |
| Machine | Linux container, 4 × Intel Xeon @ 2.10 GHz, 1 thread per core, 15 GiB RAM, kernel 6.18 |
| Windows build | MinGW-w64 13.2 (posix threads) cross-compiler from Ubuntu 24.04, x86_64 and i686 |
| Windows execution | Wine 9.0. **Wine is not Windows**: no real debugger, injection, D3D or ReadProcessMemory behaviour was checked here |
| Native checks | GCC 13.3 on Linux for the portable units (session manager, settings, scanners, benchmarks), with ThreadSanitizer |

**Checked here:**

- the root tree (runtime DLL, test target, all unit tests) cross-builds for
  **x64 and x86**;
- all root CTest suites pass under Wine: **x64 20/20** and **x86 20/20**. That
  is the 16 existing suites plus the four new project-store scenarios;
- the desktop (`app_imgui`, x64) cross-builds. Its 11 unit suites pass under
  Wine, including the new settings test and the benchmark smoke;
- `cortex.exe --gui-workspace-suite` (31 workspaces, 6 presets) and
  `--window-smoke-test` (native Win32 D3D11 window) pass under Wine + Xvfb.
  `--ai-activity-smoke` failed once on a cold Wine prefix (a "started" event
  missed within 8 s), then passed 4 out of 4 times. It is timing-sensitive, not
  a regression; see step 9.4;
- the portable units pass natively, and the session test also passes under
  ThreadSanitizer.

Note: the three GUI CTest entries use `$<TARGET_FILE:...>` as the command, so
CMake's cross-compiling emulator does not apply to them. Under Wine they must
be run as `wine cortex.exe <mode>`.

**Not checked here; left to Windows CI and a person:**

- the GUI attached-target, multi-session and cross-bitness suites, which need
  a live injected target;
- MCP end-to-end against a live target;
- the injection, debugger and AssaultCube E2E workflows;
- manual UI use.

The GitHub Actions workflows in `.github/workflows/` remain the gate.

## 2. Repository map

About 99 k lines of first-party C++/Python/PowerShell/QML, excluding third-party code.

| Path | Role | Notes |
|---|---|---|
| `core/` | `cortex_core.dll`, the in-process runtime: memory, scans, debugger (VEH), disasm, analysis, patch, struct inference, RE tools, project/knowledge store, Lua, hooks, capture, diagnostics, the route registry and named-pipe MCP server | Some subsystems run **inside the target** (`core/analysis`, `core/memory/scan.cpp`, `core/project`) |
| `core/services/` | Out-of-process services the desktop uses directly: `ValueScanner`, `PointerScanner`, memory tools, assembler, Auto Assembler, structure dissect, speedhack, `PayloadClient` (pipe transport), `OperationManager` | Portable over `target::Session`; unit-tested with an in-memory process |
| `core/target/` | Platform-neutral `Target`/`Node`/`Backend`/`Catalog`/`Session`/`SessionManager`, Windows local backend | Generation = process creation FILETIME (PID-reuse guard) |
| `app_imgui/` | Dear ImGui + D3D11 desktop (`cortex.exe`), application models over `RuntimeTransport`, CLI modes, GUI smoke suites | The production UI |
| `app/` | Frozen Qt 6 / QML desktop | **Obsolete**: not built or released; parity reference only (`app_imgui/PARITY_MATRIX.md`) |
| `host/` | External Windows debugger backend, VEH provider, probe/diagnose CLIs, unified host entry | `cortex_host.exe` only with `CORTEX_BUILD_LEGACY_COMPAT=ON` |
| `mcp_bridge/` | `cortex.exe mcp` host mode: dynamic targets, launch profiles, the `cortex_test_*` agent test layer (runner, steps, reports, chooser, generator, code context/link, knowledge graph) | Mostly header-only; heavily unit-tested |
| `agent/` | Agent guide, semantic-tools contract, testing guide, investigation template | Instructions and templates, no code |
| `injector/`, `runtime_helper/` | Standalone injector, private x86 bootstrap helper for cross-bitness load | |
| `sdk/include/cortex/` | Mod diagnostics SDK (C and C++) | |
| `tests/` | 40+ unit/contract suites, E2E PowerShell/Python, GUI suites, fixtures | Some suites are built ad hoc by workflows, not by CMake (see §3.4) |
| `.github/workflows/` | 9 workflows: contracts, full build, E2E, GUI suite, UI, unified host, AssaultCube offline, UI migration benchmarks, release | Windows runners with MSYS2 MinGW, plus one Ubuntu portable job |

## 3. Capability matrix

States:

- **functional**: implemented, wired to the UI and/or MCP, covered by tests or E2E;
- **partial**: works, but with known functional gaps;
- **experimental**: shipped behind documented caveats;
- **absent**: not implemented.

"Tests" names the suite that exercises the feature.

### 3.1 Dynamic analysis

| Capability | State | Location | UI | MCP | Tests | Limitations / duplication |
|---|---|---|---|---|---|---|
| Value scan (exact, bigger/smaller/between, unknown initial, increased/decreased (by), changed/unchanged, all numeric types, strings UTF-8/16, AOB with nibble wildcards, region/protection filters, fast scan, threads, cancel, progress, disk-backed snapshots) | functional | `core/services/value_scanner.cpp` | Memory › Value scan | — | `cortex_value_scanner_tests` | **Three scanners.** The desktop uses `ValueScanner` (out of process). MCP `scan_*` uses `core/memory/scan.cpp` (in the target). `core/services/scan_service.cpp` is used only by the frozen Qt app. Desktop and agent scan states are separate (step 2) |
| In-target scan (MCP) | functional | `core/memory/scan.cpp`, `core/api/routes_scan.cpp` | — | `scan_new`, `scan_next`, … | `cortex_core_tests`, E2E | Needs the runtime injected; `pause_process` is a control operation |
| Memory read/write, regions | functional | `core/services/memory_service.cpp`, `core/memory/` | Memory viewer | `memory_*` | core tests, security regression | Writes gated by the desktop switch / `--allow-writes` + `mutation_permission` |
| Hex viewer, data inspector, change highlighting | functional | `app_imgui/ui/memory_browser_workspace.cpp` | Memory viewer | `memory_read` | GUI workspace suite | No snapshot-to-snapshot diff in the viewer itself (snapshots workspace has it) |
| Snapshots, diff, rewind, last change | functional | `app_imgui/application/snapshots_model.cpp`, `core/timeline` | Snapshots | `snapshot_create/diff/rewind/last_change` | app models tests | Rewind is a write |
| Pointer scanner, rescan, save/load | functional | `core/services/pointer_scanner.cpp` | Tools › Pointer scan | — | `cortex_memory_tools_tests` | Desktop only; no stable/candidate/invalidated classification across sessions (step 3.3) |
| Pointer maps / spider | functional | `core/pointermap/`, `app_imgui/application/pointer_maps_model.cpp` | Pointer Maps | `pointermap_*` | E2E | In-target |
| Structure dissect (multi-instance) and inference | functional | `core/services/structure_dissect.cpp`, `core/struct/infer.cpp` | Structures | `struct_*`, `dissect_*` | `cortex_structure_dissect_tests`, core tests | Inference is heuristic and labelled as such |
| Debugger: SW/HW breakpoints, hit counts, hit logs, registers, threads, stack, pause/continue/step/step-over | functional | `host/debugger/` (external Windows debugger, VEH provider), `core/debugger/` | Debugger, Breakpoints | `debug_*` | E2E, runner E2E | Capabilities are declared per backend (`DebugCapability`). The VEH backend runs inside the target. The external backend suspends threads while attached |
| What accesses / writes an address | functional | `app_imgui/ui/access_finder_workspace.cpp`, HW data BPs | What accesses | `debug_*`, `cortex_test_run` breakpoints | GUI suite, runner E2E | A write trap reports the next instruction (documented in `AGENTS.md`) |
| Per-thread traces with register snapshots, coverage | functional | `core/debugger/`, `app_imgui/ui/trace_workspace.cpp` | Trace | `trace_*` | E2E | Bounded by count/time |
| Patches (bytes, NOP, asm, detours, trampolines, caves), action journal and rollback | functional | `core/patch/`, `core/action/` | Patches, Actions | `patch_*`, `actions_list/rollback` | `cortex_action_transaction_tests`, core tests | Rollback refuses only when original bytes differ |
| Assembler, Auto Assembler, CE tables | functional | `core/services/assembler.cpp`, `auto_assembler.cpp`, `app_imgui/application/cheat_table.cpp` | Addresses, AA editor | — | assembler / AA tests, app models | |
| Freezes, watches, page/alloc watch | functional | `core/freeze/`, `core/watch/` | Watches | `freeze_*`, `watch_*` | core tests | |
| Lua (CE-compatible, external) | functional | `app_imgui/application/lua_engine.cpp`, `core/scripting/` | Lua | `lua_exec`, `lua_scripts_*` (in-target) | `cortex_lua_engine_tests`, `lua_sandbox_tests` | Two Lua engines: desktop (external) and runtime (in-process) |
| Input injection, record/replay | functional | `core/hook/input_*`, `app_imgui/ui/input_workspace.cpp` | Input | `input_*`, `cortex_test_run` | runner E2E | Window-message delivery; not every game reads it |
| Screenshots, OCR, network events | functional / partial | `core/capture`, `core/ocr`, `core/hook/net_hook.cpp` | Screenshots, Network | `screenshot`, `ocr`, `network_*` | E2E | Renderer-dependent |
| Crash/hang diagnostics, minidumps, symbolized stacks, SDK | functional | `core/diagnostics`, `host/diagnostics`, `sdk/` | Diagnostics | CLI `diagnose`/`analyze` | diagnostics M1–M7 suites | |
| Speedhack | functional | `core/services/speedhack.cpp` | Tools | — | `cortex_speedhack_tests` | Write operation |

### 3.2 Static analysis

| Capability | State | Location | UI | MCP | Tests | Limitations |
|---|---|---|---|---|---|---|
| Disassembly (Zydis, x86/x64) | functional | `core/disasm/`, `core/services/disassembly_service.cpp` | Disassembler | `disasm` | core tests | Linear listing only. **Bitness = the runtime's bitness** |
| Function discovery | partial | `core/analysis/analysis.cpp` `FindFunctions` | Disassembler / RE | `analysis_functions` | E2E | Heuristic (ret+padding, call targets). Does not use `.pdata`/symbols, although `mcp_bridge/pe_unwind.h` can read x64 unwind ranges |
| CFG / structured CFG | partial | `analysis::BuildCFG`, `StructureCFG` | Disassembler (CFG tab) | `analysis_cfg` | E2E | Recursive descent of one function; no dominators, no trace-coverage overlay in the CFG view |
| Code xrefs / data xrefs | partial | `analysis::FindXRefs`, `FindDataXRefs` | Disassembler | `analysis_xrefs` | E2E | Direct and RIP-relative/absolute only; computed on demand by a linear sweep (no index) |
| PE headers, sections, imports, exports, patch diff vs disk | partial | `analysis::DissectPeHeaders`, `ScanPatches`; `core/services/memory_tools.cpp` | Modules | `analysis_pe_headers`, `analysis_scan_patches` | `cortex_memory_tools_tests` | **In-memory image only.** There is no offline PE file model (relocations, debug directory, rich header, file hash); `mcp_bridge/test_file_version.h` hashes files only for knowledge linking |
| Symbols (PDB via dbghelp, DWARF) | functional | `core/symbols`, `app_imgui/application/symbols_model.cpp` | Symbols | `symbols_*`, CLI `symbolize` | DWARF fixture, diagnostics | |
| Vtables + MSVC RTTI names | functional | `analysis::DumpVTable` | RE | `analysis_vtable` | E2E | Needs `/GR` |
| Annotations: comments, renames, bookmarks | partial | project addresses/notes, `re_facts` | Project, RE | `project_*` | E2E | No per-instruction comment or function rename model; no navigation history persistence |
| Pseudocode | absent | — | — | — | — | `StructureCFG` is explicitly "not decompilation" |
| Ghidra interop | partial | `core/ghidra/ghidra.cpp` | RE › Ghidra | `ghidra_export`, `ghidra_import`, `ghidra_import_symbols` | — | One-way JSON export plus a Jython import script the person runs inside Ghidra, and a JSON symbol import. No headless analysis, no decompilation, no binary-identity check, no RVA mapping beyond "first module base" (step 8) |

### 3.3 Investigation, knowledge and agents

| Capability | State | Location | UI | MCP | Tests |
|---|---|---|---|---|---|
| Native MCP server (stdio → authenticated pipe), 30 compact semantic tools, 251 tools with `--tools all`, dynamic multi-target, `tools/list_changed` | functional | `mcp_bridge/host_mode.cpp`, `core/api/` | AI Activity | all | MCP protocol/schema/authority/policy suites, security no-writes regression, native stdio E2E |
| Write authority (`--allow-writes` / `--allow-state` + `mutation_permission`), denial log | functional | `core/api`, `core/security/denial_log.h` | Read-only / Writes allowed switch | all | `mcp_authority_tests`, `security_no_writes_regression.py` |
| Semantic plans, bounded execution, cancellation, transactional rollback | functional | `core/api/semantic_tools.h`, `routes_status.cpp` | — | `cortex_*` semantic | `semantic_tools_tests`, `semantic_plan_p1_tests` |
| Launch profiles, restart/stop | functional | `mcp_bridge/launch_*` | — | `cortex_launch*`, `cortex_restart`, `cortex_stop` | `cortex_launch_config_tests`, launch E2E |
| Bounded control/action trials, compare, hypotheses, reports | functional | `mcp_bridge/test_runner.h`, `test_predictions.h` | — | `cortex_test_run/get/cancel/compare/report` | `cortex_test_runner_tests`, `test_runner_e2e.py` |
| Supervised multi-step journals, chooser, generator, catalog, investigation summary | functional (experimental status in docs) | `mcp_bridge/test_step.h`, `test_choose.h`, `test_generate.h`, `test_investigation.h` | — | `cortex_test_step_*`, `cortex_test_choose`, `cortex_test_generate*`, `cortex_test_catalog_*` | plan/generate/runner tests |
| Code context (IP → module+RVA, optional disasm, x64 unwind range) and code links to knowledge (on-disk SHA-256) | functional | `mcp_bridge/test_code_*.h`, `pe_unwind.h` | — | `cortex_test_code_context`, `cortex_test_code_link`, `cortex_knowledge_code_check` | code context/knowledge/candidates tests |
| Knowledge records (claims with status hypothesis/observed/refuted/stale, revisions, evidence, typed relations), query/resume, graph | functional (experimental) | `core/project/knowledge*.h`, `mcp_bridge/test_knowledge.h`, `test_evidence_graph.h` | Project (partial view) | `project_knowledge_*`, `cortex_knowledge_graph` | knowledge model/query/graph tests |
| Shared investigation context (desktop + MCP), caller provenance, operation history | functional (step 2) | `core/context/`, `core/api/routes_context.cpp`, `app_imgui/application/context_model.*` | Project › Investigation context, address menu | `context_get`, `context_events`, `context_observation_add` | `cortex_investigation_context_tests`, app models, native stdio E2E |
| Investigation orchestrator (question → plan → … → report states) | absent as one object | — | — | Pieces exist as journals | — |
| AI Investigation workspace in the desktop | absent | AI Activity shows calls only | — | — | — |

### 3.4 Infrastructure

| Item | State | Notes |
|---|---|---|
| `SessionManager` | **fixed in step 1** | Was unsynchronized but shared by the frame loop, background tasks and the debugger backend; see §5.1 |
| `OperationManager` (typed operation state, cancel, timeout) | functional | Used for runtime operations; not yet the single contract for UI + MCP results (step 2.5) |
| Event bus | functional (step 2) | `core/events`: 2048-entry ring with a typed catalog (`events::types`). Context events carry pid and generation. `events::Read` and `/ui/events` / `context_events` report `oldest_id`, `missed` and `gap`. `breakpoint.hit` is derived from hit counters at refresh time, never from the exception path |
| Project persistence | functional, **hardened in step 1** | Atomic replace (`ReplaceFile`/`MoveFileEx` with write-through) and `.bak`; schema migration to v3. A damaged file is now quarantined (§5.3) |
| Desktop settings / `cortex.ini` | **fixed in step 1** | Were rewritten in place with `trunc` (§5.2) |
| Test-runner records | functional | tmp + `std::filesystem::rename` |
| Ad hoc test builds | debt | `action_transaction`, `semantic_tools`, `mcp_protocol`, `mcp_authority`, `mcp_schema`, `mcp_query_schema`, `mcp_pipe_protocol`, `call_scheduler`, `mcp_bridge_policy`, `request_id`, `pagination` are compiled by inline `c++` commands in `contracts.yml`, not by CMake, so `ctest` alone does not run them (step 9.6) |

### 3.5 Obsolete or duplicated, kept for now

Nothing below was deleted in step 1. Each needs its own change, with a replacement already proven.

| Item | Why it is obsolete | Removal prerequisite |
|---|---|---|
| `app/` (Qt/QML desktop, including `app/mcp_mode.cpp`, a second copy of the MCP host) | Replaced by `app_imgui/` and `mcp_bridge/host_mode.cpp` | Parity matrix closed (`app_imgui/PARITY_MATRIX.md`); a separate PR |
| `core/services/scan_service.cpp` | Only `app/` uses it; `ValueScanner` supersedes it | Removed with `app/` |
| `cortex_host.exe` legacy compat target | `cortex.exe` owns the CLI | `serve`/`inject` migration finished |
| `ci/assaultcube_offline_e2e.ps1` v1/v2 next to v3 | Superseded scripts: `assaultcube-offline.yml` references only `_v3.ps1` | None; can be removed in a cleanup PR |
| Two Lua engines, two scanners (desktop/runtime) | Different execution sites (external vs in-target) | Not a removal: unify *results and state* (step 2), not engines |

## 4. Baseline benchmarks

`tests/bench/cortex_baseline_bench.cpp` (`cortex_baseline_bench`, built with
the app tests) measures Cortex's own algorithms on a deterministic in-memory
fixture: 256 × 1 MiB pseudo-random private regions, a 1 MiB image module,
1000 planted `int32` values, a pointer-dense heap with a known two-level chain,
and an 8 MiB synthetic instruction stream of common compiler encodings. Every
benchmark also checks its result. With `--quick` (8 MiB, 1 run) it is the CTest
entry `cortex_baseline_bench_smoke`.

The fixture's `ReadMemory` is a `memcpy`. These numbers are therefore an
**upper bound set by the algorithms**, not end-to-end figures against a live
process: `ReadProcessMemory`, `VirtualQueryEx`, page faults and the target's
own layout are not included.

Reference run: `docs/benchmarks/baseline-2026-10-30-linux-x64-container.json`
(native Linux build, GCC 13.3 `-O3`, machine in §1, median of 5 runs, 2–3 for
the slower benchmarks).

| Benchmark | Median | Throughput | Check |
|---|---:|---:|---|
| First scan, exact i32, 1 thread | 163.1 ms | 1.58 GiB/s | 1000/1000 found |
| First scan, exact i32, 4 threads | 46.6 ms | 5.51 GiB/s | 1000/1000 found |
| Next scan, changed, 1000 listed | 0.60 ms | — | 500/500 kept |
| First scan, unknown initial value (snapshot) | 364.0 ms | 706 MiB/s | snapshot kept |
| Next scan, decreased, from snapshot | 87.7 ms | 2.93 GiB/s | 1000/1000 isolated |
| AOB with wildcards (unaligned) | 14.6 ms | 17.6 GiB/s | 16/16 found |
| Pointer scan, 4 levels, max offset 0x400, 129 k noise pointers (133 k indexed) | 57.5 ms | — | planted chain found |
| Session read, 4 KiB chunks (64 MiB) | 4.2 ms | 15.3 GiB/s | memcpy bound |
| Session read, 64 KiB chunks (64 MiB) | 3.2 ms | 19.9 GiB/s | memcpy bound |
| Zydis decode + Intel format, x86 (2.52 M instructions) | 475 ms | 16.8 MiB/s, ≈5.3 M instr/s | all decoded |
| Zydis decode + Intel format, x64 (2.13 M instructions) | 477 ms | 16.8 MiB/s, ≈4.5 M instr/s | all decoded |

Observations to act on only after Windows numbers confirm them (step 9.5):

- The unknown-initial-value first scan is the slowest scanner path. It writes
  the snapshot to temporary files, so it is disk-bound.
- Formatting dominates disassembly cost, so a CFG or xref index built on the
  render thread would stall the UI (step 4.3).

**Open baselines that need a Windows host.** The GUI release job now uploads a
Windows run of the benchmark as the `cortex-baseline-windows-x64` artifact.
The following still have no measurement:

| Baseline | How to measure | Status |
|---|---|---|
| Scan time against a live process (`ReadProcessMemory`) | `cortex_test_target` + `ValueScanner` through `WindowsLocalBackend` | open |
| MCP round-trip latency (stdio → pipe → runtime) | Timestamp `tools/call` for `memory_read`, `disasm` and `scan_new` in `tests/native_mcp_stdio_e2e.ps1` | open |
| UI frame time and startup | `ui-migration-benchmarks.yml` measures startup, memory and executable size; extend it with frame time under a scan | partial |
| Cortex working set | Same workflow | partial |
| Target crash/close during an operation | E2E: kill `cortex_test_target` during a scan, a trace and a test run; expect `target_lost` and no hang | open (step 9.4) |

## 5. Step 1 fixes

All three fixes target problems the step 1 brief names: crashes, corruption,
stale state and synchronization. Each one is covered by a test that fails
without it.

### 5.1 `SessionManager` data races and use-after-free

**Problem.** One `SessionManager` is shared by the desktop frame loop, its
`std::async` workers (scans, pointer scans, `RunInBackground`), `PayloadClient`
and the external debugger backend. It had no lock: `Attach`, `Detach`,
`Activate` and `PruneDeadSessions` mutated an `unordered_map` and a string that
other threads read. `ActiveTargetId()` returned a reference to that string.
`ActiveTarget()` and `ActiveCapabilities()` returned raw pointers into a
session that a later detach could free.

**Fix.** `core/target/session_manager.{h,cpp}`:

- one mutex over all state;
- the catalog's process open runs outside the lock;
- `ActiveTargetId()` returns a `std::string`;
- `ActiveTarget()` and `ActiveCapabilities()` return `std::optional` snapshots.

Every existing caller compiles unchanged.

**Evidence.** The extended `tests/session_manager_tests.cpp` adds two checks:
a held snapshot outlives a detach, and 4 reader threads run against a writer
that attaches, detaches, activates and prunes. Under ThreadSanitizer, the
previous implementation reports **45 data races and 14 heap-use-after-free**.
The new one reports none. The Ubuntu `contracts.yml` job now runs this test
under TSan with `halt_on_error=1`.

### 5.2 Non-atomic settings and `cortex.ini` writes

**Problem.** `SettingsStore::Save` and the `cortex.ini` sync opened the live
file with `std::ios::trunc`. A crash or a full disk mid-write left a truncated
file. A truncated settings file is reset to defaults on load. A truncated
`cortex.ini` can silently drop `project_directory` or the diagnostics settings.

**Fix.** A new `services::WriteFileAtomically` in `core/services/user_data.h`
writes `<file>.tmp`, flushes it, then replaces the file. On Windows it uses
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`; elsewhere it uses `rename`. It
removes the temporary on failure. Both writers now use it.

**Evidence.** The new `tests/settings_store_tests.cpp`
(`cortex_settings_store_tests`) checks:

- the settings round-trip;
- the `.ini` keeps unmanaged keys and comments, and drops duplicates;
- no temporary file is left behind;
- a stale temporary from a crashed write is ignored and replaced;
- a failed replace leaves the previous file intact.

It passes natively and under Wine. Against the previous code, the
leftover-temporary check fails.

### 5.3 A damaged project file could be lost

**Problem.** If `<process>.json` did not parse and `.bak` was missing or also
damaged, the runtime silently started from defaults. The next two saves then
rotated the damaged bytes into `.bak` and deleted them, so the investigation
records were unrecoverable. When the primary was damaged but `.bak` was good,
the first save moved the *damaged* primary into `.bak`, destroying the only
good backup.

**Fix.** `core/project/project.cpp`:

- an unreadable primary or backup is copied to `<file>.damaged-<FILETIME>`
  before anything else happens;
- the damaged primary is then removed, so the next save does not rotate it
  over the good backup;
- if an unreadable primary cannot even be copied (locked, access denied),
  every save in that session fails rather than replace bytes nobody has kept;
- `LoadJsonFile` no longer leaves a partially parsed document in the output;
- the new `project::LoadStatus()` reports the load source (`primary`, `backup`,
  `new` or `defaults_after_damage`), the quarantined copies, `saves_blocked`
  and the schema version. The runtime log records it at start-up. It is not
  yet exposed over MCP or in the UI (step 5).

**Evidence.** The new `tests/project_store_tests.cpp` runs four CTest
scenarios: `backup`, `damaged`, `fresh` and `locked`. They check the
quarantined bytes, the load source, that the backup is still valid after two
saves, and that a locked, unreadable primary is never replaced. All four pass
under Wine.

### 5.4 Smaller corrections

- `core/disasm/disasm.h`: the comment said the disassembler decodes x86-32; it
  decodes the runtime's own bitness.

## 6. Plan for steps 2 to 10

Each step lists:

- what already exists (reuse, do not re-implement);
- the real gaps;
- the planned tasks, each meant to be its own PR;
- validation;
- dependencies.

"Fixture" means `test_target/` (x86 and x64) unless stated otherwise.

### Step 2: one engine for the UI, agents and CLI (done)

**Design decision.** The shared state lives **in the runtime**, inside the
target. That is the only component the desktop and every `cortex.exe mcp`
client already reach through the same authenticated pipe and route registry.
Putting it in the desktop would have meant a second server for agents.
Putting it in `cortex.exe mcp` would have hidden it from the desktop. The
runtime's lifetime is exactly one process generation, so the context cannot
outlive the process it describes.

**Delivered:**

| Brief item | Implementation | Test |
|---|---|---|
| 2.2 Shared investigation context | `core/context/investigation_context.*`. It holds identity (pid, generation, architecture, executable), modules, `module+RVA` location, the person's selections, observations with provenance, and a bounded operation history. Routes are `GET /context`, `GET /context/events`, `POST /context/observation` and `POST /context/select` (desktop only). The MCP tools are `context_get`, `context_events` and `context_observation_add` (state tier). The desktop has the address-menu action **Share with investigation context** and two Project tabs, **Knowledge** and **Investigation context** | `cortex_investigation_context_tests` (portable, TSan-clean), `cortex_app_models_tests` |
| Caller provenance | `core/context/caller.h`: a thread-local `desktop` / `agent` label set by the pipe dispatcher from the authenticated envelope (`PayloadClient::SetDesktopChannel`), never from tool arguments. It labels provenance and grants nothing | same |
| 2.3 Typed events | Type catalog in `core/events/events.h`: `runtime.started`, `module.loaded/unloaded`, `context.address_selected/observation_added`, `knowledge.updated`, `evidence.created` and `agent.action_requested/completed/failed`. `events::Read` reports `oldest_id`, `missed` and `gap`; `/ui/events` and `context_events` expose them | `cortex_events_tests` |
| 2.4 Process isolation | `status` reports the runtime's `generation`. `PayloadClient::VerifyTarget` refuses a runtime with the same PID but another generation (`payload_target_generation_mismatch`). Every context route takes `expected_generation` and refuses a stale one with HTTP 409 before recording anything. The host already checked `_cortex_generation` | context tests, app models (stale refresh/select refused) |
| 2.5 Result contract | `api::response::Operation` / `OperationResult` with `ok, status, error, cause, target, duration_ms, limits, data, evidence, permissions_required, rollback`. The context routes use it | context tests |
| Agent actions visible to the person | `mcp_tools::RecordedDispatch` puts every primitive call (direct or semantic step) into the context history. An agent's state, control, mutate or native-call tool publishes `agent.action_*` with argument names only | runtime E2E on Windows (open) |

**Completed in the second delivery:**

| Item | Implementation | Test |
|---|---|---|
| 2.1 Desktop scan and debugger state shared | `POST /context/share` (desktop only, bounded: 256 rows, 64 KiB). The memory workspace shares each finished scan (switch, on by default). The frame loop shares breakpoints, hit counters and paused threads at most once a second when they change. They appear in `context_get` as `shared_state`, with `module+RVA` | context tests, app models |
| 2.3 `breakpoint.hit` | `InvestigationContext::UpdateBreakpointHits`: counter deltas for the runtime's VEH breakpoints (each context route) and the desktop's external debugger (each share). Never published from the exception path. The first poll is the baseline; a breakpoint added later counts from zero | context tests |
| 2.4 Desktop operations bound to their target | `ui::TargetKey` (id + generation) replaces id-only change detection in the Memory, Project, RE and Trace workspaces, so a reused PID resets them. A value scan finishing after the target changed is discarded; a pointer scan from a previous process is labelled for rescanning. Other long operations already block target switching while they run | GUI workspace suite; scan binding by code review |
| 2.5 Contract in existing routes | `response::Annotate` adds `status, target, duration_ms, limits, evidence, permissions_required, rollback` to `scan_new`, `scan_next` and `debug_breakpoint_add`, without changing any field they already return | context tests (`Annotate`), MCP schema/authority suites |
| Isolation E2E | `tests/context_isolation_e2e.py`: two injected targets on one `cortex.exe mcp`. It checks identity, no cross-talk, stale generations refused by the host and by the runtime, nothing recorded by a refused call, `agent.action_*` events without argument values, history provenance, detach/reattach, and one target killed. It runs in the GUI suite workflow for x64 and x86 | run here on live runtimes (Wine, x64) |

**Validation against the step 2 criteria:**

- *An address found in the UI is immediately readable by MCP.*
  `cortex_app_models_tests` drives the desktop model against the real route
  semantics. The person's selection, scan results and debugger state are read
  back as an agent reads them, with `module+RVA`.
- *A discovery saved by the agent appears in the project open in Cortex.*
  The Project **Knowledge** tab lists `project_knowledge_put` records and
  refreshes on `knowledge.updated` / `evidence.created`. Agent observations
  appear in **Investigation context** with their session. On live runtimes,
  `context_isolation_e2e.py` shows the observation in `context_get` with
  agent provenance.
- *Multi-process operations stay isolated, including across disconnect and
  reattach.* `context_isolation_e2e.py` passes against two live runtimes.

**Verification environment.** The E2E ran on live runtimes injected under
Wine (x64). Windows CI runs it for x64 and x86 in the GUI suite workflow; it
had not run there when this was written.

**Deliberately left as is:**

- `context_observation_add` is not available from the compact profile:
  semantic execution only runs state tools that have a rollback contract.
- Older routes keep their own shapes plus the added contract fields; a
  breaking migration needs a protocol version bump.

### Step 3: manual mode (done)

**Verified as already functional.** Nothing was re-implemented:

- **3.1 scanner** (§3.1): every type, comparison, AOB, threads, cancel,
  progress, partial reads;
- **3.4 debugger**: SW/HW breakpoints, hit counters, logs, registers, threads,
  steps, capabilities per backend;
- **3.5 editing**: patches, freezes, Auto Assembler;
- **3.6 navigation**: the shared address menu already goes from a value to
  "Find out what writes to this address", and from a hit to the
  disassembler.

**Real gaps closed:**

| Brief item | Gap | Implementation | Test |
|---|---|---|---|
| Validation fixture | The test program changed scalar globals only | `CortexDemoActor` in `test_target/main.cpp`. It is heap-allocated (found by scanning), reached from the exported `g_cortex_demo_actor` (a pointer path exists) and written only by `CortexDemoApplyDamage` (one responsible instruction). It is triggered by **D** or by a `<manifest>.damage` file | `tests/manual_demo_e2e.py` |
| 3.5 Reversible patches | `patch::Revert` wrote the original bytes blindly: it did not check the patched bytes were still there, ignored newer overlapping patches and ignored write failures | `patch::RevertChecked`, which refuses with `patch_target_changed` (bytes rewritten or memory gone), `patch_overlaps_newer:<id>` or `patch_write_failed`. The target and the registry are left untouched. `DELETE /patch/{id}` and `batch patch_revert` report the code (409/404/500). There is deliberately no force option | `cortex_core_tests` (5 cases) |
| 3.3 Pointer paths | Paths could only be filtered by `Rescan`, which drops them; nothing distinguished stable, candidate and invalidated | `PointerScanner::Validate` records a check per process generation without dropping anything. `Status` returns `stable` (target reached in ≥2 distinct known generations), `candidate` or `invalidated` (one miss). The scan records its own generation. File format v2 keeps the history; v1 still loads. Desktop: **Validate here** and a **Status** column | `cortex_memory_tools_tests` |
| 3.2 Memory viewer | Only a 1.2 s change flash, no comparison between two captures | **Capture**, **Compare with capture** (differences stay highlighted), a changed-bytes counter and **Next change**. `services::DiffRuns` skips pages unreadable in either capture. The viewer now resets on a reused PID too | `cortex_memory_tools_tests` (`TestByteDiff`) |

**Validation (the step 3 criterion).** `tests/manual_demo_e2e.py` runs the
full chain on the fixture: find the value, watch it, find the instruction
that writes it, navigate the code, save. It passed on live runtimes under
Wine for **x64 and x86**, twice in a row (rerun-safe through
`expected_revision`). CI runs it for both architectures in the GUI suite.
`docs/demo-manual.md` is the desktop version of the same workflow.

**Not done and why:**

- **New visual demo assets** (screenshots of the steps). These need a real
  Windows desktop session, so they are left to step 10.
- **A "Find what writes" shortcut directly in the scan result row.** The
  shared address menu is already one right-click away from every result; no
  new code path was needed.
### Step 4: static analysis on an artifact-provider architecture

Extended for multi-runtime support (§8). The offline PE model planned here
becomes the first provider of a general **artifact provider** system rather
than a one-off parser. That way .NET, JavaScript, Electron and every later
runtime plug into the same detection, identity, evidence, UI and MCP path.

- **Reuse:**
  - Zydis and `analysis::*` (CFG and xrefs on bytes);
  - `mcp_bridge/pe_unwind.h` (`.pdata`);
  - `test_file_version.h` (SHA-256);
  - symbols;
  - the investigation context and knowledge store (step 2/5);
  - the launch-profile pattern (operator-owned configuration outside MCP)
    for file roots.
- **4a. Provider foundation and native PE:**
  1. `core/artifacts/`: an `ArtifactProvider` interface (detect, parse,
     list children, read child) and a registry. Every result carries the
     artifact identity (path, size, SHA-256, format, provider and version)
     and per-capability status (`static`, `decompile`, `live`). Each
     capability is separately `validated` or `experimental` against a named
     fixture.
  2. An offline PE provider over file bytes: headers, sections, imports,
     exports, relocations, debug directory (CodeView GUID/age, PDB path),
     TLS, exception directory and the CLR header. It is bounds-checked and
     fuzz-tested, and kept separate from the loaded-image view.
  3. Container providers, so nested artifacts are reached in one walk:
     ZIP (JAR, APK, NuGet, wheels) and ASAR.
  4. Access:
     - CLI `cortex.exe inspect <file> [--json]`;
     - MCP host tools `cortex_artifact_inspect` / `cortex_artifact_children` /
       `cortex_artifact_read`, read-only and limited to the roots the
       operator passes with `--artifact-root`. An agent cannot widen them;
     - desktop: an **Artifacts** workspace.
- **4b. JavaScript/TypeScript and Electron, static:**
  - a JS/TS tokenizer that knows strings, templates, regexes and comments;
  - extraction of ES module `import`/`export`, CommonJS `require`, and
    dynamic `import()` with literal specifiers, plus a module graph;
  - detection of minified and bundled code (webpack/esbuild/rollup runtime
    signatures);
  - source maps: inline or `sourceMappingURL`, VLQ decoding, and mapping back
    to original files;
  - Electron: the ASAR provider (header, file table, unpacked entries,
    integrity hash when present), `package.json` `main`, `BrowserWindow`
    `preload` and `webPreferences` extraction, and the IPC channels named in
    `ipcMain`/`ipcRenderer`/`contextBridge` calls. A static channel list is
    labelled as such, never as observed traffic.
- **4c. .NET, static:**
  - CLR header, metadata root and streams (`#~`, `#Strings`, `#US`, `#GUID`,
    `#Blob`);
  - tables `Module`, `TypeRef`, `TypeDef`, `Field`, `MethodDef`, `Param`,
    `MemberRef`, `AssemblyRef`, `Assembly` and `ManifestResource`;
  - method bodies (tiny and fat headers) and a CIL disassembler (opcode
    table, operand tokens resolved against the tables);
  - assembly references graph.
- **4d. Native static analysis as planned before:**
  - a function catalog with per-entry provenance (`pdata`, `export`,
    `symbol`, `call_target`, `heuristic`);
  - an xref index built in the background with `OperationManager` progress;
  - a CFG view with block coverage from traces, computed off the render
    thread;
  - annotations keyed by artifact SHA-256 + `module+RVA` (or + metadata token
    for .NET, + source location for JS);
  - a pseudocode view interface, filled by providers in step 8.
- **Validation:** golden-JSON fixtures, built or committed in the repo:
  - the test target with symbols (PE);
  - a small .NET assembly compiled in CI (`dotnet build`, C# with known
    types, methods and IL);
  - a JS project with ESM, CJS, a bundle and a source map;
  - an ASAR packed by `@electron/asar` in CI;
  - fuzzing of every binary parser.

### Step 5: evidence and knowledge

- **Multi-runtime addition:**
  - evidence subjects become artifact-aware: SHA-256 + format + location,
    where the location is `module+RVA`, a .NET metadata token, a JS
    file:line:column through its source map, a JVM class/method or a Wasm
    function index;
  - typed cross-runtime relations, for example
    `js_function -calls-> native_export`,
    `il2cpp_method -compiled_to-> module+RVA` and
    `ipc_channel -handled_by-> js_function`. Each keeps its provenance and
    its evidence class (static, decompiled or observed).

- **Reuse:**
  - knowledge records (status, revisions, evidence, typed relations);
  - test reports;
  - code links (module, RVA, SHA-256);
  - project atomic persistence and the quarantine (§5.3).
- **Gaps:**
  - one evidence record type shared by trials, scans, breakpoints and static
    results;
  - the classification `verified observation | correlation | hypothesis |
    contradictory | inconclusive | refuted`, mapped onto the existing
    `hypothesis | observed | refuted | stale` without silent upgrades;
  - session-to-session comparison by binary identity + RVA;
  - a documented JSON/Markdown export.
- **Validation:**
  - reopen a completed investigation after a restart and confirm that no new
    observation is created;
  - old evidence stays readable without the process.

### Step 6: investigation orchestrator

- **Multi-runtime addition:** cross-runtime workflows over the same
  journal, for example:
  - "Electron IPC to native addon": the renderer call, the main-process
    handler, then the `.node` PE export, then a breakpoint;
  - ".NET method to JIT code": the metadata token, the runtime method
    address, then a native trace;
  - "IL2CPP method to native": `global-metadata.dat` method, then its RVA in
    `GameAssembly.dll`, then a breakpoint.

  Each hop records which capability produced it.

- **Reuse:**
  - the `cortex_test_step_*` journals, which already implement the
    plan → control → action → report loop with revisions and human approval;
  - the chooser and generator;
  - bounded runs;
  - the two-level permissions.
- **Gap:** one investigation object with states (`planned, running,
  awaiting_approval, completed, failed, cancelled, inconclusive`), an explicit
  plan display, limits and workflows (memory, function, state, structure,
  crash) that compose the existing tools.
- **Rule:** no second agent system. The orchestrator is a journal plus MCP
  tools, and the AI client stays external.

### Step 7: AI investigation workspace

- **Multi-runtime addition:**
  - the Artifacts workspace (4a) gains per-format views: module graph and
    source-mapped code for JS, the ASAR tree and IPC list for Electron, a
    type/method tree with CIL and decompiled C# for .NET, classes for the
    JVM, functions and imports for Wasm;
  - a process tree for multi-process targets (Electron
    main/renderer/GPU/utility).

  Everything is reachable from the same address/selection menus and shared
  with agents through the investigation context.

- **Reuse:** AI Activity model, workspace registry, address context menu,
  background tasks.
- **Gap:**
  - a workspace that lists investigations, plans, approvals and evidence
    (read from the step 5/6 store);
  - context actions shown only when their capability is present;
  - bidirectional navigation using `module+RVA`.
- **Rule:** no API key in Cortex; the AI side is any MCP client.

### Step 8: Ghidra and the `AnalysisProvider` interface

- **Multi-runtime addition (external engines, all optional, invoked as
  processes, never linked):**
  - **ILSpy** (`ilspycmd`, MIT) for C# decompilation;
  - **Vineflower** (Apache-2.0) or **CFR** (MIT) for Java;
  - **Ghidra** for native code, and for IL2CPP correlation helpers.

  Live-runtime providers:
  - **CDP** (Chrome DevTools Protocol, WebSocket) for Node.js and Electron
    main/renderer/preload: scripts, breakpoints, console, and the IPC
    observed through `Runtime.evaluate`-free hooks set with `Debugger`
    breakpoints only;
  - **.NET live**: a CLR-aware provider. First it reads managed state from
    dumps and live processes through the CLR's data access layer; managed
    breakpoints come after that is validated;
  - **JDWP** for the JVM;
  - **DAP/debugpy** for Python.

  Each provider declares and is validated per capability.

- **Reuse:** the `ghidra_export`/`ghidra_import_symbols` routes and the existing
  Jython script.
- **Tasks:**
  1. An `AnalysisProvider` interface: name, version, platforms, formats,
     features, availability, permissions and typed results with provenance.
  2. A Ghidra provider:
     - detect it (`GHIDRA_INSTALL_DIR` or an explicit path, version check);
     - run `analyzeHeadless` in a **Cortex-owned temporary project**, never
       an existing project of the person's;
     - a post-script that exports functions, symbols, xrefs, types and
       decompiled C keyed by RVA;
     - a cache keyed by SHA-256;
     - cancellation by process termination.
  3. Map the results to the loaded module only when the hash matches.
- **Licence:** Ghidra is Apache-2.0 and is invoked as an external process, not
  linked. Record this in `docs/third-party.md`.
- **Validation:** CI with a pinned Ghidra on the PE fixture.

### Step 9: quality

- **Multi-runtime addition:**
  - fuzz every new parser (PE, CLR metadata, ZIP, ASAR, source maps, class
    files, Wasm, PyInstaller CArchive, `global-metadata.dat`);
  - an E2E per live provider against an owned fixture (a Node/Electron app
    launched through a launch profile with `--inspect`/`--remote-debugging-port`,
    a .NET console app, a JVM app with JDWP, a Python app with debugpy).

- **Tasks:**
  - move the ad hoc `contracts.yml` builds into CMake/CTest;
  - fuzz the PE parser and the JSON loaders;
  - robustness E2E (target killed, module unloaded, provider absent or crashed,
    damaged project, which is covered for the project file by §5.3);
  - compare against §4 baselines;
  - benchmark fixtures against CE/Ghidra, measured per dimension.

### Step 10: product

- The README order, the portable archive, three demos and the guides listed in
  the brief.
- `CONTRIBUTING.md`, `SECURITY.md`, issue and PR templates. None exist today:
  `.github/` contains workflows only.
- A release only after explicit approval.

## 8. Multi-runtime support (added requirement)

Cortex must analyze more than native Windows C/C++ binaries. This section
places that work in the remaining steps; completed steps are not reopened.
What they built is reused:
- the investigation context and caller provenance (step 2) carry any
  runtime's selections and observations;
- the evidence/knowledge store and result contract (steps 2 and 5) are
  runtime-neutral;
- the launch profiles (operator-owned) are how a live runtime is started
  with its debug interface.

### 8.1 Principles

- **One provider system, not separate tools.** Artifact providers handle
  static work on files; target backends and debug providers handle live
  processes (the existing `target::Backend` / `DebugProvider` shapes,
  extended). The GUI, CLI and MCP reach all of them through the same
  registry.
- **Three capabilities, validated separately.** Static parsing, decompilation
  and live debugging are separate capabilities, each `validated` only against
  a named fixture in CI. A provider that parses .NET metadata does not
  thereby claim C# decompilation or managed debugging.
- **The native engine stays first-class.** The memory scanner, debugger and
  runtime injection are unchanged; managed and scripting providers sit
  beside them. Native correlation (a JS native addon, a JIT-compiled method,
  IL2CPP code) always ends in the native engine's `module+RVA`.
- **Security unchanged.**
  - Every file and process response is untrusted data: parsers are
    bounds-checked and fuzzed.
  - MCP file access is limited to operator-given roots.
  - Live debug interfaces are reached only on localhost, for processes the
    operator started through a launch profile or explicitly attached, and
    only with the existing write authority for anything that changes the
    target.
  - No bypass of protections. If an Electron app disables inspection with
    its fuses, or a .NET app is obfuscated or signed against tampering,
    Cortex reports it and does not circumvent it.

### 8.2 Order and placement

| Priority | Technology | Static (step 4) | Decompile (step 8) | Live (step 8, E2E step 9) |
|---|---|---|---|---|
| 1 | **JavaScript / TypeScript** | Tokenizer, ESM/CJS imports and exports, module graph, bundle/minifier detection, source maps (4b) | Source-map recovery of original files; no decompiler needed | CDP for Node.js: scripts, breakpoints, call frames, console |
| 1 | **Electron** | ASAR provider, `package.json` main, preload and `webPreferences`, static IPC channel list (4b) | as JS | CDP for main/renderer/preload, the process tree, IPC observed via breakpoints on `ipcMain`/`ipcRenderer` handlers, native addon (`.node` = PE) correlation to the native engine |
| 1 | **.NET / C#** | CLR metadata tables, CIL disassembly, assembly graph (4c) | ILSpy (`ilspycmd`) provider, mapped back to metadata tokens | Managed state read from dumps and live processes first; managed breakpoints after that is validated; JIT address to native engine correlation |
| 2 | **Python** | `.py` imports/defs, `.pyc` header and marshal reading, PyInstaller CArchive/PYZ extraction, dependency list | Bytecode disassembly (`dis`-equivalent for supported versions); source decompilers only where a licensed, maintained one exists | DAP through debugpy for processes started with it |
| 2 | **Unity / IL2CPP** | UnityFS bundle headers and asset listing, `global-metadata.dat` (types, methods, strings), managed assemblies through the .NET provider | .NET provider for Mono builds | IL2CPP method to `GameAssembly.dll` RVA, then the native engine (breakpoints, traces) |
| 3 | **Java / JVM** | JAR (ZIP provider), class file parser (constant pool, fields, methods, attributes), bytecode disassembly | Vineflower/CFR provider | JDWP |
| 3 | **WebAssembly** | Module parser (sections, types, imports/exports, functions, data), disassembler | Text format output | Through CDP for Wasm running in Chromium/Node |
| later | **Android APK, ELF, Mach-O, other platforms** | ZIP + DEX provider; ELF and Mach-O providers beside PE | external engines | new target backends (Linux runtime parity already planned) |

Priority 1 is implemented in step 4 (static) and step 8 (decompile and
live), before priority 2 starts. Priority 2 and 3 follow once priority 1 is
validated in CI. The "later" row is planned, not scheduled.

### 8.3 Validation fixtures (to build with each provider)

| Technology | Fixture |
|---|---|
| JS | A repo-local project: ESM and CJS modules, a webpack/esbuild bundle and its source map, built in CI with pinned versions |
| Electron | A minimal app (main, preload, renderer, two IPC channels, one native addon), packed into an ASAR in CI and launched through a launch profile |
| .NET | A C# console app and a class library (`dotnet build`, pinned SDK) with known types, generics, properties, events and IL patterns |
| Python | A script plus a PyInstaller one-file build of it (pinned PyInstaller) |
| Unity | A tiny Unity-independent `global-metadata.dat` fixture generated from a known IL2CPP output, plus a UnityFS header fixture; a full Unity build is optional |
| JVM | A `javac`-built JAR with known classes |
| Wasm | A `.wat`-assembled module with imports, exports, globals and a data segment |

### 8.4 Agents across runtimes

The agent works through the same MCP surface:
- artifact tools (read-only, operator roots);
- the investigation context, now carrying artifact-located selections and
  observations;
- knowledge records whose evidence names the capability that produced them.

A cross-runtime chain is a sequence of linked records, for example:
1. JS renderer call;
2. `ipcMain` handler;
3. native addon export `module+RVA`;
4. breakpoint hit.

Each link is labelled static, decompiled or observed. A static link never
becomes "observed" without a live capability producing it.

## 9. Risks and open questions

- **Generation 0 is a wildcard.** `SessionManager::Attach` treats an unknown
  process generation as "same process". If `GetProcessTimes` fails, a recycled
  PID could reuse a stale session. Making this strict needs a check of every
  backend that can report 0.
- **In-target analysis.** Static analysis runs inside the target through the
  runtime, so it needs injection and the runtime's bitness. The offline PE model
  (step 4) removes that dependency for static work.
- **Wine coverage.** Unit tests pass under Wine. The behaviour of the
  debugger, the injection path and the UI on Windows rests on the existing
  Windows CI.

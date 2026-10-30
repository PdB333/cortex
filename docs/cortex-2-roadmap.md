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
| Investigation orchestrator (question → plan → … → report states) | absent as one object | — | — | Pieces exist as journals | — |
| AI Investigation workspace in the desktop | absent | AI Activity shows calls only | — | — | — |

### 3.4 Infrastructure

| Item | State | Notes |
|---|---|---|
| `SessionManager` | **fixed in step 1** | Was unsynchronized but shared by the frame loop, background tasks and the debugger backend; see §5.1 |
| `OperationManager` (typed operation state, cancel, timeout) | functional | Used for runtime operations; not yet the single contract for UI + MCP results (step 2.5) |
| Event bus | partial | `core/events`: 2048-entry in-process ring, string type + JSON string payload, no target/generation in the envelope, no explicit gap signal (ids are contiguous, so a client *can* detect a gap, but the API does not report it). Only a handful of event types are published |
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

### Step 2: one engine for the UI, agents and CLI

- **Reuse:**
  - `RuntimeTransport` / `RuntimeModelBase` / `PayloadClient`;
  - `OperationManager`;
  - `core/api` route contracts and `response_contract.h`;
  - the target generation;
  - `cortex_targets`.
- **Gaps:**
  - The desktop's `ValueScanner` results, pointer scans and debugger state are
    invisible to MCP. Only what is saved to the project (inside the runtime)
    is shared.
  - The event envelope has no target/generation and no gap signal.
  - There is no shared result contract between desktop models and MCP.
- **Tasks:**
  1. **Investigation context service** (`core/services/investigation_context.*`):
     - per target+generation, it holds selected addresses, scan result
       references, breakpoints, observations and provenance;
     - the desktop owns it; it is published to the runtime project so MCP reads
       one source of truth;
     - first slice: "address selected in the UI is readable by
       `cortex_context_get`".
  2. **Typed events.** Extend `events::Event` with `target_id`, `generation`,
     `source` (`ui|mcp|runtime`) and `seq`, and add `ProcessAttached/Detached`,
     `ModuleLoaded/Unloaded`, `AddressSelected`, `BreakpointHit`,
     `EvidenceCreated`, `KnowledgeUpdated` and `AgentAction*`. `Since` returns
     `{events, first_available_id, gap}`. The existing types stay as they are.
  3. **Per-operation target binding.** Every MCP call already carries
     `_cortex_target`. Make the desktop models carry the target id+generation
     they were started for, and refuse completion if the active target changed
     (the PID-reuse test exists in `session_manager_tests`).
  4. **Result contract.** Map `OperationSnapshot` and the route
     `response_contract.h` into one `{status, error, cause, target, generation,
     duration_ms, limits, data, evidence[], permissions_required, rollback}`
     shape. Adopt it first in the scan and debugger models.
- **Validation:**
  - an app-models test (scripted transport): a UI selection is visible through
    the MCP context tool;
  - a multi-target E2E: two fixtures, switching the UI target does not redirect
    an in-flight agent call.
- **Depends on:** nothing.

### Step 3: manual mode

- **Reuse:** almost everything in §3.1 is already functional; the matrix shows
  the scanner and debugger are complete.
- **Gaps:**
  - pointer path classification across sessions (stable / candidate /
    invalidated);
  - a memory-viewer diff between two captures;
  - a one-action jump from a value to the instruction that writes it (it exists
    as several menus: shorten the path);
  - rollback refusal when a patched region changed incompatibly (verify it
    exists, then test it).
- **Tasks:**
  1. Pointer path status with `PointerScanner::Rescan` across target
     generations.
  2. Viewer diff using snapshots.
  3. A "Find what writes" action from the scan results row, using a HW data
     breakpoint, then open the instruction in the disassembler.
  4. An E2E demo script on `test_target` x86 and x64:
     - scan the value;
     - watch it;
     - find what writes it;
     - navigate to the code;
     - save it to the project.
- **Validation:** the demo script in CI (both bitnesses), with screenshots in
  `docs/images`.

### Step 4: static analysis

- **Reuse:** Zydis, `analysis::*`, `mcp_bridge/pe_unwind.h`,
  `test_file_version.h` (SHA-256), symbols.
- **Gaps:**
  - an offline PE model (file, not image);
  - a function list from `.pdata`, exports and symbols before heuristics;
  - an xref index;
  - an interactive CFG with trace coverage;
  - per-instruction comments and renames;
  - pseudocode (provider-backed).
- **Tasks:**
  1. `core/services/pe_file.*`, a portable, bounds-checked parser over bytes:
     headers, sections, imports, exports, relocations, debug directory
     (CodeView GUID/age), TLS, exception directory and SHA-256. It is
     fuzz-tested and kept separate from the loaded-image view.
  2. A function catalog with provenance per entry (`pdata`, `export`, `symbol`,
     `call_target`, `heuristic`), never presented as certain.
  3. An xref index built in the background with `OperationManager` progress.
  4. A CFG view with block coverage from traces, computed off the render thread.
  5. An annotation model keyed by `module+RVA` + binary hash.
  6. A pseudocode view interface, filled in step 8.
- **Validation:** a PE fixture with known imports, exports, functions and
  relocations (built in CI from `test_target` with symbols); golden JSON.

### Step 5: evidence and knowledge

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

- **Reuse:** AI Activity model, workspace registry, address context menu,
  background tasks.
- **Gap:**
  - a workspace that lists investigations, plans, approvals and evidence
    (read from the step 5/6 store);
  - context actions shown only when their capability is present;
  - bidirectional navigation using `module+RVA`.
- **Rule:** no API key in Cortex; the AI side is any MCP client.

### Step 8: Ghidra and the `AnalysisProvider` interface

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

## 7. Risks and open questions

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

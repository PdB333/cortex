# External analysis providers (Ghidra, ILSpy)

Cortex can run external analysis engines on a file and keep their results
next to its own static analysis. The engines are **not linked** into Cortex.
Each one runs as a separate process on a file the person (or the operator of
`cortex.exe mcp`) points at, and its output is turned into typed JSON with
provenance.

| Provider | Engine | Licence | Formats | Features | Status |
|---|---|---|---|---|---|
| `ghidra` | Ghidra `analyzeHeadless` (11.x, JDK 21+) | Apache-2.0 | PE | `analyze`: functions (RVA, name, signature, body ranges, calls), imports, composite types, decompiled C keyed by RVA | validated on PE x64 and x86 (`cortex_test_target`) |
| `ilspy` | ILSpy command line `ilspycmd` (9.x) | MIT | .NET assemblies | `decompile` (the whole assembly as a C# project, or one type), `list_types` | validated on `tests/fixtures/dotnet` |
| `vineflower` | Vineflower 1.11 (`java -jar vineflower.jar`, JDK 11+) | Apache-2.0 | `.jar`, `.class` | `decompile` (Java sources) | validated on `tests/fixtures/java`: the sources recompile and every class has the original's `javap -p` signatures |

Code: `core/providers/` (interface, process runner, job service, the two
providers), `mcp_bridge/analysis_tools.h` (MCP tools and the CLI).

## Configuring an engine

Engines are located by the person or the operator. An MCP client cannot name
an executable or pass engine arguments.

- **Ghidra:** pass `--ghidra-dir <folder>` or set `GHIDRA_INSTALL_DIR`. The
  folder must contain `support/analyzeHeadless[.bat]` and
  `Ghidra/application.properties`, from which the version is read. Ghidra needs
  a JDK (21 for 11.x) in `JAVA_HOME` or `PATH`.
- **ILSpy:** pass `--ilspycmd <path>`, set `CORTEX_ILSPYCMD`, or put
  `ilspycmd` on `PATH` (`dotnet tool install ilspycmd`). You can point at the
  tool's `ilspycmd.dll`; it is then run by `dotnet` (from `--dotnet`,
  `DOTNET_HOST_PATH` or `PATH`). The version comes from `ilspycmd --version`.
- **Vineflower:** pass `--vineflower <vineflower.jar>` or set `CORTEX_VINEFLOWER`; `java` comes from `JAVA_HOME` or
  `PATH` (`--java` on the CLI). The version is read from the jar's `--help` banner.
- **Desktop:** Artifacts › Engines › *Engine locations*. The paths are saved in
  `analysis-engines.json` in the user data folder; an empty field falls back to
  the environment.

## Running

- **Desktop:** inspect a file, then open **Engines** and click *Analyse and
  decompile (Ghidra)* for a PE or *Decompile C# (ILSpy)* for an assembly. The
  job runs in the background and can be cancelled. Once it completes, the list
  shows Ghidra's functions or ILSpy's files, and selecting one shows its code.
  When a Ghidra result exists, the Code tab's function detail has a
  **Ghidra decompilation** button.
- **CLI:**
  ```
  cortex provider list [--ghidra-dir DIR] [--ilspycmd PATH]
  cortex provider run ghidra app.exe --part decompiled --rva 0x14c0
  cortex provider run ilspy Lib.dll --feature list_types --part types
  cortex provider run ilspy pkg.nupkg --child lib/net8.0/Lib.dll --part files --query Actor.cs
  ```
- **MCP** (`cortex.exe mcp --artifact-root DIR --ghidra-dir ... --ilspycmd ...`):
  - `cortex_analysis_providers`: availability, version, licence, formats,
    features and their status;
  - `cortex_analysis_start {provider, feature, path, child?, options?}`: starts
    a job. The file must be inside an `--artifact-root`. A `child` entry
    (ZIP/NuGet/ASAR) is extracted to the cache folder first;
  - `cortex_analysis_get {job_id, part?, rva?, query?, offset?, limit?, max_chars?}`:
    the job state and, once completed, the summary or one result array (paged,
    filtered by RVA or name, long texts clipped);
  - `cortex_analysis_cancel {job_id}` and `cortex_analysis_jobs`.

### Options

- **Ghidra `analyze`:**
  - `decompile_limit` (0–5000, default 200): how many functions to decompile,
    in address order;
  - `decompile`: up to 256 RVAs to decompile instead;
  - `decompile_timeout`: per-function decompiler timeout, 1–300 s (default 30);
  - `timeout`: the whole run, 30–7200 s (default 1800);
  - `max_cpu`: 1–16 (default 2).
- **ILSpy:**
  - `type` (`Namespace.Type`, `decompile` only), checked against a strict
    name pattern so it cannot carry switches;
  - `timeout`: 10–3600 s (default 600).

## Guarantees and how they are enforced

- **Own project, own files.** Ghidra runs in a new temporary project folder
  that Cortex creates, with `-deleteProject`. Cortex writes its post-script
  (`CortexExport.java`, embedded in Cortex) into that folder. A project of the
  person's is never opened. ILSpy writes into a fresh temporary folder. Both
  folders are deleted after the run.
- **Identity.** The file's SHA-256 is computed before the run, and again after
  it; if the file changed, the result is discarded (`file_changed_during_run`).
  Ghidra also reports the SHA-256 of the bytes it imported, which must equal
  Cortex's (`identity_mismatch` otherwise).
- **Mapping to a live module.** Results are keyed by RVA and the file's
  SHA-256. They apply to a loaded module only when that module's file has the
  same SHA-256. This is the rule the Code tab's *Show in live disassembly* and
  `cortex_knowledge_code_check` already follow.
- **Cache.** `<cache>/<provider>/<engine version>/<sha256>/<feature>-<options hash>.json`,
  written atomically. The default folder is `analysis-cache` in the user data
  folder, or the one given by `--analysis-cache`. The same file, engine version
  and options are answered from the cache without starting the engine. A
  damaged entry is ignored and rewritten.
- **Cancellation.** Cancelling a job, its `timeout`, or closing Cortex kills
  the engine's process tree:
  - on Windows, a job object with kill-on-close; Ghidra's launcher starts a
    JVM, which is in the job;
  - elsewhere, the process group.

  A process that survives termination is abandoned after 10 s, not waited
  for.
- **No arguments from MCP.** On Windows, Ghidra's launcher is a `.bat`. Every
  argument passed through `cmd.exe` is refused if it contains `" % ! & | < > ^`
  or a line break, because batch files expand `%1` unquoted. A file whose path
  contains one of these must be copied elsewhere first.
- **Network.** `ilspycmd` is always run with `--disable-updatecheck`, since it
  otherwise contacts NuGet on every run. The `.NET` telemetry opt-out is set
  for its process. Ghidra headless makes no network calls for a local import.
- **Concurrency.** Two jobs run at a time; further jobs queue. The 64 most
  recent jobs are kept in memory; results stay in the cache.
- **Read-only for targets.** No provider touches a target process. The MCP
  tools are read-only, except `cortex_analysis_cancel`, which stops Cortex's
  own engine process. They work without `--allow-writes`/`--allow-state`.

## Validation

`cortex_analysis_provider_tests` always tests the parts that need no engine:
- the process runner: exit code, output, environment, timeout, cancellation
  that also kills a grandchild, `.bat` quoting and refusal;
- result views;
- the job service with a fake provider: cache by SHA-256 surviving a restart,
  queueing, cancellation, a file changed during a run, and cleanup of the work
  folders.

With `GHIDRA_INSTALL_DIR` / `CORTEX_ILSPYCMD` set (and
`CORTEX_REQUIRE_ENGINES=1` in CI), it runs the real engines and checks them
against Cortex's own parsers and the fixture source, not the engines' own
claims.

**Ghidra 11.4.2** on `cortex_test_target` x64 and x86:
- the bytes Ghidra imported have Cortex's SHA-256, and the image base and
  bitness match `ParsePe`;
- every exported function is a Ghidra function at the same RVA (7/7 on both).
  On x64, every start in the exception directory (`.pdata`) is a Ghidra
  function (3817/3817);
- the decompiled `CortexDemoApplyDamage` contains the source's writes to the
  actor (`+ 4` health, `+ 8` armor, `+ 0xc` hits) and the `/ 3` of the
  armour rule;
- some function calls it (call references);
- a second request comes from the cache, `decompile` limits the output to
  the listed RVA, and a running job is cancelled.

**ilspycmd 9.1.0.7988** on `Cortex.Fixture.dll`:
- `list_types` equals the TypeDef table read by Cortex's metadata reader;
- the project contains `Actor.cs` with the original signatures
  (`ApplyDamage(int amount, DamageKind kind)`, generic `Pick<T>` with its
  constraint);
- one type decompiles alone;
- a switch disguised as a type name is refused.

`tests/analysis_e2e.py` drives the same through `cortex.exe mcp`. It also
rebuilds ILSpy's decompiled project with `dotnet build` and compares the
rebuilt assembly's types and member signatures with the original's, as read by
`System.Reflection.Metadata` (`tests/fixtures/dotnet/Oracle`). It also checks
the `--artifact-root` boundary and decompiling from inside the NuGet package.
The `analysis-engines` CI job runs everything with a pinned Ghidra and
ilspycmd on Windows. The GUI suite decompiles the fixture from the Engines tab.

Performance: Ghidra's auto-analysis of the test target, with 400 functions
decompiled, takes about 2 minutes on two cores. ILSpy takes 1–2 s for the
fixture. A cache hit costs one file hash plus reading the cached JSON.

## Limits

- Decompiled code is the engine's reconstruction, not the original source.
  Names such as `param_1` and types are Ghidra's guesses. ILSpy's C# compiles to
  the same API on the fixture, which does not prove identical bodies.
- ILSpy 9.1's project does not compile as written. Its `AssemblyInfo.cs`
  carries `[module: RefSafetyRules(11)]`, which the compiler reserves for
  itself (CS8335). The E2E removes that one line, and reports it, before
  rebuilding; Cortex returns the files unchanged.
- Ghidra is validated on PE x64/x86 only. ELF, Mach-O and other formats Ghidra
  can import are not offered until they are validated.
- Results are the engine's defaults: no custom analyzers, no PDB loading
  options, no user scripts.
- The decompile limit applies in address order. A function beyond it is
  listed but not decompiled; request it with `decompile`.
- One run is one process with its own JVM (no Ghidra server, no reuse between
  runs). The cache is the only speed-up.
- Java (Vineflower/CFR) and live providers (CDP for Node/Electron, .NET live,
  JDWP, DAP) are not implemented yet; see the roadmap.

## Live provider: CDP (Node.js and Electron)

`core/live/cdp.{h,cpp}` connects to the Chrome DevTools Protocol inspector of
a Node.js process (`node --inspect=127.0.0.1:PORT`). Electron exposes the
same inspector: `--inspect` for its main process, `--remote-debugging-port`
for its renderers. The WebSocket client (`core/live/websocket.cpp`, RFC 6455
with the handshake key checked) connects to **127.0.0.1 only**; a target
whose WebSocket URL is not on the inspector's own loopback port is refused.

What it does:
- lists targets (`/json/list`), attaches, and enables the `Debugger` and
  `Runtime` domains;
- lists parsed scripts (URL, lines, source map URL) and returns their source;
- records console messages and uncaught exceptions (bounded);
- sets **log breakpoints** at a 1-based line (and column) of a script URL. At
  each hit it captures the call stack and the top frame's locals, then
  resumes at once:
  - locals come from `Runtime.getProperties` with `ownProperties`, which
    lists accessors without running them; object previews and texts are
    clipped;
  - after `max_hits` (default 20, at most 1000) the breakpoint removes itself.

What it never does:
- `Runtime.evaluate`;
- breakpoint conditions (CDP evaluates them inside the target);
- setting variables or editing scripts;
- `runIfWaitingForDebugger`;
- leaving the program paused. A pause Cortex did not set, such as a
  `debugger;` statement, which pauses once a debugger is enabled, is recorded
  as a *foreign pause* and resumed. The program behaves as it would without a
  debugger, apart from the time spent at a hit.

Authority:
- **MCP:**
  - `cortex_live_targets/_attach/_status/_scripts/_source/_console/_hits` are
    read-only. They work only on ports the operator allows with
    `--inspect-port <port>` (repeatable); any other port is refused
    (`port_not_allowed`);
  - `cortex_live_breakpoint_set/_remove` are target control, like the native
    debugger's breakpoints: they need `mutation_permission` **and** the
    operator's `--allow-writes` (`write_authority_required` otherwise, logged
    as a denial);
  - `cortex_live_detach` removes Cortex's breakpoints and disables the domains.
- **CLI:** `cortex live targets|scripts|watch --port N ...`, on the person's
  own local inspector.
- **Desktop:** AI Investigation › **Live (CDP)**: port, attach, scripts and
  source, log breakpoint on a line, hits with locals. The breakpoint button
  follows the desktop's write permission.

### Validation

`cortex_cdp_tests` checks SHA-1 and Base64 against RFC vectors (including the
RFC 6455 handshake example) and the refusals: port not allowed, nothing
listening. It then starts `tests/fixtures/node-live/app.js` under a real
`node --inspect` and checks it against its source:
- the script and its source are listed;
- a log breakpoint on the health write resolves and records 6 hits. Each hit
  is in `applyDamage` at that line, with `amount = 9`, `absorbed` between 0
  and 3, and the actor's health (from the preview) following the fixture's
  rule from one hit to the next: it drops by `amount - absorbed`, or resets
  to 100;
- the breakpoint removes itself after `max_hits`;
- the `debugger;` statement at every 50th tick is recorded as a foreign pause
  (`reason: other`) and resumed. Console ticks keep increasing, so the
  program never stopped;
- after detaching, a new session sees the process still running.

The test passes natively (Linux, ASan/UBSan) and as a Windows build
(Winsock) under Wine against the same native node. `tests/live_e2e.py` drives
`cortex.exe mcp` and `cortex.exe live watch`:
- the `--inspect-port` boundary;
- breakpoints refused without `--allow-writes`, and without
  `mutation_permission`;
- 5 hits following the rule, the foreign pause, ticks continuing, detach.

The GUI suite drives the Live (CDP) tab when `CORTEX_GUI_SUITE_NODE_FIXTURE`
is set (CI). The Windows CI runs all three against the runner's node.

### Limits (CDP)

- Validated on Node.js 22. **Electron is not validated**: no Electron binary
  runs in CI yet. Its inspector is the same protocol, but its renderer targets
  (`page`) and preload scripts have not been tested.
- Locals are captured for the top frame only, and objects only as V8's
  preview (at most 24 properties, values as text).
- Hits are matched by the inspector's breakpoint id. Source maps are listed,
  but hits are not yet mapped back to original TypeScript positions.
- A process started with `--inspect-brk` stays paused at its first line:
  Cortex does not release it.
- IPC observation in Electron is done by setting log breakpoints in
  `ipcMain`/`ipcRenderer` handlers by hand. There is no dedicated IPC view.

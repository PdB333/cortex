# Cortex

> AI-ready runtime instrumentation for Windows games and native applications.

![Platform](https://img.shields.io/badge/platform-Windows-0078D6)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![Architectures](https://img.shields.io/badge/architectures-x86%20%7C%20x64-555555)
![API](https://img.shields.io/badge/API-local%20REST-2EA44F)

Cortex is a hybrid external host and injectable Windows agent that exposes a
running process through local, authenticated REST APIs. It brings memory scanning, runtime analysis,
debugging, patching, input automation, screenshots, and persistent project
state behind one machine-readable interface designed for tools and AI agents.

The HTTP server binds only to `127.0.0.1`. Protected routes require a
256-bit token generated beside the DLL.

> [!WARNING]
> Use Cortex only with software you own or are authorized to inspect. It is
> intended for offline research, debugging, accessibility, and single-player
> modding. Do not use it to bypass anti-cheat systems or interfere with online
> services.

## Contents

- [Why Cortex?](#why-cortex)
- [Highlights](#highlights)
- [Renderer support](#renderer-support)
- [Requirements](#requirements)
- [Build](#build)
- [External Host](#external-host)
- [Load Cortex](#load-cortex)
- [Verify the connection](#verify-the-connection)
- [Quickstart tutorial](#quickstart-tutorial)
- [Background capture and input](#background-capture-and-input)
- [Debugger with expression captures](#debugger-with-expression-captures)
- [Model Context Protocol (MCP) endpoint](#model-context-protocol-mcp-endpoint)
- [Network hook](#network-hook)
- [Session export](#session-export)
- [Configuration](#configuration)
- [API overview](#api-overview)
- [Persistent projects](#persistent-projects)
- [Action journal and batches](#action-journal-and-batches)
- [Architecture](#architecture)
- [Security model](#security-model)
- [Known limitations](#known-limitations)
- [Dependencies](#dependencies)
- [Contributing](#contributing)
- [For AI agents](#for-ai-agents)
- [License](#license)

## Why Cortex?

Traditional game research often moves between a memory scanner, debugger,
disassembler, scripting tool, and notes. Cortex joins those workflows through
one JSON interface while keeping bulk scans outside the target:

- an agent can discover the complete API through `GET /tools` or
  `GET /openapi.json`;
- a human can follow activity through the Dear ImGui overlay;
- named addresses, pointer paths, freezes, structures, and notes survive
  between sessions;
- writes and patches are tracked by an action journal and can be rolled back;
- batch operations reduce API round trips and support transactional mutation.

```text
AI agent / script / developer tool
                 |
                 | HTTP + JSON + X-Cortex-Token
                 v
   127.0.0.1:6970      127.0.0.1:6969
          |                    |
  Cortex Host outside     Cortex Agent inside
  scans, memory, maps     hooks and live debug
          |                    |
          +------ one runtime workspace ------+
```

## Highlights

| Area | Capabilities |
|---|---|
| Memory | Typed read/write, batches, fills, region enumeration, exact 64-bit integer handling |
| Scanning | External exact/comparative scans, AOB patterns, strings, code caves, intersections, persistent pointer maps |
| Reverse engineering | x86/x64 disassembly, CFGs, xrefs, vtables, PE headers, inferred structures, Ghidra bridge |
| Debugging | Breakpoints, expression-based memory captures at hit, C-like conditions, trigger-to-trace, StackWalk64 + heuristic fallback, paginated logs |
| Runtime instrumentation | Tracked patches, relocation-aware trampolines, freezes, calls, page/allocation watches, targeted rewind |
| Automation | **Background** keyboard/mouse injection (Win32 + DirectInput synthesis), **background** screenshots (any renderer), input sequences, record/replay, window control |
| Networking | ws2_32 recv/send/WSARecv/WSASend interceptor with ring buffer |
| Addressing | Universal `module+RVA` resolver on every route (ASLR-proof) |
| AI integration | Native **Model Context Protocol** (JSON-RPC 2.0) endpoint, auto-derived tool catalog, `/session/export` archive |
| Persistence | Per-process addresses, pointer paths, notes, freezes, and named structures |
| Safety | Loopback-only server (v4 + v6), token authentication, Host/Origin checks, JSON content validation, mutation journal |

## Renderer support

Cortex selects the available renderer hook at runtime.

| Backend | 32-bit | 64-bit |
|---|:---:|:---:|
| Direct3D 8 | Yes | No |
| Direct3D 9 | Yes | Yes |
| Direct3D 10 | Yes | Yes |
| Direct3D 11 | Yes | Yes |
| Direct3D 12 | No | Yes |
| OpenGL | Yes | Yes |
| Native Vulkan | No | No |

The 32-bit D3D8 backend exists for legacy titles. The other backends use
[kiero](https://github.com/Rebzzel/kiero). D3D12 is restricted to x64 because
the ImGui backend requires pointer-sized GPU descriptor handles.

## Requirements

- Windows 7 or newer;
- CMake 3.20 or newer;
- Ninja;
- MinGW-w64 with an x86 or x64 compiler matching the target process;
- administrator privileges only when Windows access rules require them.

> [!IMPORTANT]
> Injector and DLL bitness must match the target process. An x86 build cannot
> be injected into an x64 process, and an x64 build cannot be injected into an
> x86 process.

## Build

Dependencies are pinned in `CMakeLists.txt` and populated with CMake
`FetchContent`.

### 32-bit

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc `
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++
cmake --build build --config Release
```

### 64-bit

```powershell
cmake -S . -B build_x64 -G Ninja `
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc `
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build_x64 --config Release
```

Each build produces:

- `cortex_core.dll` - the DLL loaded into the target process;
- `injector.exe` - the command-line injector;
- `cortex_host.exe` - the external controller and self-pollution-free scanner.

After dependencies have been populated once, configure with
`-DCORTEX_OFFLINE=ON` to prevent CMake from attempting network access.

### Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build_x64 -C Release --output-on-failure
```

The regression suite also covers owned-memory exclusion, relocation-aware
trampolines, multi-instance structure inference, targeted snapshots, and rewind.

## External Host

For memory searches, prefer the external Host. Its buffers live in a separate
process and therefore can never appear in the target's scan results.

```powershell
.\cortex_host.exe --process game.exe --port 6970
# or
.\cortex_host.exe --pid 12344
```

The Host writes `cortex_host.token` and exposes `/health`, `/modules`,
`/memory/regions`, `/memory/read`, `/memory/write`, `/scan/new`, `/scan/next`,
and `/scan/results/{id}`. Use the injected Agent on port 6969 for renderer
hooks, breakpoints, traces, native calls, input, and screenshots. The Host and
Agent executables should match the target bitness for complete address-space
coverage.

## Load Cortex

Choose one loading method.

### Option 1: command-line injector

Start the target process, then run:

```powershell
.\injector.exe <process-name-or-pid> [path-to-cortex_core.dll]
```

Examples:

```powershell
.\injector.exe game.exe
.\injector.exe 12344 .\cortex_core.dll
```

If the DLL path is omitted, the injector looks for `cortex_core.dll` beside
`injector.exe`.

### Option 2: ASI loader

If the target already uses an ASI loader:

1. rename a matching build of `cortex_core.dll` to `cortex.asi`;
2. copy it into the game's ASI or `scripts` directory;
3. launch the game normally.

## Verify the connection

Public health and discovery endpoints do not require a token:

```powershell
Invoke-RestMethod http://127.0.0.1:6969/health
Invoke-RestMethod http://127.0.0.1:6969/tools
```

All other endpoints require the token stored in `cortex.token` beside the DLL:

```powershell
$token = (Get-Content .\cortex.token -Raw).Trim()
$headers = @{ "X-Cortex-Token" = $token }

Invoke-RestMethod `
  -Uri http://127.0.0.1:6969/modules `
  -Headers $headers
```

Read a typed value:

```powershell
$body = @{
  address = "0x400000"
  type = "u32"
} | ConvertTo-Json

Invoke-RestMethod `
  -Method Post `
  -Uri http://127.0.0.1:6969/memory/read `
  -Headers $headers `
  -ContentType "application/json" `
  -Body $body
```

## Quickstart tutorial

A complete first-hit walkthrough using the bundled `test_target` binary. It has
known canary values so you can verify every subsystem end-to-end without a game.

**1. Launch the test target**

```powershell
.\test_target_x64.exe
```

It opens a small blue window and prints its PID + the addresses of its canary
values (`g_cortex_u32 = 0xDEADBEEF`, `g_health = 100`, a frame counter, ...).

**2. Inject Cortex**

```powershell
.\injector_x64.exe test_target_x64.exe
```

**3. Read the token and confirm health**

```powershell
$token = (Get-Content .\cortex.token -Raw).Trim()
$h = @{ "X-Cortex-Token" = $token }
Invoke-RestMethod http://127.0.0.1:6969/health
```

**4. Read a value by `module+RVA`** (survives ASLR/reboots)

```powershell
$body = @{ address = "test_target_x64.exe+0x4000"; type = "u32" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:6969/memory/read `
  -Headers $h -ContentType "application/json" -Body $body
# -> { "value": 3735928559 }  (0xDEADBEEF)
```

**5. Take a background screenshot** (no need to focus the window)

```powershell
Invoke-WebRequest "http://127.0.0.1:6969/screenshot?mode=auto" `
  -Headers $h -OutFile shot.png
```

**6. Send a background input sequence**

```powershell
$seq = @{
  mode = "os"
  steps = @(
    @{ type = "key_tap"; vk = 0x57; hold_ms = 50 },   # W
    @{ type = "delay"; ms = 200 },
    @{ type = "text"; text = "hello" }
  )
} | ConvertTo-Json -Depth 5

Invoke-RestMethod -Method Post -Uri http://127.0.0.1:6969/input/sequence `
  -Headers $h -ContentType "application/json" -Body $seq
```

**7. Set a breakpoint that captures memory at hit**

```powershell
$bp = @{
  address = "test_target_x64.exe+0x1234"
  kind    = "hw"
  capture = @(
    @{ name = "health"; expression = "[rcx+0x18]"; type = "i32" }
  )
} | ConvertTo-Json -Depth 5

$r = Invoke-RestMethod -Method Post -Uri http://127.0.0.1:6969/debug/breakpoint `
  -Headers $h -ContentType "application/json" -Body $bp
# Later: paginated log
Invoke-RestMethod "http://127.0.0.1:6969/debug/breakpoint/$($r.id)/log?limit=50" -Headers $h
```

**8. Export a full session archive**

```powershell
Invoke-RestMethod -Method Post http://127.0.0.1:6969/session/export -Headers $h
# Writes cortex_sessions/session_<UTC>/session.json + screenshot.png
```

**9. Drive Cortex from an MCP client**

```powershell
$req = @{ jsonrpc="2.0"; id=1; method="tools/list" } | ConvertTo-Json
Invoke-RestMethod -Method Post http://127.0.0.1:6969/mcp `
  -Headers $h -ContentType "application/json" -Body $req
```

## Background capture and input

Cortex captures and injects input **without requiring the target window to be
focused**. This works on any renderer (D3D8/9/10/11/12, OpenGL) and even on
minimized windows.

**Screenshots** — `GET /screenshot?mode=<mode>`:

| Mode | Behavior |
|---|---|
| `render` | Grab the hooked backbuffer (highest fidelity, needs the render loop active) |
| `window` | `PrintWindow(PW_RENDERFULLCONTENT)` — works in background, any renderer |
| `last` | Return the last cached frame (zero cost, may be stale) |
| `auto` | Try `render` → `window` → `last`, headers report the source |

The response includes an `X-Cortex-Capture-Source` header so callers know which
path served the image.

**Input** — three transports, use the one that suits the target:

| Route | Transport | Use when |
|---|---|---|
| `POST /input/key`, `/input/mouse_*` | `PostMessage` | Background, Win32-message-driven games |
| `POST /input/sequence` with `mode:"dinput"` | DirectInput vtable hook on `GetDeviceState` | Games that read DirectInput directly (older titles) |
| `POST /input/sequence` with `mode:"game"` | `SendInput` | Foreground, works everywhere |

**Sequences** queue multi-step scripts (`key_tap`, `mouse_click`, `mouse_move`,
`text`, `delay`). Poll `GET /input/sequence/{id}` for status; cancel with
`DELETE /input/sequence/{id}`.

**Record & replay** — `POST /input/record/start` installs low-level keyboard +
mouse hooks on a dedicated pump thread. `POST /input/record/stop` returns the
captured sequence, ready to feed back into `/input/sequence`.

**Window control** — `GET /window`, `POST /window/{focus,restore,minimize,move}`.

## Debugger with expression captures

Every breakpoint can carry a list of memory captures evaluated at each hit.
Expressions support registers, integer literals, `+`/`-`, and pointer-sized
dereferences with `[]`, so complex layouts resolve inline:

```json
{
  "address": "engine.dll+0x2A0F10",
  "kind": "hw",
  "capture": [
    { "name": "hp",       "expression": "[[ecx+0x18]+0x4]", "type": "i32"  },
    { "name": "name",     "expression": "[ecx+0x40]",       "type": "cstring", "size": 32 },
    { "name": "position", "expression": "ecx+0x100",        "type": "bytes",   "size": 12 }
  ]
}
```

Types: `bytes`, `cstring`, `u8/i8/u16/i16/u32/i32/u64/i64`, `float`, `double`.

Hit logs are paginated and non-destructive — the ring buffer reports
`dropped_entries` and `total_hits` so agents never miss activity even if the
consumer is slow:

```powershell
GET /debug/breakpoint/{id}/log?since_seq=0&limit=200
# -> { entries, returned, next_seq, dropped_entries, total_hits }
```

**Trigger → auto trace** — attach a trace template with
`POST /debug/breakpoint/{id}/trigger` and the debugger will start a trace on
every hit, optionally auto-stopping when the current function returns
(`stop_on_return`).

Stack walks fall back through **EBP chain → `StackWalk64` (with FPO) → executable-page
heuristic scan** so broken/optimized prologues still yield a call stack.

## Model Context Protocol (MCP) endpoint

`POST /mcp` speaks **JSON-RPC 2.0** and exposes every Cortex route as a typed
MCP tool. Tools are auto-derived from the same `/tools` manifest — there is no
second registry to keep in sync.

Supported methods:

- `initialize` — returns `protocolVersion: "2024-11-05"`, capabilities, serverInfo.
- `tools/list` — the full catalog as MCP tool descriptors with `inputSchema`.
- `tools/call` — invokes a tool by name; the endpoint loops back through the
  same HTTP server so route handlers stay the single source of behavior.
- `ping` — liveness.
- Batch (JSON array) requests are supported.

Arguments format:

```json
{
  "jsonrpc": "2.0", "id": 42, "method": "tools/call",
  "params": {
    "name": "memory_read",
    "arguments": {
      "address": "test_target_x64.exe+0x4000",
      "type": "u32"
    }
  }
}
```

Path placeholders (`/debug/breakpoint/{id}/log`) go in `_path`, query params in
`_query`:

```json
{
  "name": "debug_breakpoint__id__log",
  "arguments": {
    "_path": { "id": "3" },
    "_query": { "since_seq": "0", "limit": "50" }
  }
}
```

The MCP endpoint still requires `X-Cortex-Token`; a single token authenticates
both the REST and MCP surfaces.

## Network hook

Cortex intercepts `recv`, `send`, `WSARecv`, and `WSASend` on `ws2_32` and
keeps the last 512 events in a ring buffer with a 64-byte hex preview:

```powershell
POST /network/capture   { "enabled": true }
GET  /network/events?limit=100
```

Useful for observing single-player games that talk to auth or telemetry
services, or for reverse-engineering local IPC.

## Session export

`POST /session/export` writes a self-contained archive under
`<module-dir>/cortex_sessions/session_<UTC-timestamp>/`:

- `session.json` — modules, breakpoints (with paginated logs, captures, and
  each address expressed as `module+RVA`), traces metadata, project state.
- `screenshot.png` — a snapshot taken via `mode=auto`.

Archives are directly re-loadable and diffable between runs, which makes bug
reports and reproducers actually reproducible.

## Configuration

Create an optional `cortex.ini` beside the DLL:

```ini
port = 6969
toggle_key = 0x7B
log_console = true
api_token =
```

| Key | Default | Description |
|---|---:|---|
| `port` | `6969` | Local HTTP port |
| `toggle_key` | `0x7B` | Overlay hotkey; `0x7B` is F12 |
| `log_console` | `true` | Open the debug console during initialization |
| `api_token` | empty | Fixed token; when empty, Cortex loads or creates `cortex.token` |

## API overview

Every route uses JSON unless it returns binary data such as a screenshot.
The authoritative route manifest is always available from the running DLL:

```text
GET http://127.0.0.1:6969/tools
GET http://127.0.0.1:6969/openapi.json
```

| Domain | Main routes |
|---|---|
| Discovery | `/status`, `/health`, `/tools`, `/openapi.json`, `/modules` |
| Memory | `/memory/read`, `/memory/write`, `/memory/fill`, `/memory/regions`, `/memory/ownership` |
| Scanning | `/scan/new`, `/scan/next`, `/scan/results/{id}`, `/scan/aob`, `/scan/strings`, `/scan/pointers`, `/scan/pointer_path`, `/scan/intersect`, `/scan/code_caves` |
| Disassembly and analysis | `/disasm`, `/analysis/functions`, `/analysis/cfg`, `/analysis/xrefs`, `/analysis/vtable`, `/analysis/structure`, `/analysis/pe_headers` |
| Debugger | `/debug/breakpoint`, `/debug/paused`, `/debug/registers`, `/trace/*`, `/watch/page_access` |
| Watches | `/watch`, `/watch/events`, `/watch/allocations`, `/watch/page_access` |
| Patching | `/patch/write`, `/patch/assemble`, `/patch/detour`, `/patch/trampoline`, `/patch/alloc_cave` |
| Persistent project | `/project`, `/project/address`, `/project/pointer_path`, `/project/resolve/{name}`, `/project/note` |
| Automation | `/input/*` (key, mouse, sequence, text, record), `/screenshot?mode=`, `/window/*`, `/prompt/*`, `/call/function`, `/freeze`, `/struct/*` |
| Reverse engineering | `/pointermap/*`, `/struct/infer`, `/ghidra/*`, `/snapshot/*`, `/dissect/*` |
| Networking | `/network/capture`, `/network/events` |
| Orchestration | `/batch/run`, `/events`, `/actions`, `/actions/rollback`, `/session/export` |
| MCP | `POST /mcp` (JSON-RPC 2.0: `initialize`, `tools/list`, `tools/call`, `ping`) |

### Scan behavior

Memory scans enumerate committed pages with `VirtualQuery` and read them in
bounded 8 MiB blocks. Failed large reads are retried in 64 KiB windows, and
adjacent blocks overlap so values and patterns crossing a block boundary are
not missed.

Injected scans exclude Cortex's registered module, scratch buffers, pointer
maps, and code caves by default. Set `exclude_cortex=false` only for a focused
diagnostic. The external Host remains the preferred scanner.

Large allocator arenas are scanned rather than skipped. A global AOB scan
covers committed readable image, mapped, and private regions; providing a
`module` deliberately restricts the scan to that executable or DLL.

## Persistent projects

Cortex stores one project per target under:

```text
cortex_projects/<ProcessName>.json
```

A project can contain:

- named addresses;
- stable pointer paths;
- free-form research notes;
- active freezes;
- named structure definitions.

Project files are written atomically and restored when Cortex initializes.
This allows an agent or developer to resume work without rediscovering every
address after each session.

## Action journal and batches

Memory writes, patches, freezes, and other tracked mutations are recorded in
an action journal. Use:

```text
GET  /actions
POST /actions/rollback
POST /actions/clear
```

`POST /batch/run` can execute several operations in one request. Transactional
batches reject unsupported irreversible mutations and roll back supported
writes if a later operation fails.

## Architecture

```text
cortex/
|-- CMakeLists.txt
|-- injector/
|   `-- main.cpp                 standalone DLL injector
|-- host/
|   `-- main.cpp                 external controller and scanner
|-- core/
|   |-- dllmain.cpp             initialization and shutdown
|   |-- config.cpp              cortex.ini parser and defaults
|   |-- log.cpp                 debug console + rotating log file
|   |-- api/                    HTTP server and route domains
|   |-- memory/                 safe reads, writes, and scans
|   |-- process/                module enumeration
|   |-- debugger/               breakpoints and execution control
|   |-- disasm/                 Zydis integration
|   |-- symbols/                DbgHelp / PDB symbol resolution
|   |-- analysis/               CFG, xrefs, vtables, structures
|   |-- dissect/                inferred structure layouts
|   |-- patch/                  patches, assembly, detours, caves
|   |-- pointermap/             persisted cross-session pointer paths
|   |-- timeline/               targeted checkpoints and rewind
|   |-- ghidra/                 runtime import/export bridge
|   |-- watch/                  data, page, and allocation watches
|   |-- project/                persistent per-target state
|   |-- action/                 mutation journal and rollback
|   |-- events/                 Server-Sent Events stream
|   |-- hook/                   renderer and input hooks
|   |-- overlay/                Dear ImGui UI
|   |-- capture/                screenshots (render hook + fallbacks)
|   |-- prompt/                 human-in-the-loop prompt queue
|   |-- freeze/                 periodic value enforcement
|   |-- struct/                 named runtime structures
|   `-- call/                   guarded native function calls
|-- tests/
|   `-- core_tests.cpp
`-- third_party/
```

## Security model

Cortex intentionally treats process memory access as a privileged local
operation:

- the server listens on `127.0.0.1` only;
- every non-public route requires `X-Cortex-Token`;
- token comparison is constant-time;
- non-local Host and Origin values are rejected;
- POST, PUT, and PATCH bodies must use `application/json`;
- responses disable caching and MIME sniffing.

The public routes are limited to `/status`, `/health`, `/tools`, and
`/openapi.json`.

## Known limitations

- Windows only;
- injector and DLL bitness must match the target;
- native Vulkan rendering is not hooked;
- dynamic addresses still require signatures or pointer paths across restarts;
- trampoline creation deliberately rejects an internal relative branch inside
  the overwritten prologue instead of emitting an unsafe gateway;
- arbitrary memory writes, patches, and native calls can crash the target;
- protected or anti-cheat-enabled processes may reject injection and are
  outside the intended scope of this project.

## Dependencies

- [Dear ImGui](https://github.com/ocornut/imgui)
- [MinHook](https://github.com/TsudaKageyu/minhook)
- [cpp-httplib](https://github.com/yhirose/cpp-httplib)
- [nlohmann/json](https://github.com/nlohmann/json)
- [Zydis](https://github.com/zyantific/zydis)
- [stb](https://github.com/nothings/stb)
- [kiero](https://github.com/Rebzzel/kiero)

Dependency revisions are pinned in `CMakeLists.txt` for reproducible x86 and
x64 builds.

## Contributing

When adding an endpoint, keep these three locations synchronized:

1. the implementation in `core/api/routes_*.cpp`;
2. registration in `core/api/server.cpp`;
3. the live manifest in `core/api/routes_status.cpp`.

Before submitting a change, build and test both architectures.

## For AI agents

If you are driving Cortex from an LLM or automated agent, see
[`agent/agents.md`](agent/agents.md) for the connection, authentication, and
workflow conventions the API assumes.

## License

Released under the [MIT License](LICENSE).

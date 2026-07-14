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

- [Highlights](#highlights)
- [Renderer support](#renderer-support)
- [Build](#build)
- [Load Cortex](#load-cortex)
- [Configuration](#configuration)
- [API overview](#api-overview)
- [Persistent projects](#persistent-projects)
- [Architecture](#architecture)
- [Security model](#security-model)
- [Known limitations](#known-limitations)

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
| Debugging | Breakpoints, C-like conditions, detailed access events, conditional traces, coverage and dynamic call graphs |
| Runtime instrumentation | Tracked patches, relocation-aware trampolines, freezes, calls, page/allocation watches, targeted rewind |
| Automation | Keyboard and mouse injection, screenshots, prompts, batch execution, live SSE events |
| Persistence | Per-process addresses, pointer paths, notes, freezes, and named structures |
| Safety | Loopback-only server, token authentication, Host/Origin checks, JSON content validation, mutation journal |

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
- `injector.exe` - the command-line injector.
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
| Automation | `/input/*`, `/screenshot`, `/prompt/*`, `/call/function`, `/freeze`, `/struct/*` |
| Reverse engineering | `/pointermap/*`, `/struct/infer`, `/ghidra/*`, `/snapshot/*`, `/dissect/*` |
| Orchestration | `/batch/run`, `/events`, `/actions`, `/actions/rollback` |

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
|   |-- api/                    HTTP server and route domains
|   |-- memory/                 safe reads, writes, and scans
|   |-- debugger/               breakpoints and execution control
|   |-- disasm/                 Zydis integration
|   |-- analysis/               CFG, xrefs, vtables, structures
|   |-- patch/                  patches, assembly, detours, caves
|   |-- pointermap/             persisted cross-session pointer paths
|   |-- timeline/               targeted checkpoints and rewind
|   |-- ghidra/                 runtime import/export bridge
|   |-- watch/                  data, page, and allocation watches
|   |-- project/                persistent per-target state
|   |-- action/                 mutation journal and rollback
|   |-- hook/                   renderer and input hooks
|   |-- overlay/                Dear ImGui UI
|   |-- capture/                screenshots
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

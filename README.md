# Cortex

> AI-ready runtime instrumentation for Windows games and native applications.

![Platform](https://img.shields.io/badge/platform-Windows-0078D6)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![Architectures](https://img.shields.io/badge/architectures-x86%20%7C%20x64-555555)
![API](https://img.shields.io/badge/API-local%20REST-2EA44F)

Cortex is a hybrid external host and injectable Windows agent that exposes a
running process through a local, authenticated REST + MCP API. Memory
scanning, disassembly, debugging, patching, input automation, screenshots,
OCR, Lua scripting, network capture, crash diagnostics, and persistent project
state — one machine-readable interface designed for tools and AI agents.

The HTTP server binds only to `127.0.0.1`. Protected routes require a
256-bit token generated beside the DLL.

> [!WARNING]
> Use Cortex only with software you own or are authorized to inspect.
> Intended for offline research, debugging, accessibility, and single-player
> modding. Do not use it to bypass anti-cheat or interfere with online services.

## Features

| Area | Highlights |
|---|---|
| Memory | Typed read/write, batches, region enumeration, external scans (exact/comparative/AOB/strings/code caves), persistent pointer maps |
| Reverse engineering | x86/x64 disassembly, CFG, xrefs, vtables, PE headers, inferred structures, Ghidra bridge |
| Debugger | HW/SW breakpoints, expression-based memory captures on hit, `StackWalk64` + heuristic fallback, trigger→auto-trace, paginated logs |
| Diagnostics | Crash dumps, breadcrumbs, registered mods/scopes/values/hooks, PDB/DWARF symbolization, freeze capture and evidence-based analysis |
| Automation | **Background** screenshots (any renderer) & input (Win32 + DirectInput synthesis), sequences, record/replay, window control |
| Networking | ws2_32 recv/send/WSA* interceptor with ring buffer |
| Scripting | Embedded **Lua 5.4** sandbox with `cortex.*` bindings + persisted catalog |
| Vision | **OCR** via Windows.Media.Ocr (Win10+, no bundled binaries) |
| AI integration | Native **MCP** endpoint (JSON-RPC 2.0) + **stdio bridge** for Claude Desktop / Cursor / Cline |
| Addressing | Universal `module+RVA` on every route (ASLR-proof) |
| Persistence | Named addresses, pointer paths, notes, freezes, structures per target |
| Safety | Loopback-only, token auth, Host/Origin checks, mutation journal + rollback |

Renderer hooks: D3D8 (x86), D3D9/10/11 (x86+x64), D3D12 (x64), OpenGL. Vulkan not hooked.

## Quickstart

```powershell
# 1. Launch your target, then inject
.\cortex_host.exe inject game.exe

# 2. Verify with the generated token
$h = @{ "X-Cortex-Token" = (Get-Content .\cortex.token -Raw).Trim() }
Invoke-RestMethod http://127.0.0.1:6969/health

# 3. Read memory by module+RVA (survives ASLR)
$b = @{ address = "game.exe+0x4000"; type = "u32" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:6969/memory/read `
    -Headers $h -ContentType "application/json" -Body $b

# 4. Background screenshot
Invoke-WebRequest "http://127.0.0.1:6969/screenshot?mode=auto" -Headers $h -OutFile shot.png
```

Any other route: consult `GET /tools` (self-documenting manifest) or `GET /openapi.json`.

## One host executable

All user-facing command-line tools are exposed through `cortex_host.exe`:

```text
cortex_host.exe serve ...       external REST controller and scanner
cortex_host.exe inject ...      inject cortex_core.dll
cortex_host.exe diagnose ...    monitor crashes and freezes
cortex_host.exe analyze ...     analyze a crash/freeze directory
cortex_host.exe symbolize ...   resolve PDB or DWARF symbols
cortex_host.exe mcp ...         stdio MCP bridge
```

The historical `cortex_host.exe --pid ...` syntax remains supported and maps to
`cortex_host.exe serve --pid ...`.

## Build

Requires CMake 3.20+, Ninja, MinGW-w64 (x86 and/or x64). Deps are pinned in
`CMakeLists.txt` and fetched via CMake `FetchContent`.

```powershell
# 32-bit
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc `
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++
cmake --build build --config Release

# 64-bit (mirror the above with x86_64-w64-mingw32-*)
```

Each normal build produces `cortex_core.dll` and the single user-facing
`cortex_host.exe`. Test executables are generated only for validation targets.
Run `ctest --test-dir build` to check.

To build only the lightweight host and skip renderer/injected-core dependencies:

```powershell
cmake -S tools/unified_host -B build/unified-host
cmake --build build/unified-host --config Release
```

Use `-DCORTEX_OFFLINE=ON` to prevent network access after the first configure.

## Load Cortex

Two ways:

**Unified host.** Start the process, then run:

```powershell
.\cortex_host.exe inject <name-or-pid> [cortex_core.dll]
```

**ASI loader.** If the target has an ASI loader, rename `cortex_core.dll` to
`cortex.asi` and drop it in the game's `scripts`/ASI directory.

The host and DLL bitness must match the target process for injection and full
CPU-context diagnostics.

## Connecting an MCP client

Cortex exposes MCP two ways:

- **HTTP + JSON-RPC 2.0** on `POST /mcp` (custom agents).
- **stdio bridge** through `cortex_host.exe mcp` for out-of-the-box clients like
  Claude Desktop, Cursor, and Cline. Register it:

  ```json
  {
    "mcpServers": {
      "cortex": {
        "command": "C:/path/cortex_host.exe",
        "args": ["mcp", "--token-file", "C:/path/cortex.token"]
      }
    }
  }
  ```

Tools are auto-derived from the same `/tools` manifest — no second registry.

## Crash and freeze diagnostics

Watch an injected process from outside the game:

```powershell
.\cortex_host.exe diagnose --pid 1234 --heartbeat render --hang-ms 5000
```

Analyze an existing report or symbolize an address later:

```powershell
.\cortex_host.exe analyze C:\path\to\crash_directory
.\cortex_host.exe symbolize --image C:\mods\MyMod.dll --rva 0x1832
```

See [`docs/external-diagnostics.md`](docs/external-diagnostics.md),
[`docs/symbols.md`](docs/symbols.md), and [`docs/hooks.md`](docs/hooks.md).

## Lua scripting

`POST /lua/exec` runs a Lua 5.4 snippet in a fresh sandbox
(`timeout_ms` default 5000). Bindings under `cortex.*`:

```lua
local v = cortex.memory.read("engine.dll+0x1234", "u32")
cortex.memory.write(v_addr, "u32", 42)
cortex.log("hit")     -- to overlay
cortex.sleep(200)
print(cortex.describe(cortex.resolve("engine.dll+0x1234")))
```

Catalog: `GET/POST/DELETE /lua/scripts[/{name}[/run]]` — persisted under
`<module_dir>/cortex_scripts/`.

## OCR

`POST /ocr` with `{image_base64 | image_path, language?}` returns recognized
text plus per-word bounding boxes. Backend: Windows.Media.Ocr via a
PowerShell shim extracted to `%TEMP%` on first use — no bundled Tesseract.
Requires Win10+ with an OCR language pack installed.

Typical loop: `GET /screenshot?mode=auto` → base64 → `POST /ocr`.

## Configuration

Optional `cortex.ini` beside the DLL:

```ini
port = 6969
toggle_key = 0x7B     # overlay hotkey (0x7B = F12)
log_console = true
api_token =           # empty = load/create cortex.token
```

## API overview

| Domain | Main routes |
|---|---|
| Discovery | `/status`, `/health`, `/tools`, `/openapi.json`, `/modules` |
| Memory | `/memory/{read,write,fill,regions,ownership}` |
| Scanning | `/scan/{new,next,results,aob,strings,pointers,pointer_path,intersect,code_caves}` |
| Analysis | `/disasm`, `/analysis/{functions,cfg,xrefs,vtable,structure,pe_headers}` |
| Debugger | `/debug/breakpoint`, `/debug/{paused,registers}`, `/trace/*`, `/watch/*` |
| Patching | `/patch/{write,assemble,detour,trampoline,alloc_cave}` |
| Automation | `/input/*`, `/screenshot?mode=`, `/window/*`, `/prompt/*`, `/call/function`, `/freeze` |
| Scripting | `/lua/exec`, `/lua/scripts[/{name}[/run]]` |
| Vision | `/ocr` |
| Networking | `/network/{capture,events}` |
| Persistence | `/project`, `/project/{address,pointer_path,resolve,note}` |
| Orchestration | `/batch/run`, `/events`, `/actions[/rollback]`, `/session/export` |
| MCP | `POST /mcp` (JSON-RPC 2.0) |

Full route bodies, query params, and examples: `GET /tools`.

### Background capture and input

Screenshots (`GET /screenshot?mode=<render|window|last|auto>`) work in
background via `PrintWindow(PW_RENDERFULLCONTENT)` and a last-frame cache.
Input has three transports:

- `PostMessage` (background, Win32 loops) — `/input/{key,mouse_*,text}`
- DirectInput synthesis (games that read the device directly) —
  `/input/sequence` with `mode:"dinput"`
- `SendInput` (foreground) — `mode:"game"`

Record/replay via `/input/record/{start,stop}`.

### Debugger captures

Each breakpoint may carry a `capture[]` of typed expression reads evaluated
at hit time (registers, `+`/`-`, pointer-sized `[]` deref):

```json
{ "capture": [
    { "name": "hp", "expression": "[[ecx+0x18]+0x4]", "type": "i32" }
] }
```

Hit logs are paginated (`?since_seq=&limit=`) with `dropped_entries` and
`total_hits`. Attach a trigger via `POST /debug/breakpoint/{id}/trigger` to
auto-start a trace on hit (`stop_on_return` optional).

Stack walks combine EBP chain → `StackWalk64` → heuristic exec-page scan.

## Persistence & journaling

- Per-target `cortex_projects/<ProcessName>.json` — named addresses,
  pointer paths, notes, freezes, structures. Loaded at startup.
- Mutation journal: `GET /actions`, `POST /actions/rollback`,
  `POST /actions/clear`.
- `POST /session/export` writes a reproducible archive under
  `cortex_sessions/session_<UTC>/` (state + screenshot).

## Security model

- Server binds `127.0.0.1` only (IPv4 + IPv6 loopback).
- All non-public routes require `X-Cortex-Token` (constant-time compare).
- Host/Origin headers must be loopback; POST/PUT/PATCH require
  `application/json`.
- Public routes: `/status`, `/health`, `/tools`, `/openapi.json`.
- Anti-cheat-protected processes are out of scope.

## Known limitations

- Windows only; host, injector path and DLL bitness must match the target for
  injection and trusted register contexts.
- Native Vulkan rendering is not hooked.
- Arbitrary memory writes, patches, and native calls can crash the target.
- Dynamic addresses require signatures or pointer paths across restarts
  (use `module+RVA` or `/project` for stable ids).
- Production readiness still requires validation inside representative real
  games, launchers, overlays and crash-handler combinations.

## Dependencies

Pinned in `CMakeLists.txt`: [Dear ImGui](https://github.com/ocornut/imgui),
[MinHook](https://github.com/TsudaKageyu/minhook),
[cpp-httplib](https://github.com/yhirose/cpp-httplib),
[nlohmann/json](https://github.com/nlohmann/json),
[Zydis](https://github.com/zyantific/zydis),
[stb](https://github.com/nothings/stb),
[kiero](https://github.com/Rebzzel/kiero),
[Lua 5.4](https://github.com/lua/lua).

## For AI agents

See [`agent/agents.md`](agent/agents.md) for connection, authentication, and
workflow conventions.

## License

[MIT](LICENSE).
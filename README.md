# Cortex

> Runtime observability, instrumentation, and dynamic analysis for software — built around a common target and capability model.

![Release](https://img.shields.io/github/v/release/PdB333/cortex)
![Runtime](https://img.shields.io/badge/runtime-Windows%20x86%20%7C%20x64-0078D6)
![Target model](https://img.shields.io/badge/target%20model-Windows%20%7C%20Linux%20%7C%20PS4-555555)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![API](https://img.shields.io/badge/API-REST%20%2B%20MCP-2EA44F)

Cortex is a runtime analysis platform with an external host, an optional injected Windows agent, and machine-readable REST + MCP APIs. It combines memory inspection, scanning, disassembly, debugging, tracing, reversible patching, input automation, screenshots, OCR, Lua scripting, network observation, crash diagnostics, and persistent analysis state behind one interface designed for humans, tools, and AI agents.

**Current runtime support:** Windows x86 and x64.

**Cross-platform model:** Cortex v0.5.0 introduces platform-neutral `Target`, `Node`, `Backend`, `Catalog`, architecture, and capability contracts for Windows, Linux, and PS4 targets. Linux and PS4 are represented by the common model, but they do **not** yet have production runtime backends.

The local HTTP API is loopback-only; the default endpoint is `http://127.0.0.1:6969`. Protected routes require a generated 256-bit token.

> [!WARNING]
> Use Cortex only with software and systems you own or are authorized to inspect.
> Cortex is intended for debugging, software research, accessibility, testing, diagnostics, and controlled modding. Anti-cheat bypass, unauthorized access, and interference with online services are out of scope.

## Latest release — v0.5.0

[v0.5.0](https://github.com/PdB333/cortex/releases/tag/v0.5.0) is the current public release.

Release archives:

- `cortex-v0.5.0-windows-x64.zip`
- `cortex-v0.5.0-windows-x86.zip`

Both archives are built and validated by the release workflow before publication and contain `cortex_host.exe`, `cortex_core.dll`, `cortex.asi`, the standalone compatibility injector, the matching test target, documentation, SDK files, and agent documentation.

Highlights in v0.5.0:

- hardened request handling, stable response contracts, request IDs, bounded payloads, pagination helpers, and checked memory ranges;
- nested action transactions, rollback guards, mutation journaling, and stronger Lua sandbox/resource limits;
- typed MCP schemas, safe path/query encoding, explicit risk metadata, and stricter loopback bridge policy;
- semantic plan IDs, lifecycle/evidence metadata, timeout/cancellation requirements, and rollback requirements for mutations;
- a generic Target / Node / Backend / Capability architecture with Windows, Linux, PS4, x86, x64, and ARM64 identities;
- a read-only `cortex_host probe` command and OpenGL runtime validation;
- dedicated P1–P4 CI plus full Windows x86/x64 build, injection, E2E, MCP, and release validation.

See [`CHANGELOG.md`](CHANGELOG.md) for the full release history.

## Architecture

Cortex is moving from a Windows-process-centric toolkit to a target-oriented runtime platform.

```text
                         Cortex
                           |
              +------------+------------+
              |                         |
         Controller                  Protocols
      CLI / AI / REST / MCP        Target contract
              |                         |
              +------------+------------+
                           |
                        Catalog
                           |
                 +---------+---------+
                 |                   |
                Nodes              Targets
                 |                   |
         +-------+-------+      +----+----+
         |       |       |      |         |
      Windows   Linux    PS4   process   host / network / ...
         |       |       |
         +-------+-------+
                 |
              Backends
```

The common model deliberately contains no Win32-specific API types. Backends advertise capabilities such as process information, memory observation, scanning, debugging, diagnostics, network observation, and window capture. Clients can therefore adapt to what a target actually supports instead of assuming every operation exists everywhere.

Today, the concrete runtime remains Windows-first. The cross-platform layer is the foundation for future local/remote Nodes, Linux instrumentation, and controlled PS4 adapters.

## Features

| Area | Highlights |
|---|---|
| Target model | Platform-neutral Targets, Nodes, Backends, Catalogs, architectures, and capability sets |
| Memory | Typed read/write, batches, region enumeration, scans (exact/comparative/AOB/strings/code caves), persistent pointer maps |
| Reverse engineering | x86/x64 disassembly, CFG, xrefs, vtables, PE headers, inferred structures, Ghidra bridge |
| Debugger | HW/SW breakpoints, expression-based captures, stack walking, trigger→trace workflows, paginated logs |
| Diagnostics | Crash dumps, breadcrumbs, registered mods/scopes/values/hooks, PDB/DWARF symbolization, freeze/hang capture, evidence-based analysis |
| Automation | Background screenshots and input, sequences, record/replay, window control |
| Networking | ws2_32 recv/send/WSA* observation with bounded event storage |
| Scripting | Embedded Lua 5.4 sandbox with `cortex.*` bindings and persisted script catalog |
| Vision | OCR via Windows.Media.Ocr (Win10+, no bundled OCR engine) |
| AI integration | Native MCP endpoint + stdio bridge + 30 domain-neutral semantic planning tools |
| Addressing | Universal `module+RVA` addressing for ASLR-stable workflows |
| Persistence | Named addresses, pointer paths, notes, freezes, structures, sessions |
| Safety | Loopback-only API, token auth, Host/Origin checks, request limits, mutation journal + rollback |

Renderer hooks currently include D3D8 (x86), D3D9/10/11 (x86+x64), D3D12 (x64), and OpenGL. Vulkan is not hooked.

## Quickstart

```powershell
# 1. Launch an application you are authorized to inspect, then inject Cortex
.\cortex_host.exe inject app.exe

# 2. Load the generated API token
$h = @{ "X-Cortex-Token" = (Get-Content .\cortex.token -Raw).Trim() }

# 3. Check the injected runtime
Invoke-RestMethod http://127.0.0.1:6969/health

# 4. Read memory using module+RVA addressing
$b = @{ address = "app.exe+0x4000"; type = "u32" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri http://127.0.0.1:6969/memory/read `
    -Headers $h -ContentType "application/json" -Body $b

# 5. Capture a screenshot
Invoke-WebRequest "http://127.0.0.1:6969/screenshot?mode=auto" `
    -Headers $h -OutFile shot.png
```

For the complete live API surface, use `GET /tools` or `GET /openapi.json`.

## One host executable

User-facing command-line functionality is exposed through `cortex_host.exe`:

```text
cortex_host.exe serve ...       external REST controller and scanner
cortex_host.exe inject ...      inject cortex_core.dll into an authorized target
cortex_host.exe probe --pid ... read-only external process/runtime probe
cortex_host.exe diagnose ...    monitor crashes, hangs, and heartbeats
cortex_host.exe analyze ...     analyze a crash/hang artifact directory
cortex_host.exe symbolize ...   resolve PDB or DWARF symbols
cortex_host.exe mcp ...         local stdio MCP bridge
```

`probe` is intentionally non-destructive. It reports process liveness, window state, bitness/shared diagnostics information, and heartbeat age without requiring injection or modifying the target.

The historical `cortex_host.exe --pid ...` syntax remains supported and maps to `cortex_host.exe serve --pid ...`.

## Build

Requires CMake 3.20+, Ninja, and MinGW-w64 for Windows x86 and/or x64 builds. Third-party FetchContent dependencies are pinned to immutable revisions.

```powershell
# 32-bit
cmake -S . -B build-x86 -G Ninja `
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc `
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++
cmake --build build-x86 --config Release

# 64-bit
cmake -S . -B build-x64 -G Ninja `
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc `
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build-x64 --config Release
```

A normal Windows build produces `cortex_core.dll` and `cortex_host.exe`. Validation targets are built when testing is enabled.

```powershell
ctest --test-dir build-x64 --output-on-failure
```

To build only the lightweight unified host:

```powershell
cmake -S tools/unified_host -B build/unified-host
cmake --build build/unified-host --config Release
```

Use `-DCORTEX_OFFLINE=ON` to prevent dependency network access after dependencies are already available locally.

## Load Cortex on Windows

**Unified host:**

```powershell
.\cortex_host.exe inject <name-or-pid> [cortex_core.dll]
```

**ASI loader:** if the target has an authorized ASI loader, use the packaged `cortex.asi` or rename `cortex_core.dll` to `cortex.asi` and place it in the loader's expected directory.

The host, injector path, and DLL bitness must match the target process for injection and trusted CPU-context diagnostics.

## MCP and AI integration

Cortex exposes MCP in two ways:

- HTTP + JSON-RPC 2.0 on `POST /mcp`;
- a local stdio bridge through `cortex_host.exe mcp`.

Example client configuration:

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

Primitive MCP tools are derived from the same `/tools` HTTP manifest. In v0.5.0 the MCP layer also provides:

- typed JSON Schemas instead of string-only argument descriptions;
- percent-encoded path and query rendering;
- validation of required query containers and unresolved path placeholders;
- `_cortex` risk metadata for generated primitive tools;
- local-only bridge host/port policy checks.

### Semantic tools

Cortex exposes 30 domain-neutral semantic planning tools for observation, search, tracing, structure inference, hypothesis testing, and reversible experiments.

Semantic plans include a deterministic `plan_id`, lifecycle states, evidence confidence, evidence-state vocabulary, declared primitive steps, and explicit execution requirements. Server-side multi-step execution remains disabled until cancellation, timeout, permission, and rollback semantics are enforced end to end.

See [`agent/semantic-tools.md`](agent/semantic-tools.md) and [`agent/agents.md`](agent/agents.md).

## Crash, hang, and runtime diagnostics

Read-only probe:

```powershell
.\cortex_host.exe probe --pid 1234 --heartbeat render
```

Monitor an injected process externally:

```powershell
.\cortex_host.exe diagnose --pid 1234 --heartbeat render --hang-ms 5000
```

Analyze and symbolize existing artifacts:

```powershell
.\cortex_host.exe analyze C:\path\to\crash_directory
.\cortex_host.exe symbolize --image C:\mods\MyMod.dll --rva 0x1832
```

See [`docs/external-diagnostics.md`](docs/external-diagnostics.md), [`docs/symbols.md`](docs/symbols.md), and [`docs/hooks.md`](docs/hooks.md).

## Lua scripting

`POST /lua/exec` executes Lua 5.4 code in a fresh sandbox. v0.5.0 tightens the sandbox and adds bounded script size, output, read sizes, timeout handling, and mutation journaling for Cortex-backed writes.

```lua
local v = cortex.memory.read("engine.dll+0x1234", "u32")
cortex.log("value=" .. tostring(v))
cortex.sleep(200)
```

Script catalog: `GET/POST/DELETE /lua/scripts[/{name}[/run]]`, persisted under `<module_dir>/cortex_scripts/`.

## OCR

`POST /ocr` with `{image_base64 | image_path, language?}` returns recognized text and per-word bounding boxes. The current Windows backend uses Windows.Media.Ocr through a PowerShell shim and requires an installed OCR language pack.

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

`GET /tools` remains the source of truth for live route bodies, query parameters, descriptions, and generated MCP primitive contracts.

## Background capture and input

Screenshots (`GET /screenshot?mode=<render|window|last|auto>`) support background capture through renderer hooks, `PrintWindow(PW_RENDERFULLCONTENT)`, and a last-frame cache.

Input transports include:

- `PostMessage` for background Win32 loops;
- DirectInput synthesis through `/input/sequence` with `mode:"dinput"`;
- `SendInput` for foreground automation with `mode:"game"`.

Record/replay is available through `/input/record/{start,stop}`.

## Debugger captures

Breakpoints may include expression-based typed captures evaluated on hit:

```json
{
  "capture": [
    { "name": "value", "expression": "[[ecx+0x18]+0x4]", "type": "i32" }
  ]
}
```

Hit logs are paginated and traces can be started automatically from breakpoint triggers. Stack walking combines frame-chain, `StackWalk64`, and heuristic executable-page fallback strategies.

## Persistence and mutation journal

- Per-target project files store named addresses, pointer paths, notes, freezes, and structures.
- `GET /actions` exposes the mutation journal with bounded pagination.
- `POST /actions/rollback` reverts journaled actions supported by the underlying operation.
- `POST /actions/clear` clears journal history.
- `POST /session/export` writes reproducible session artifacts.
- Internal action transactions support nested checkpoints and automatic rollback guards for uncommitted scopes.

## Security and API reliability

- Local API access is loopback-only.
- Protected routes require `X-Cortex-Token` with constant-time token comparison.
- Host/Origin validation rejects non-local origins.
- JSON-modifying routes require the expected content type.
- Request bodies are bounded and malformed/oversized request metadata is rejected early.
- Successful HTTP responses expose correlation IDs through `X-Cortex-Request-Id`; structured errors can include the same ID in JSON.
- Memory operations use checked address-range arithmetic before low-level access.
- The MCP stdio bridge accepts only local loopback endpoints.
- Dependency revisions used by the main and lightweight host builds are pinned.

Public routes include `/status`, `/health`, `/tools`, and `/openapi.json`.

## Validation

Cortex v0.5.0 is validated by multiple independent CI layers rather than a single compile check:

- Windows x86 and x64 full builds;
- CTest on both Windows architectures;
- action transaction and rollback-guard tests;
- request ID, response contract, pagination, and request-limit tests;
- Lua sandbox/resource-limit tests;
- MCP schema, query-container, URI rendering, and bridge-policy tests;
- semantic plan lifecycle and contract tests;
- read-only `cortex_host probe` build validation;
- OpenGL/WGL runtime fixture validation;
- generic Target/Node/Backend/Catalog model tests on Windows x86/x64;
- portable C++17 target-model tests on Linux;
- real Windows DLL injection and semantic MCP calls;
- deterministic Windows E2E scenarios covering API, memory, Lua, MCP, diagnostics, render capture, crash, and hang workflows;
- release packaging validation for both Windows architectures.

The release workflow refuses publication if its build, CTest, live injection/MCP, or packaging stages fail.

## Current scope and roadmap

Implemented runtime today:

- Windows x86/x64 host and injected agent;
- local REST + MCP APIs;
- Windows process instrumentation, diagnostics, render capture, automation, and analysis;
- generic target/capability contracts shared independently of Win32.

Not yet implemented as production runtime backends:

- remote Cortex Node transport and pairing;
- Windows-wide generic process discovery through the new Catalog API;
- Linux process instrumentation/backend;
- multi-machine distributed tracing/observation;
- PS4 runtime instrumentation/backend;
- dedicated D3D8/D3D12 runtime fixtures in the automated matrix;
- native Vulkan renderer hooks.

The intended progression is Windows generic Targets → Cortex Nodes/remote transport → Linux backend → multi-node workflows → experimental controlled PS4 backend.

## Dependencies

Core dependencies include Dear ImGui, MinHook, cpp-httplib, nlohmann/json, Zydis, stb, kiero, and Lua 5.4. FetchContent revisions used by the build are pinned for reproducibility.

See [`docs/p2-dependency-revisions.md`](docs/p2-dependency-revisions.md) for the dependency audit introduced during v0.5.0 hardening.

## Documentation

- [`agent/agents.md`](agent/agents.md) — AI-agent connection and workflow conventions
- [`agent/semantic-tools.md`](agent/semantic-tools.md) — semantic tool catalog and evidence rules
- [`docs/p2-mcp-contracts.md`](docs/p2-mcp-contracts.md) — MCP contract hardening
- [`docs/p3-runtime-validation.md`](docs/p3-runtime-validation.md) — runtime validation work
- [`docs/p4-target-model.md`](docs/p4-target-model.md) — generic target architecture
- [`docs/external-diagnostics.md`](docs/external-diagnostics.md) — external crash/hang diagnostics
- [`docs/symbols.md`](docs/symbols.md) — symbol workflows
- [`README_INSTALL.txt`](README_INSTALL.txt) — packaged Windows installation guide

## License

[MIT](LICENSE).

# Cortex — guide for AI agents

This document is written for an LLM or automated agent driving Cortex. It
explains how to connect and use Cortex productively. The runtime always
self-documents its full route list through the `/tools` manifest (and
`/openapi.json` on the legacy HTTP API); this guide covers the workflow and the
conventions those manifests assume.

## Connecting: MCP over stdio

The supported path is Model Context Protocol over stdio from `cortex.exe`:

```json
{
  "mcpServers": {
    "cortex": { "command": "C:/path/to/cortex.exe", "args": ["mcp"] }
  }
}
```

This is read-only. To let the agent change the target, the person adds `"--allow-writes"` to `args` (`["mcp", "--allow-writes"]`); an agent cannot add it itself.

```text
AI client -> cortex.exe mcp (stdio) -> authenticated Named Pipe -> cortex_core.dll in the target
```

- Without a selector the server starts targetless. Use `cortex_processes`,
  `cortex_attach`, `cortex_targets` and `cortex_detach`; after an attach or a
  detach Cortex sends `notifications/tools/list_changed`, so refresh
  `tools/list`.
- `--pid <pid>` / `--process <name>` attach at startup (repeatable).
- With several attached targets every runtime tool takes a required
  `_cortex_target` (PID, target id or unique process name).
- The default `--tools compact` profile exposes the 30 semantic tools
  (see [`semantic-tools.md`](semantic-tools.md)); `--tools all` adds every
  primitive route.
- Primitive calls put path placeholders in `arguments._path` and query
  parameters in `arguments._query`.
- Batch (array) requests are supported. Diagnostics go to stderr; stdout is
  protocol data only.

Tokens are handled for you: the runtime writes a private
`cortex.mcp.<pid>.token` beside `cortex_core.dll` and `cortex.exe` reads it.
Your MCP calls show up in the desktop's **AI Activity** panel, so the person
at the screen can see what the agent is doing.

See [`docs/mcp.md`](../docs/mcp.md) for protocol versions, routing and
security details.

## Legacy HTTP API

The loopback REST API (`127.0.0.1:6969`, header `X-Cortex-Token` read from
`cortex.token` beside the DLL, plus `POST /mcp`) is compatibility/debug only
and is **off by default**; it starts only with `http_api_enabled=true` in
`cortex.ini`. The external `cortex_host serve` controller (port `6970`) is a
legacy tool. The route names below are the same primitives the MCP catalog
exposes, so everything in this guide applies to both.

## First moves in a new session

1. `health` — confirm the runtime is up.
2. `tools/list` (or the `/tools` manifest) — load the authoritative parameter
   names. **Always trust the exact field names from the manifest**; do not
   guess.
3. `modules` — get the target's module list (name, base, size). Resolve
   addresses relative to a module base, never to a raw absolute from a prior
   session.
4. `project` — recover named addresses, pointer paths, structures, and notes
   saved in earlier sessions. This is your long-term memory.

## Permission to change the target

Attaching grants no write permission. Cortex starts read-only for you: a call
that changes the target (control, mutation, native call) is refused with
`write_authority_required` unless the person who started `cortex.exe mcp` passed
`--allow-writes`, and you cannot turn that on. Writing `mutation_permission=true`
on a call only declares that you mean to change something; it is required, and
it is not sufficient. Do not retry a refused call with different arguments; tell
the person what you needed and why. Every refused attempt is logged.

Without `--allow-writes`, `cortex_attach` only connects to a runtime that is
already loaded in the process; it will not load one. Inside `batch_run`, only
the operations on its allowlist run.

When writes are allowed, every mutation is journaled and can be rolled back.
Use `mutation_permission` only for the specific operation that needs it.

## Core conventions

- All request/response bodies are JSON unless a route returns binary data
  (e.g. `/screenshot` returns `image/png`, or `{image_base64}` with
  `?encoding=base64`).
- Numeric types for memory routes: `i8/i16/i32/i64`, `u8/u16/u32/u64`,
  `float`, `double`, `bytes` (hex), `string`.
- Addresses may be sent as hex strings (`"0x13B7161C"`) or numbers.
- **`module.ext+RVA` form** is accepted everywhere addresses are expected —
  either as a single string (`"godfather2.exe+0x554820"`) or as an object
  (`{"module":"godfather2.exe","rva":"0x554820"}`). Cortex re-resolves the
  base at every call, so persisted scripts survive ASLR across sessions.
- **Prefer `module+RVA`** for any address you plan to store, log, or send
  through `/session/export`. Cortex emits `address_named` fields in this form
  automatically.
- Absolute addresses change on every restart. Persist a **pointer path**, an
  **AOB signature**, or the `module+RVA` form (via `/project`) so an address
  can be re-resolved later.

## Typical reverse-engineering loop

1. **Find a value** — `POST /scan/new` with the current
   value, play the game so the value changes, then `POST /scan/next` with the
   new value / a comparison filter (`increased`, `decreased`, `changed`,
   `bigger`, `smaller`, `between`, deltas). Repeat until few candidates remain.
2. **Confirm** — freeze a candidate (`POST /freeze`) and check the effect
   in-game before trusting it. Convergence across scans is not proof; a
   freeze test is.
3. **Persist** — save the confirmed address under a name with
   `POST /project/address`, or a `POST /project/pointer_path` if it moves.
4. **Understand** — `GET /disasm`, `/analysis/functions`, `/analysis/cfg`,
   `/analysis/xrefs`, `/analysis/vtable`, `/analysis/structure` to map the
   code and structures around it.
5. **Modify** — `POST /memory/write`, `/patch/write`, `/patch/detour`, or
   `POST /call/function`. Every mutation is journaled; undo with
   `POST /actions/rollback`.

## Background capture and input

Cortex works even when the game window is not focused or is minimized.

- **Screenshots** — `GET /screenshot?mode=auto` tries the render hook, then
  `PrintWindow(PW_RENDERFULLCONTENT)`, then a cached last frame. The response
  carries an `X-Cortex-Capture-Source` header telling you which path served it.
- **Input transports** — `os` (Win32 `PostMessage`, works in background),
  `dinput` (DirectInput synthesis for games that read the device directly),
  `game` (`SendInput`, foreground only). Pick per target.
- **Sequences** — `POST /input/sequence` queues multi-step scripts. Poll
  `GET /input/sequence/{id}`, cancel with `DELETE /input/sequence/{id}`.
- **Record/replay** — `POST /input/record/start` then `/stop` returns a
  sequence that can be re-fed into `/input/sequence`.
- **Window control** — `GET /window`, `POST /window/{focus,restore,minimize,move}`.

## Breakpoint captures and traces

Every breakpoint may carry a `capture` array evaluated at each hit. Expressions
support registers, integers, `+`/`-`, and pointer-sized dereferences with `[]`:

```json
{
  "capture": [
    { "name": "hp", "expression": "[[ecx+0x18]+0x4]", "type": "i32" },
    { "name": "name", "expression": "[ecx+0x40]", "type": "cstring", "size": 32 }
  ]
}
```

Hit logs are **paginated** and **non-destructive** — page with
`?since_seq=&limit=` and watch `dropped_entries` + `total_hits`. Attach an
auto-trace with `POST /debug/breakpoint/{id}/trigger` (supports
`stop_on_return` to bound the trace to the current function).

Stack walks combine EBP chain → `StackWalk64` → heuristic exec-page scan, so
optimized prologues still yield a call stack.

## Network capture

`POST /network/capture { enabled: true }` starts intercepting
`recv/send/WSARecv/WSASend` on `ws2_32`. Read the ring buffer with
`GET /network/events?limit=`.

## Session export

`POST /session/export` writes a reproducible archive
(`cortex_sessions/session_<UTC>/session.json` + screenshot) with modules,
breakpoints (with paginated logs, captures, and `module+RVA` addresses), trace
metadata, and project state. Use it as a bug report or a diffable checkpoint.

## Human-in-the-loop

When you need the person at the screen to act or report something the runtime
can't observe, create a prompt; the Cortex desktop shows it as a modal dialog:

- `prompt_value_change` — ask the human to change a named value in the
  application (e.g. set "Health" to 50) and click **Done**; no timer.
- `prompt_timed_test` — ask the human to test something for
  `duration_seconds`, then report a text or number result; the answer control
  stays disabled until the timer runs out.

Then poll `prompt_status` (`GET /prompt/{id}`) until `status` is `answered`. Answering is deliberately not an MCP tool: only the human, through the
desktop, can answer, so an agent cannot confirm its own test. Creating a
prompt fails explicitly when no desktop is attached to present it.

Use `GET /screenshot` to *see* the result of an action — close the loop
visually rather than assuming a write had the intended effect.

## Safety notes

- Arbitrary writes, patches, and native calls can crash the target. Prefer
  reversible operations and keep the action journal in mind.
- `/debug/breakpoint` can freeze the game's threads until you continue
  (`/debug/paused`, then resume). Set the breakpoint `kind` explicitly.
- Batch related operations with `POST /batch/run` to reduce round trips;
  transactional batches roll back supported writes if a later step fails.

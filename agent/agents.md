# Cortex — guide for AI agents

This document is written for an LLM or automated agent driving Cortex. It
explains how to connect, authenticate, and use the API productively. The
running Agent always self-documents its full route list through
`GET /tools` and `GET /openapi.json`; this guide covers the workflow and the
conventions those manifests assume.

## The two endpoints

Cortex has two cooperating servers, both bound to `127.0.0.1` only:

| Component | Default port | Role |
|---|---:|---|
| **Agent** (injected DLL) | `6969` | Renderer hooks, overlay, breakpoints, live debugging, patches, calls, screenshots, prompts |
| **Host** (external exe) | `6970` | Heavy memory scans from outside the process, so scan buffers never pollute the target |

Prefer the **Host** for scanning (`/scan/*`) and the **Agent** for anything
that touches the render loop, execution control, or the human overlay. Both
share one persistent per-target workspace (`project.json`).

## Authentication

Every non-public route requires the header `X-Cortex-Token`.

- The Agent reads or creates `cortex.token` next to the DLL.
- The Host writes `cortex_host.token` next to its exe.

Read that file and send its contents as `X-Cortex-Token` on every request.
The only routes that need no token are `/status`, `/health`, `/tools`, and
`/openapi.json`.

## First moves in a new session

1. `GET /health` — confirm the server is up.
2. `GET /tools` — load the authoritative route manifest with parameter names.
   **Always trust the exact field names from `/tools`**; do not guess.
3. `GET /modules` — get the target's module list (name, base, size). Resolve
   addresses relative to a module base, never to a raw absolute from a prior
   session.
4. `GET /project` — recover named addresses, pointer paths, structures, and
   notes saved in earlier sessions. This is your long-term memory.

## Native MCP transport

Cortex also speaks **Model Context Protocol** over `POST /mcp` (JSON-RPC 2.0).
The tool catalog is auto-derived from `/tools`, so there is no second registry
to maintain.

- `initialize` returns `protocolVersion "2024-11-05"`, capabilities, serverInfo.
- `tools/list` returns MCP tool descriptors with an `inputSchema`.
- `tools/call` loops back through the same HTTP server; put path placeholders
  in `arguments._path` and query params in `arguments._query`.
- Batch (array) requests are supported.

The same `X-Cortex-Token` gates `/mcp` — one token for both surfaces.

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

1. **Find a value** — scan on the Host: `POST /scan/new` with the current
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

When you need the person at the screen to act or report something the API
can't observe, use `POST /prompt`:

- `response_type: "ack"` — a button the human presses when done.
- `response_type: "number"` / `"text"` — a value the human types back.
- `timer_seconds` — show a countdown (e.g. "take damage for 10s and tell me
  the total").

Then poll `GET /prompt/{id}` until `status` is `answered` or `timeout`.

Use `GET /screenshot` to *see* the result of an action — close the loop
visually rather than assuming a write had the intended effect.

## Safety notes

- Arbitrary writes, patches, and native calls can crash the target. Prefer
  reversible operations and keep the action journal in mind.
- `/debug/breakpoint` can freeze the game's threads until you continue
  (`/debug/paused`, then resume). Set the breakpoint `kind` explicitly.
- Batch related operations with `POST /batch/run` to reduce round trips;
  transactional batches roll back supported writes if a later step fails.

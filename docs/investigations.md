# Investigations

An investigation is a journal that holds:
- a testable question;
- a visible plan of tool calls;
- limits and approvals by the person;
- the evidence it produced (`docs/evidence.md`).

It is **not** a second agent. The AI client stays outside Cortex and runs the
tools itself, through the usual MCP gates. Cortex enforces the state machine,
runs the plan's read-only static steps when asked, and refuses to record what
a person has not approved.

Investigations are stored by the MCP host, one JSON file each (schema
`cortex.investigation.v1`), in the user data folder or under
`cortex.exe mcp --investigation-store <dir>`.

## States

```
planned -> running <-> awaiting_approval
running -> completed | failed | inconclusive
planned | running | awaiting_approval -> cancelled
```

- `awaiting_approval` is entered automatically when the next step's effect
  is `target_input` or `mutation`.
- `completed` requires every step finished (done, failed or skipped) and at
  least one cited evidence record that exists in the evidence store.
- `failed` and `inconclusive` need a conclusion text.
- A write after the deadline fails the investigation (`deadline_exceeded`).
- Every transition is appended to the journal with its actor: `agent`,
  `cli`, `desktop` or `cortex`.

## Plan

Each step has:

| Field | Values |
|---|---|
| `tool` | the MCP tool the step uses |
| `capability` | `static`, `decompile` or `live` |
| `effect` | `read`, `state`, `target_input` or `mutation` |
| `runner` | `server` (Cortex runs it: only read-only static artifact tools) or `agent` |
| `hop` | what the step links: for example `JS require -> .node PE` |

Steps run in plan order:
- `cortex_investigation_run_step` runs a server step. Cortex stores the
  output (up to 16 KiB) and its SHA-256.
- `cortex_investigation_step_result` records an agent step. Cortex checks the
  `trial` references in the test runner and the `evidence` references in the
  evidence store; a `tool_call` reference is the agent's account.
- `cortex_investigation_skip` skips a step with a reason.

## Approvals

A step whose effect is `target_input` or `mutation` cannot be recorded until
the person approves it:

```bash
cortex.exe investigation approve <id> <step> --note "D key once"
```

```bash
cortex.exe investigation reject <id> <step>
```

The person can also approve or reject a step in the desktop: **AI
Investigation** workspace, Investigations tab. There the actor is
`desktop`. There is no MCP approval tool, and `mutation_permission` is not
an approval.
Approving a live step counts against `max_live_steps` (default 4).

The approval controls what the investigation records. It does not replace
the gates of the tools themselves: the input runner still needs its launch
profile and `allow_input`, and breakpoints still need `--allow-writes`.

## Templates

| Workflow | Steps |
|---|---|
| `function_static` (`path`, `function`) | catalog, CFG, callers (run by Cortex), then evidence |
| `memory_value` (`value`) | scan, approved input trial, next scan, approved write breakpoint, evidence |
| `electron_ipc_native` (`asar`, `addon`, `export`) | Electron view of the app, the addon's identity, its export in the catalog, the export's CFG (run by Cortex), an approved breakpoint on module+RVA, evidence |

A `custom` workflow passes its own `plan` (1 to 32 steps).

## Tools and command line

- MCP readers: `cortex_investigation_templates`, `_list`, `_get`, `_report`
  (Markdown).
- MCP writers, which need `mutation_permission` and `--allow-state`: `_create`,
  `_start`, `_run_step`, `_step_result`, `_skip`, `_conclude`, `_cancel`.
- CLI: `cortex.exe investigation list | show <id> | report <id> | approve|reject
  <id> <step> [--note] [--store DIR]`.

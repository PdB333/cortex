# Cortex MCP

Cortex exposes Model Context Protocol (MCP) directly from the unified `cortex.exe` product. The recommended transport is stdio. On Windows, Cortex attaches the selected process, activates the architecture-matched runtime when required, and forwards MCP JSON-RPC over the authenticated private named-pipe transport.

There is no separate user-facing MCP executable in the unified product.

## Recommended stdio configuration

The default profile is `compact`, which exposes the semantic Cortex tools rather than every low-level primitive. Configure the AI client once and start Cortex without a target selector:

```json
{
  "mcpServers": {
    "cortex": {
      "command": "C:/path/to/cortex.exe",
      "args": ["mcp"]
    }
  }
}
```

This targetless MCP server remains useful before any process is attached. Its host-control catalog contains:

- `cortex_processes` — refresh and filter the local process list;
- `cortex_attach` — attach one process by PID or unique process name;
- `cortex_detach` — remove one attached process from this MCP connection;
- `cortex_targets` — list the processes currently attached to this MCP server.

After attach or detach, Cortex emits `notifications/tools/list_changed` and advertises `capabilities.tools.listChanged=true`, so MCP clients can refresh the runtime tool catalog without restarting the server or editing their configuration.

`--pid` and `--process` remain compatible startup auto-attach shortcuts:

```text
cortex.exe mcp --pid 1234
cortex.exe mcp --process app.exe
```

Diagnostics are written to stderr so stdout remains MCP protocol data only.

## Dynamic process selection

A typical long-lived session is:

```text
AI client -> cortex.exe mcp
                |
                +-- cortex_processes
                +-- cortex_attach(pid=1234)
                +-- runtime tools
                +-- cortex_detach(_cortex_target=1234)
                +-- cortex_attach(pid=5678)
```

Attaching a target does not enable Mutation. State-changing runtime calls still require their existing explicit `mutation_permission=true` contract.

## Multiple attached targets

One Cortex MCP server can keep more than one process attached at the same time:

```text
cortex.exe mcp --pid 1234 --pid 5678
cortex.exe mcp --process game.exe --process helper.exe
cortex.exe mcp --pid 1234 --process helper.exe
```

Each target gets an independent runtime connection. Requests do **not** switch a shared global process behind the model's back.

When more than one target is attached, Cortex augments every normal tool schema with the required `_cortex_target` argument. It accepts:

- a PID integer;
- a PID string;
- the Cortex target id;
- a unique attached process name.

`tools/list` also exposes the local `cortex_targets` tool. Call it to retrieve the attached target ids, names, PIDs and current liveness before routing work.

Example tool call:

```json
{
  "jsonrpc": "2.0",
  "id": 21,
  "method": "tools/call",
  "params": {
    "name": "capture_runtime_state",
    "arguments": {
      "_cortex_target": 1234,
      "objective": "Capture the current runtime inventory"
    }
  }
}
```

A second request can target PID `5678` concurrently. The `_cortex_target` field is consumed by the stdio router and is not forwarded as a primitive/semantic argument to the target runtime.

With only one attached target, `_cortex_target` remains optional for backwards compatibility.

### Cancellation with multiple targets

Lifecycle and cancellation notifications are broadcast to the attached runtime connections. This keeps `notifications/cancelled` responsive even while independent requests are running against different targets.

## Tool profiles

`cortex.exe mcp` defaults to:

```text
--tools compact
```

The compact profile exposes the semantic tools and hides raw primitives from `tools/list`. Semantic execution may still call an explicitly allowlisted primitive internally when `execute=true` is requested.

Use the complete primitive surface only when an advanced client needs it:

```text
cortex.exe mcp --tools all
```

A startup target can still be supplied, for example `cortex.exe mcp --pid 1234 --tools all`. The target-routing field is added to both compact and full runtime tool catalogs.

## MCP protocol versions

Cortex supports both MCP lifecycle eras:

- modern `2026-07-28`: stateless `server/discover`, per-request protocol metadata, cache hints on tool lists;
- legacy initialize-based clients: `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05`.

JSON-RPC notifications do not normally produce a response. Batch requests are supported; calls in the same batch may route to different `_cortex_target` values.

## Native architecture

Single target:

```text
AI client
   |
 stdio
   |
cortex.exe mcp
   |
authenticated Named Pipe
   |
cortex_core.dll in target
   |
mcp_protocol / mcp_tools
   |
native Cortex services
```

Multiple targets:

```text
                         +-> target A SessionManager -> PayloadClient -> target A runtime
AI client -> cortex.exe -|
                         +-> target B SessionManager -> PayloadClient -> target B runtime

                    tools/call arguments._cortex_target
                                  |
                                  +---- selects the route explicitly
```

The runtime MCP executor and the REST-compatible route layer share the same business handlers. Primitive MCP calls therefore execute native route logic directly rather than making a second loopback HTTP request.

## Named-pipe security and framing

The Windows runtime endpoint is local IPC:

- every injected target generates a private per-process token file named `cortex.mcp.<pid>.token`;
- the endpoint name is derived from a 64-bit hash of that private MCP token, so two targets using the same runtime directory do not collide;
- the raw token is not embedded in the pipe name;
- the complete private MCP token is still included in every native envelope and compared in constant time;
- `cortex.token` remains the separate REST/HTTP compatibility credential;
- remote pipe clients are rejected when supported by the Windows SDK/runtime;
- frames use a 32-bit length prefix and are capped at 16 MiB;
- each stdio connection uses a client session identifier so cancellation request IDs remain scoped.

The endpoint hash is rendezvous information, not authentication. If a second target cannot bind the legacy loopback HTTP port because another Cortex runtime already owns it, its native MCP pipe remains available; native MCP is the primary transport for the unified product.

## Semantic server-side execution

Semantic tools support plan-only calls:

```json
{
  "objective": "Observe a labelled runtime transition"
}
```

A plan-only call returns `status: "plan_ready"` and does not change runtime state.

To execute server-side, provide `execute=true` and an explicit non-empty `steps` array. Cortex intentionally does not infer primitive arguments from the objective.

```json
{
  "objective": "Capture current runtime state",
  "execute": true,
  "timeout_ms": 5000,
  "steps": [
    {"tool": "health", "arguments": {}},
    {"tool": "modules", "arguments": {}}
  ]
}
```

For multi-target MCP, `_cortex_target` belongs at the **top-level semantic tool arguments**, next to `objective`. The whole bounded semantic execution is then routed to that target.

Execution rules:

- maximum 32 primitive steps;
- every primitive must belong to the semantic tool's `_primitives` allowlist;
- step arguments are explicit JSON objects;
- later steps may reference evidence from an earlier step with `$from_step` plus an optional JSON pointer;
- outputs are recorded in the returned `evidence` array;
- a failed primitive stops the sequence;
- control, mutation, and native-call primitives require `mutation_permission=true`;
- active primitives without a known rollback contract are rejected before execution;
- mutations with a supported contract execute inside `action::Transaction`;
- failure, observed cancellation, or observed timeout rolls that transaction back;
- `rollback_on_success=true` can be used for reversible causal experiments that should leave no committed mutation.

Example evidence reference:

```json
{
  "tool": "scan_results",
  "arguments": {
    "_path": {
      "scan_id": {"$from_step": 0, "pointer": "/result/scan_id"}
    }
  }
}
```

### Cancellation and timeout semantics

Cancellation and deadlines are cooperative at orchestration boundaries. Cortex checks them before and after primitive calls. A primitive that is itself blocking cannot be pre-empted in the middle of that call unless that primitive has its own cancellation/timeout mechanism.

Native stdio remains responsive to `notifications/cancelled` because normal requests use independent pipe instances and cancellation is forwarded without waiting for an active request to finish.

## Mutation permission

Attaching a target does not grant permission to modify it. Control, mutation, and native-call operations continue to require explicit `mutation_permission=true` in the relevant MCP call.

This permission remains per operation; multi-target routing does not implicitly enable Mutation on either target.

## Compatibility

The injected runtime can still expose loopback REST and HTTP `/mcp` compatibility surfaces for diagnostics and older integrations. The normal unified-product path is `cortex.exe mcp` over stdio and native IPC.
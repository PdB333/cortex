# Cortex MCP

Cortex exposes Model Context Protocol (MCP) to local AI clients. The recommended transport is stdio, backed by an authenticated local Windows named pipe into the injected runtime. The loopback HTTP `/mcp` endpoint remains available as a compatibility and debugging transport.

## Recommended stdio configuration

The default stdio profile exposes the semantic Cortex tools rather than every low-level primitive. This keeps model context smaller while still allowing bounded server-side orchestration.

```json
{
  "mcpServers": {
    "cortex": {
      "command": "C:/path/to/cortex_host.exe",
      "args": ["mcp", "--process", "app.exe"]
    }
  }
}
```

`--process` (or `--pid`) uses the injector linked into `cortex_host.exe`, waits for the injected runtime and token to become available, then starts the stdio MCP session. Injector diagnostics are redirected to stderr so stdout remains MCP protocol data only.

To connect to a runtime that is already injected:

```json
{
  "mcpServers": {
    "cortex": {
      "command": "C:/path/to/cortex_host.exe",
      "args": ["mcp", "--token-file", "C:/path/to/cortex.token"]
    }
  }
}
```

Native IPC is the default. The previous loopback transport remains available explicitly:

```text
cortex_host mcp --transport http --token-file C:/path/to/cortex.token
```

## Tool profiles

`cortex_host mcp` defaults to:

```text
--tools compact
```

The compact profile exposes the 30 semantic tools and hides raw primitives from `tools/list`. Semantic execution may still call an explicitly allowlisted primitive internally when `execute=true` is requested.

Use the complete primitive surface when debugging Cortex itself or when an advanced client needs direct control:

```text
cortex_host mcp --tools all --token-file C:/path/to/cortex.token
```

The HTTP `/mcp` compatibility endpoint uses the full profile unless `X-Cortex-MCP-Tools: compact` is supplied.

## MCP protocol versions

Cortex supports both MCP lifecycle eras:

- modern `2026-07-28`: stateless `server/discover`, per-request protocol metadata, cache hints on tool lists;
- legacy initialize-based clients: `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05`.

JSON-RPC notifications never produce a response. `notifications/cancelled` is still delivered to the execution layer so an active semantic orchestration can observe cancellation.

## Native architecture

The MCP protocol parser and tool executor are transport-independent:

```text
AI client
   |
 stdio
   |
cortex_host mcp
   |
authenticated Named Pipe
   |
cortex_core.dll
   |
mcp_protocol::Handle
   |
mcp_tools / semantic executor
   |
native route dispatcher
   |
Cortex runtime services
```

The REST and MCP paths share the same route handlers:

```text
                 HTTP REST adapter
                       |
                       v
                 RouteRegistrar
                  /          \
                 /            \
        cpp-httplib route   native route registry
                                  |
                                  v
                             MCP ToolExecutor
```

`RouteRegistrar` mirrors each business handler into both cpp-httplib and the in-process native registry. Primitive MCP calls therefore execute the existing route logic directly in memory rather than performing a loopback HTTP request.

HTTP `/mcp` is now only a transport adapter:

```text
HTTP /mcp -> mcp_protocol -> mcp_tools -> native route dispatcher
```

There is no second HTTP call from the MCP executor back into Cortex REST.

## Named-pipe security and framing

The native endpoint is local Windows IPC:

- endpoint name is derived from a 64-bit hash of the existing random Cortex token;
- the raw token is never embedded in the pipe name;
- the complete token is still included in every native envelope and compared in constant time;
- remote pipe clients are rejected when supported by the Windows SDK/runtime;
- frames use a 32-bit length prefix and are capped at 16 MiB;
- each stdio bridge creates a session identifier used to isolate cancellation request IDs.

The endpoint hash is rendezvous information, not authentication.

## Semantic server-side execution

Semantic tools still support plan-only calls:

```json
{
  "objective": "Observe a labelled runtime transition"
}
```

A plan-only call returns `status: "plan_ready"` and does not change runtime state.

To execute server-side, the client must provide `execute=true` and an explicit non-empty `steps` array. Cortex intentionally does not infer primitive arguments from the objective.

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

Example reference:

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

Cancellation and deadlines are cooperative at orchestration boundaries. Cortex checks them before and after primitive calls. A primitive that is itself blocking cannot currently be pre-empted in the middle of that call unless that primitive has its own cancellation/timeout mechanism.

Native stdio remains responsive to `notifications/cancelled` because normal requests use independent pipe instances and the bridge dispatches cancellation without waiting for an active request to complete.

The HTTP compatibility transport should provide a stable `X-Cortex-MCP-Session` value when cancellation is required. `cortex_host mcp --transport http` does this automatically.

## Compatibility

REST remains loopback-only and unchanged as a public Cortex surface. Existing clients can continue to call REST or HTTP `/mcp`. The native stdio path is an additional local transport that removes HTTP from the normal MCP execution chain without duplicating route business logic.

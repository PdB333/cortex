# Cortex MCP

Cortex exposes Model Context Protocol (MCP) to local AI clients while keeping the runtime analysis surface loopback-only.

## Recommended stdio configuration

The default stdio profile exposes the semantic Cortex tools rather than every low-level primitive. This keeps the model context smaller and encourages evidence-oriented workflows.

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

`--process` (or `--pid`) uses the injector already linked into `cortex_host.exe`, waits for the injected runtime and token to become available, then starts the stdio MCP session. Injector diagnostics are redirected to stderr so stdout remains MCP protocol data only.

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

## Tool profiles

`cortex_host mcp` defaults to:

```text
--tools compact
```

The compact profile exposes the semantic planning catalog. It intentionally hides raw REST-derived primitives from `tools/list` so clients do not have to load the entire low-level API into model context.

Use the complete primitive surface when debugging Cortex itself or when an advanced client needs direct control:

```text
cortex_host mcp --tools all --token-file C:/path/to/cortex.token
```

The HTTP `/mcp` compatibility endpoint continues to use the full profile unless `X-Cortex-MCP-Tools: compact` is supplied.

## MCP protocol versions

Cortex supports both MCP lifecycle eras:

- modern `2026-07-28`: stateless `server/discover`, per-request protocol metadata, cache hints on tool lists;
- legacy initialize-based clients: `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05`.

JSON-RPC notifications never produce a response. This includes legacy `notifications/initialized`, cancellation notifications, and unknown notifications.

## Architecture

The protocol parser is transport-independent:

```text
JSON-RPC message
      |
      v
mcp_protocol::Handle
      |
      +---- tool catalog callback
      |
      +---- tool execution callback
```

The current runtime execution path remains:

```text
AI client
   |
 stdio
   |
cortex_host mcp
   |
loopback HTTP /mcp
   |
MCP protocol core
   |
loopback REST route dispatch
   |
Cortex runtime
```

This is intentionally documented rather than hidden. The stdio-facing client uses stdio, but the injected runtime still uses its existing loopback HTTP API internally.

## Next architectural step: native ToolExecutor

Removing HTTP from the internal MCP path correctly requires extracting route business logic from the `httplib` handlers into transport-neutral operations. The target architecture is:

```text
                 REST adapter
                      |
MCP stdio ------> ToolExecutor <------ MCP HTTP adapter
                      |
                 Cortex services
```

Each route should become a thin adapter responsible for HTTP parsing/status serialization only. `ToolExecutor` should own typed operation lookup, validation, capability checks, mutation permission, cancellation, deadlines, and result contracts.

A named pipe or other local IPC transport is useful only after this extraction. Adding a pipe that simply calls HTTP again would change the diagram without removing the architectural dependency.

## Semantic execution

Semantic tools currently return deterministic plans. `execute=true` remains disabled until Cortex can bind semantic plan steps to validated primitive arguments and enforce all of the following end-to-end:

- cancellation;
- deadlines/timeouts;
- capability checks;
- explicit mutation permission;
- action checkpoints and rollback for mutations;
- evidence capture and result attribution.

Until those guarantees exist, returning a plan is safer and more reproducible than pretending a generic multi-step executor can infer missing primitive arguments.

# P2 MCP contract hardening

P2 hardens Cortex's MCP transport and schema surface and makes the user-facing
`cortex_host mcp` entry point local-only.

## Enforced in P2

- `cortex_host mcp` accepts only loopback targets (`127.0.0.1`, `localhost`, `::1`, `[::1]`) and a
  valid TCP port before forwarding to the stdio bridge.
- `mcp_bridge/policy.h` defines the local-host, port, and bounded-message policy and has x86/x64
  contract tests.
- Third-party `FetchContent` inputs in the root build are pinned to immutable commit SHAs rather
  than mutable version/tag references.
- The native `/mcp` router now uses `core/api/mcp_contract.h` for both URI rendering and primitive
  tool schemas.
- MCP path placeholders are mandatory, path/query components are percent-encoded, and the rendered
  request target is bounded before loopback dispatch.
- Primitive MCP tools expose useful JSON Schema types for booleans, integers, arrays, and addresses
  instead of coercing every manifest property to `string`.
- Path parameters are represented as a required `_path` object with explicit required properties;
  query parameters remain under `_query` with their own typed properties.
- Primitive tools expose `_cortex.risk` plus `mutation_permission_required` metadata derived from
  the shared risk classifier. This is descriptive metadata only; it is not an authorization grant.
- The P2 CI matrix builds the complete Cortex tree on x86 and x64, runs CTest, and separately checks
  MCP URI/schema rules, bridge policy, and the inherited P1 request/pagination contracts.

## Contract source

`core/api/mcp_contract.h` is the shared implementation used by the native MCP router:

1. `RenderPath` requires every declared `{placeholder}`, percent-encodes path/query components, and
   bounds the final request target.
2. `SchemaForProperty` maps the legacy `/tools` manifest descriptions to JSON Schema types while
   accepting explicit schema objects for future manifest entries.
3. `ClassifyTool` assigns the descriptive risk classes `observe`, `analyze`, `control`, `mutate`, and
   `native_call`.

The `/tools` manifest remains the primitive operation registry. MCP continues to dispatch primitive
calls back to the same authenticated loopback REST routes, so behavior is not duplicated in a
second implementation.

## Security boundary

The URI/schema contract and `_cortex.risk` metadata are not authentication boundaries and do not
by themselves grant or revoke mutation capabilities. Token authentication, Host/Origin validation,
and loopback HTTP binding remain the server security boundary.

A future capability layer can enforce `observe` / `control` / `mutate` / `native_call` policy using
the same risk classifier, but that enforcement should be implemented and tested independently from
URI encoding and schema generation.

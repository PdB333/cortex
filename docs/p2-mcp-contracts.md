# P2 MCP contract hardening

P2 introduces reusable transport/schema primitives around Cortex's MCP integration and makes the
user-facing `cortex_host mcp` entry point local-only. This document distinguishes guarantees that
are enforced by executable code from helpers that are intentionally staged for later integration.

## Enforced in P2

- `cortex_host mcp` accepts only loopback targets (`127.0.0.1`, `localhost`, `::1`, `[::1]`) and a
  valid TCP port before forwarding to the stdio bridge.
- `mcp_bridge/policy.h` defines the local-host, port, and bounded-message policy and has x86/x64
  contract tests.
- Third-party `FetchContent` inputs in the root build are pinned to immutable commit SHAs rather
  than mutable version/tag references.
- The P2 CI matrix builds the complete Cortex tree on x86 and x64, runs CTest, and separately checks
  MCP URI/schema rules, bridge policy, and the inherited P1 request/pagination contracts.

## Prepared for route integration

`core/api/mcp_contract.h` provides two pieces that can be used by the native `/mcp` router:

1. `RenderPath` requires every declared `{placeholder}`, percent-encodes path/query components, and
   bounds the final request target to the HTTP URI limit.
2. `SchemaForProperty` maps the legacy `/tools` manifest descriptions to useful JSON Schema types
   (`boolean`, `integer`, arrays, and integer-or-string address values), while accepting explicit
   schema objects for future manifest entries.

The existing `core/api/routes_mcp.cpp` still owns primitive tool dispatch. Until it is migrated to
these helpers, its legacy string-only schemas and legacy path renderer remain the runtime behavior.
Do not remove `tests/mcp_schema_tests.cpp` when performing that migration: wire the router to the
helper, then add an end-to-end `tools/list` / `tools/call` regression test before declaring the
migration complete.

## Non-goals of this P2 slice

The URI/schema helper is not an authentication boundary and does not grant or revoke mutation
capabilities. Token authentication and loopback HTTP binding remain the server security boundary.
Any future MCP permission tiers should be implemented explicitly and tested independently from URI
encoding and schema generation.

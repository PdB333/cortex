# Cortex documentation

This directory contains both user-facing documentation for the unified Cortex application and lower-level implementation/compatibility notes.

## User documentation

- [Getting started](getting-started.md) — download/extract the preview, attach a target, scan, use Addresses, understand Mutation and run the manual validation checklist.
- [Illustrated UI walkthrough](ui-walkthrough.md) — step-by-step guide with real Cortex screenshots: target selection, Scanner, Addresses, Memory, Disassembly, Debugger, RE and Settings.
- [French illustrated walkthrough](ui-walkthrough-fr.md) — French translation.
- [Cortex UI guide](ui-guide.md) — complete workspace map, address context menu, shortcuts, Settings and Mutation behavior.

## Product architecture

- [Unified application architecture](unified-app-architecture.md) — one-product `cortex.exe` architecture, runtime layering, MCP and instrumentation boundaries.
- [Target model](p4-target-model.md) — common target/backend model used for Windows and future platform parity.

## Runtime and protocol internals

- [MCP internals](mcp.md)
- [P2 MCP contracts](p2-mcp-contracts.md)
- [P2 dependency revisions](p2-dependency-revisions.md)
- [Runtime validation](p3-runtime-validation.md)
- [External diagnostics](external-diagnostics.md)

## Feature internals / compatibility

- [Hooks](hooks.md)
- [Symbols](symbols.md)
- [Mod SDK compatibility](mod-sdk.md)

Some lower-level documents preserve historical compatibility paths. For current user-facing behavior, prefer the root [README](../README.md), [Getting started](getting-started.md) and [Cortex UI guide](ui-guide.md).
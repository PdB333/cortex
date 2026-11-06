# Cortex documentation

This directory contains both user-facing documentation for the unified Cortex application and lower-level implementation/compatibility notes.

## User documentation

- [Getting started](getting-started.md) — download/extract the release, attach a target, scan, use Addresses, understand write permission and run the manual validation checklist.
- [Illustrated UI walkthrough](ui-walkthrough.md) — step-by-step guide with real Cortex screenshots: target selection, value scan, Addresses, Memory viewer, Disassembler, Debugger, RE and Settings.
- [French illustrated walkthrough](ui-walkthrough-fr.md) — French translation.
- [Demonstration A: manual reverse engineering](demo-manual.md) — find a value, watch it, find what writes it, navigate the code and keep it across sessions, on Cortex's test program.
- [Cortex UI guide](ui-guide.md) — complete workspace map, address context menu, shortcuts, Settings and write-permission behavior.
- [Analysis providers](analysis-providers.md) — Ghidra, ILSpy and Vineflower, and the live providers (CDP, debugpy, netcoredbg, JDWP): setup, guarantees, validation and limits.
- [Evidence](evidence.md) and [investigations](investigations.md) — records with provenance and status; supervised multi-step investigations.
- [Launching and testing programs](automated-tests.md) — operator launch profiles and the bounded trial runner.
- [Known limitations](limitations.md) — the documented limits, collected in one page.
- [Third-party notices](third-party.md) — licences of the libraries and of the optional external engines.
- [Contributing](../CONTRIBUTING.md) and [security policy](../SECURITY.md).
- [Releasing](releasing.md): how the release workflow builds, checks and (only after approval) publishes the archive.

## Product architecture

- [Cortex 2.0 roadmap and audit](cortex-2-roadmap.md) — capability matrix of the current code, baseline benchmarks, step 1 fixes and the plan for the following steps.
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
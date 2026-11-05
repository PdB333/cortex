# Known limitations

This page collects, in one place, the limits that are documented across
Cortex's docs. Each item links to the page that explains it and is kept up to
date there. If something here contradicts that page, the page wins: please
report the mismatch.

## Scope

- **Windows targets only.** The runtime and debugger attach to Windows x86 and
  x64 processes. ELF is supported for static analysis through Ghidra only;
  Mach-O is not validated ([analysis providers](analysis-providers.md)).
- **No protection bypass.** Cortex does not get around anti-cheat, DRM,
  licence checks, obfuscation, anti-debugging or an Electron app's disabled
  inspector. A program that refuses a debugger stays refused
  ([SECURITY.md](../SECURITY.md)).
- **No autonomous gameplay.** The launcher is limited to profiles the operator
  configures, and the test runner sends bounded window-message input. Not
  every program handles window messages
  ([automated tests](automated-tests.md)).
- **Cortex 2.0 is unreleased.** Static analysis of files, evidence,
  investigations, external engines and live providers exist on the
  development branch and in CI artifacts, not in the v1.0.1 release
  ([roadmap](cortex-2-roadmap.md)).

## Security model

- **The pipe token is a file.** `cortex.mcp.<pid>.token` is readable by the
  Windows user who runs Cortex. An agent that also has a shell or file access
  as that user can talk to the runtime directly, outside every MCP rule
  ([MCP internals](mcp.md)).
- **"Read-only" is not "no effect".** Read-only debugger tools attach a
  debugger, which suspends the target's threads while it handles events. Each
  attach and detach is logged and shown in AI Activity
  ([MCP internals](mcp.md)).
- **`mutation_permission` is the agent's stated intent, not human consent.**
  The authority is the operator's `--allow-writes` or the desktop switch.
  A time-limited authority granted from the desktop is planned, not built
  ([MCP internals](mcp.md)).

## Analysis results

- **Observations are not explanations.** Matching bytes, a breakpoint hit or a
  nearest symbol do not prove what a function means or where it starts. A
  hardware write trap reports the instruction *after* the write
  ([agent guide](../agent/agents.md)).
- **External engines run with their defaults.** Ghidra uses no custom
  analyzers, no PDB options and no user scripts. ILSpy's output can need small
  fixes before it rebuilds: for example, its `RefSafetyRules` attribute is
  rejected by `csc` ([analysis providers](analysis-providers.md)).
- **One Ghidra run is one JVM.** The cache is the only speed-up.

## Live providers

- **Electron is not validated.** CDP is validated on Node.js 22. Electron
  renderer targets and preload scripts have not been tested, and IPC is
  observed only by setting log breakpoints in handlers by hand.
- **Values are shallow.** Only the top frame's locals are captured, one level
  deep, as the debugger's own preview.
- **debugpy runs `__repr__`.** Values come from `repr()`, so a user class's
  `__repr__` runs in the target.
- **netcoredbg on Windows needs stdio mode.** The 3.1.2 `--server` mode reads
  no command after `initialize` on Windows, so Cortex starts the adapter
  itself (`--netcoredbg`). The server mode serves one session and exits.
- **No .NET dump analysis.**
- **Source maps are listed, but hits are not mapped back** to original
  TypeScript positions.
- **`--inspect-brk` processes stay paused** at their first line: Cortex does
  not release them.

The pages linked above give details: [analysis providers](analysis-providers.md),
in the "Limits", "Limits (CDP)" and "Limits (DAP)" sections.

## Not implemented yet

- Python bytecode and PyInstaller, Unity/IL2CPP, a native Java class-file
  parser, and WebAssembly (roadmap priorities 2 and 3).
- Android APK, Mach-O and other platforms (planned, not scheduled).
- Fuzzing of the live protocol decoders (WebSocket, DAP, JDWP) and of the
  project, knowledge and evidence files. The file parsers are fuzzed
  ([tests/fuzz](../tests/fuzz/README.md)).
- The robustness end-to-end tests (target killed mid-operation, module
  unloaded, provider crashed): step 9 ([roadmap](cortex-2-roadmap.md)).

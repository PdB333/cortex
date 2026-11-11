# Known limitations

This page collects, in one place, the limits that are documented across
Cortex's docs. Each item links to the page that explains it and is kept up to
date there. If something here contradicts that page, the page wins: please
report the mismatch.

## PS5 connector

The experimental [PS5 connector](ps5-connector.md) now uses our own Cortex
Remote protocol and native Cortex agent with mutual TLS. The client/server
have localhost tests, and the PS5 ELF is cross-linked against SDK v0.45.
No physical console or firmware is validated. Memory reads temporarily stop
the target only with agent permission. There is no breakpoint channel or
automatic console setup; this is separate from the Windows target backend.

## Scope

- **Native injected runtime and debugger: Windows targets only.** The runtime and debugger attach to Windows x86 and
  x64 processes. ELF32/64 has a native static reader in the wave-1 worktree; Ghidra remains optional for decompilation;
  Mach-O is not validated ([analysis providers](analysis-providers.md)).
- **No protection bypass.** Cortex does not get around anti-cheat, DRM,
  licence checks, obfuscation, anti-debugging or an Electron app's disabled
  inspector. A program that refuses a debugger stays refused
  ([SECURITY.md](../SECURITY.md)).
- **No autonomous gameplay.** The launcher is limited to profiles the operator
  configures, and the test runner sends bounded window-message input. Not
  every program handles window messages
  ([automated tests](automated-tests.md)).
- **Formats.** Cortex reads PE, .NET, Electron ASAR, JavaScript/TypeScript,
  ZIP/JAR. The wave-1 worktree additionally reads native ELF, PDB/MSF7, CAB, CFB/MSI and APPX/MSIX metadata with explicit partial-support limits. Other formats are planned, by priority, in
  the [formats roadmap](formats-roadmap.md).

- **WinRT .winmd support is structural and partial.** Cortex inventories CLR tables and tdWindowsRuntime types, but does not validate all WinRT-specific GuidAttribute/VersionAttribute and type projection rules. The local fixture is synthetic and is not a full SDK conformance oracle. No WinRT activation or code execution.
- **Windows driver files (.sys) are classified heuristically.** A Native-subsystem PE plus a .sys extension, WDM flag or recognized kernel import is labelled a driver candidate. The entry RVA is not a verified DriverEntry symbol, and Cortex does not check whether Windows would load the file.

## Wave-1 reader limits

See [implementation and verification](formats-wave1-implementation.md). PDB is an MSF7/common-record reader, not full recursive type resolution. CAB expansion supports stored/MSZIP only, not LZX/Quantum or split cabinets. MSI is database inspection only, with bounded codepage support. APPX block hashes are compared to an untrusted manifest; signatures and publisher trust are not validated. The portable static target is not a Linux process debugger or Android port.

## Additional archive, certificate and package reader limits

See [additional implementation scope](formats-next-implementation.md). These are
local development capabilities, not promises about the published release.

- TAR links/sparse entries and ar thin references are not followed. gzip has
  bounded expansion and CRC/ISIZE checks; 7z/xz/zstd/bzip2 decompression is not
  added. Debian/npm files are inspected, never installed or executed.
- Certificate and CMS fields are structural metadata. Signature, chain and
  revocation verification are **not implemented**. Matching subject/issuer
  names is not evidence of a self-signature. Catalog CTL content is exposed
  raw; individual catalog entries are not decoded.
- COM support is MSFT only. SLTG, full imported-type resolution, defaults and
  codepage projection remain unsupported. No target COM object is activated.
- CRX proofs and claimed extension IDs are not authenticated. XPI/VSIX/WAR/EAR
  manifests are inventories, not full schema validation. JMOD is a ZIP
  container view, not Java bytecode/module analysis. Python code is never
  imported. Squirrel URLs/hashes are claims, never fetched or applied.

## Installation and updates

- **Not code-signed yet.** SmartScreen warns on the first run of the
  installer and of `cortex.exe`. An update is accepted only when its SHA-256
  matches the release's `SHA256SUMS.txt`; that proves the file is the one
  published, not who built it ([installing and updating](setup-and-updates.md)).
- **AI client setup** edits the user-level configuration of eight known
  clients. A file that is not plain JSON (comments) is left untouched, and
  per-project configurations are not changed.
- **An installed copy is updated only when nothing else uses its files** (an
  MCP server started from it, a target with its runtime loaded); Cortex lists
  them instead of forcing.

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
- Fuzzing of the project store (Windows-only). File parsers, live protocol
  framing, evidence and investigation records and knowledge input are
  fuzzed ([tests/fuzz](../tests/fuzz/README.md)).
- The robustness end-to-end tests (target killed mid-operation, module
  unloaded, provider crashed): step 9 ([roadmap](cortex-2-roadmap.md)).

# Formats wave 1: Windows-first implementation plan

**Base:** Cortex 2.1 development branch. **Scope:** authorized static inspection.
Providers consume bounded artifact ByteSource objects. They never execute
an artifact, load a driver, invoke a script, decrypt protected assets,
unpack commercial protectors or bypass access controls.

Roughly 200 format/target families is a roadmap, not a current support claim
or a requirement to implement 200 independent parsers. Separate three
capabilities: static parsing, decompilation and live debugging.

## Architecture and acceptance contract

Reuse core/artifacts Provider, Registry, ByteSource, InspectOptions and
the cortex.artifact.v1 schema. A format must be reachable through the
Artifacts GUI, cortex.exe inspect CLI and read-only cortex_artifact_*
MCP tools. Preserve SHA-256 identity, bounded output and file access roots.

A delivered format needs:
1. Structural detection beyond a filename where feasible, typed output and
   documented unsupported cases.
2. A deterministic fixture in tests/fixtures (prefer a source generator)
   checked against an independent reference wherever possible.
3. Positive, negative, truncated, oversized and corrupted-input tests.
4. A libFuzzer harness (or integration in existing artifact/PE harnesses)
   and a valid corpus seed. Keep every crash regression.
5. Windows x64/x86 builds and tests, native ASan/UBSan for portable parsers
   and real Windows acceptance where relevant. Wine alone is insufficient.
6. Update docs/formats-roadmap.md, docs/limitations.md and the changelog.
   Mark capabilities validated only for exercised fixtures.

For containers, enforce nesting, output-size and decompression-ratio limits.
Never run installers, CustomAction tables, pickles, scripts or binaries.
Encrypted and commercially protected content remains unreadable.

## Dependencies and delivery order

| Item | Target | Foundation and acceptance |
|---|---|---|
| 1A-1 | Windows driver .sys | DONE IN LOCAL SOURCE: PE Native subsystem, candidate entry RVA, WDM flag, direct kernel imports; x86/x64 fixtures, corruptions and fuzz seeds. Does not prove a DriverEntry symbol or Windows loader acceptance |
| 1A-2 | WinRT .winmd | Structural parser implemented locally using CLR metadata and tdWindowsRuntime flags; SDK-based conformance, projection rules and independent oracle remain open |
| 1A-3 | COM .tlb | Build bounded MSFT/SLTG type-library parsers; no COM activation; compare GUIDs and signatures with a trusted dumper |
| 1B | PDB/MSF | Read MSF block stream, DBI/TPI/IPI, symbols and line records; cross-check CodeView GUID+age; block-map overflow tests |
| 1C-1 | CAB | Bounded catalog/extraction for explicitly supported compression types, with corrupt and bomb fixtures |
| 1C-2 | MSI | Reuse CAB; implement OLE compound storage and MSI table inventory; CustomAction entries are displayed but never invoked |
| 1C-3 | MSIX, APPX, bundles | Reuse ZIP; validate manifest, identity and signature metadata without executing package contents |
| 1D | Squirrel, NSIS, Inno, WiX Burn, InstallShield | Build recognizable package inventories with per-variant fixtures; mark partial until coverage proven |
| 1E | Native ELF32/64 | Header, segments, sections, symbols, relocations, notes, PIE/NX/RELRO and validation on owned ELF fixtures; Ghidra remains optional for decompilation |
| 1F | WebView2/Chromium live | Reuse CDP but restrict to explicitly authorized localhost debug endpoints, no evaluation, explicit permissions for breakpoints; real Windows fixture |

Remaining P1 dependencies include X.509/PKCS#7 and .cat catalogs, .inf
inventory, and .NET ReadyToRun/bundles. PE and minidump features already
present are not reimplemented. Packer signatures are detection only.

## Multi-platform path

**Linux static first:** factor Windows-only CLI/file dependencies behind
portable interfaces, add native ELF parsing, and build the analysis-only
toolchain on Linux. Then port ImGui to GLFW/SDL + OpenGL.

**Linux live second:** implement a separate target::Backend for process,
module and thread enumeration; process_vm_readv for reads and explicitly
authorized process_vm_writev/ptrace for state changes and breakpoints.
Provide real owned-process fixtures and do not reuse the Windows injected
runtime DLL as a Linux implementation.

**Android static:** APK, AXML, resources.arsc, DEX. **Android live:** Cortex
remains on the PC and uses ADB for only owned/authorized debuggable apps
or personally controlled rooted devices. No Android bypass or jailbreak.

## Status

Delivered in v2.1.0. What was verified, and how, is in
[formats-wave1-implementation.md](formats-wave1-implementation.md); the
remaining limits are in [limitations](limitations.md).

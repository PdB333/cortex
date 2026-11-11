# Formats wave 1: Windows-first implementation plan

**Base:** Cortex 2.1 development branch. **Scope:** authorized static inspection.
Providers consume bounded artifact ByteSource objects. They never execute
an artifact, load a driver, invoke a script, decrypt protected assets,
unpack commercial protectors or bypass access controls. The earlier blanket PS5 exclusion is superseded by the operator-requested
[PS5 connector](ps5-connector.md): our own Cortex Remote protocol, PC client
and native agent, separate from static format readers.

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
or personally controlled rooted devices. No Android bypass or jailbreak. PS5 debug-server connectivity is tracked
separately in [PS5 connector](ps5-connector.md).

## Current state of this worktree

- [x] Windows driver candidate parser in existing PE provider (source).
- [x] Deterministic x86/x64 synthetic PE fixtures and corpus wiring (source).
- [ ] Full unit, fuzz, Windows x64/x86 and UI/CLI/MCP CI validation.
- [x] Structural WinMD recognition, type/method inventory and negative fixture tests (local only).
- [ ] Independent SDK oracle, full WinMD semantic conformance and COM typelibs.
- [ ] PDB/MSF.
- [ ] CAB, MSI, MSIX and installer families.
- [ ] Native ELF.
- [ ] WebView2 live.
- [ ] Linux static/runtime and Android static/ADB.

No future item is shipped merely because this plan lists it.

## Implemented increment and remaining acceptance

The following now have code, fixtures, sanitizer tests in independent Windows x64/x86 builds and CLI/MCP checks: PDB/MSF7, native ELF, stored/MSZIP CAB, CFB/MSI and APPX/MSIX/bundles. See [measured scope and remaining work](formats-wave1-implementation.md). This supersedes the original unchecked implementation notes above without treating those broader lots as fully finished.

- [x] Native PDB/MSF7 common-record reader; deeper type semantics remain open.
- [x] Native ELF32/64 reader; compressed sections and sectionless dynsym remain open.
- [x] CAB stored/MSZIP, including cross-block history; LZX remains open.
- [x] CFB v3/v4 and MSI database inventory, including embedded CAB traversal.
- [x] APPX/MSIX and bundle metadata, plus untrusted block-map comparison.
- [x] Platform-neutral static build and CLI; Windows x64/x86 sanitizer-tested.
- [ ] Linux CI execution; native coverage-guided fuzzing on Linux.
- [ ] COM typelibs, certificate/catalog readers, installer families, WebView2 live.
- [ ] Full Linux GUI/process backend and Android/ADB.

## Additional reader increment

See [formats-next-implementation.md](formats-next-implementation.md). MSFT COM
metadata, structural X.509/PEM/CMS, TAR/gzip/ar plus Debian/npm containers,
CRX/XPI/VSIX, Java package wrappers, Python ZIP metadata and Squirrel RELEASES
now have integrated code and local checks. SLTG, full certificate/catalog
verification and the installer executable payload readers remain open. Earlier
unchecked rows describe the broader end goals, not a claim that no code exists.

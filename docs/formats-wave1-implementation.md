# Wave 1: local implementation and verification

Status: **released in v2.1.0**. This document records
what has code and test evidence; it is not a claim that all wave 1 or 200 formats
are supported. The original .sys and structural .winmd additions are preserved.

## Implemented readers

| Family | Implementation | Verified scope | Important exclusions |
|---|---|---|---|
| Native PDB/MSF 7 | `core/artifacts/pdb_provider.cpp` | Stream allocation/bounds, GUID+age, DBI modules/source files, public/data/private procedures, common TPI/IPI names and sizes, C13 lines | MSF 2, portable PDB, full recursive type semantics, C11, OMAP remapping, source-server commands |
| Native ELF | `core/artifacts/elf_provider.cpp` | 32/64-bit, both byte orders, extended counts, sections/program headers, symbols, REL/RELA, dynamic dependencies, GNU notes/build IDs, hardening metadata | Compressed sections, packed RELR, reconstruction of sectionless dynamic symbols, live Linux backend |
| CAB | `core/artifacts/cab_provider.cpp` | File/folder inventory, CFDATA checksums, stored and MSZIP, dictionary across blocks, safe in-memory child access | LZX, Quantum, continued/spanned files, reserved CFDATA extensions, installer execution |
| OLE/CFB and MSI | `compound_reader.cpp`, `compound_provider.cpp`, `msi_tables.cpp` | CFB v3/v4 FAT/MiniFAT, nested storage, MSI string pool/schema/column-major tables, signed integers/nulls, CustomAction as data, nested CAB | Full MSI semantics, transforms, unsupported codepages, package/signature trust, installation or COM activation |
| APPX/MSIX and bundles | `appx_metadata.cpp`, `xml_reader.h`, existing ZIP provider | Namespace-aware manifest metadata, identity/apps/dependencies/capabilities, bundle members, SHA-256 64-KiB block comparison | Full manifest validation, signature/trust validation, compressed block/local-header verification, package installation |

All new readers are integrated with the shared Registry and existing GUI,
CLI and MCP entry points. A filename does not imply a supported capability.
Capabilities remain **experimental** where coverage is partial. The generic
Artifacts GUI displays their JSON trees; no bespoke debugger view was added.

## Independent fixtures

PDB includes a Clang/lld 17 generated PDB alongside deterministic MSF7
fixtures. ELF includes the existing GCC-built `actor.elf` and 32/64 LE/BE
synthetics. CAB includes an archive produced by Microsoft's `makecab` plus
Python-zlib blocks that require previous-block history. MSI was authored
with the Windows database APIs only: it was never installed or registered,
and its `NeverRun` CustomAction was never invoked. APPX fixtures use Python's
ElementTree, zipfile and hashlib as independent metadata/hash producers.

Each family has positive/negative bounds tests, malformed inputs, generators,
a corpus and the `fuzz_wave1` entry point. Fixtures are data, never executed.

## Verification actually performed on 2026-11-07

- Windows x64 and **independent x86 executables**, built with LLVM-MinGW
  Clang 23.1.5, ASan and UBSan: four CTest suites pass on each architecture.
  LeakSanitizer is disabled on Windows; UBSan halts on findings.
- Main GNU build: artifact and .NET tests pass. The full ImGui executable
  builds, and `--smoke-test` / `--window-smoke-test` return zero.
- `tests/formats_wave1_e2e.py`: **38 passing CLI/MCP checks**, no write
  authority and no target process. Includes MSI -> CAB -> file content,
  identity hashes, absent/outside root refusal and altered APPX block hashes.
- Existing `tests/artifact_mcp_e2e.py` passes (PE, ASAR, source maps, bounds).
- The dedicated harness was run on **37 seed files plus 20,000 deterministic
  mutations** in an ASan/UBSan x64 build. All 20,000 complete after the ZIP
  regression fix. This is **not coverage-guided libFuzzer execution**.
- `tests/fuzz/regressions/fuzz_wave1/zip-invalid-utf8-name` retains the exact
  mutated file that originally threw during JSON serialization. ZIP names
  are now safe UTF-8 for display; invalid-name entries cannot be opened.

The Windows MinGW compiler rejects libFuzzer coverage flags for its target.
Native Linux Clang/libFuzzer build instructions and a workflow are provided,
but **no Linux CI result is claimed** until that workflow actually runs.
The GUI smoke test is not a complete visual acceptance test for every format.

## Portable static foundation

`tools/static` builds the shared file parsers and `cortex-inspect` without
Windows process APIs, the GUI, injection, Zydis or external decompilers. It
has been compiled and tested on Windows x64/x86, and on Linux by the
*Portable static formats* workflow. This is **not** a Linux desktop,
Linux live debugger or Android port.

## Work still open

The [next increment](formats-next-implementation.md) now supplies MSFT typelibs,
structural certificates/CMS, Squirrel RELEASES and additional archive/package
readers. Still open: SLTG/full COM type resolution, deeper PDB and PE/PDB
cross-checking, CAB LZX, more MSI variants, cryptographic package/catalog trust,
NSIS/Inno/InstallShield/WiX payloads, WebView2 live, Linux GUI/process and
Android/ADB. WinMD still needs an SDK conformance oracle.

No release, remote CI, commit, push or merge is implied by these local results.

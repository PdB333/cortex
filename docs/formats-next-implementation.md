# Additional artifact readers: implementation and verification

**Status: released in v2.1.0.** This
increment builds on [wave 1](formats-wave1-implementation.md). It adds useful
readable containers and package metadata, not a claim of 200 fully supported
formats. Parsing, decompilation and live debugging are distinct capabilities.
The new standalone providers declare **experimental** capabilities. The existing
ZIP provider's validated status describes its container reader; the newly added
package manifests are partial metadata readers, not full schema validators.

## Coverage and exclusions

| Family | Implemented | Deliberate limits |
|---|---|---|
| TAR | v7/ustar headers and checksums, PAX metadata, GNU long names, bounded ordinary-file children | Links and unsafe/duplicate names cannot be opened; sparse entries unsupported; inspection stops at the first valid end marker |
| gzip | RFC 1952 members, optional headers/header CRC, concatenated members, DEFLATE, CRC32/ISIZE; one `payload` child | At most 64 MiB compressed input and 64 MiB expanded total; no extraction to a path supplied by the file |
| ar / .a / .lib | Common/GNU long-name table and BSD inline-name containers, bounded members | Symbol indexes listed but not decoded; no COFF object-symbol parser; thin archives rejected without reading external files |
| Debian .deb | Version/control/data member inventory, nested tar.gz traversal through shared readers | No install, script execution or network; xz/zstd/bzip2 members are listed but have no new decompressor |
| npm .tgz | gzip -> TAR -> package/package.json, name/version/dependencies/scripts inventory | Scripts are text, never run; no install, dependency resolution or fetch |
| X.509 DER/PEM | Issuer/subject, serial, validity strings, algorithm OIDs, SPKI and certificate SHA-256, extension inventory; PEM child certificates | No signature/chain/revocation verification; not a complete PKIX or validity-policy checker; unsupported BER/high-tag encodings refused |
| PKCS#7/CMS | SignedData certificate and signer inventory, encapsulated content and certificate children | Definite-length DER only; signatures are always explicitly unverified; no certificate-store import or URL fetch |
| Catalog .cat | CTL content-type OID recognition within SignedData, public certificates and raw content child | Not a Windows catalog-entry decoder or catalog signature verifier; the test catalog has synthetic content under the CTL OID |
| COM .tlb (MSFT) | Library/type GUIDs, names, versions, interfaces/enums, methods, parameters, common VARTYPE descriptors | No SLTG, COM activation or registration; imported references, defaults/custom attributes and full array/type semantics unresolved; non-ASCII ANSI bytes escaped |
| XPI / WebExtension ZIP | Manifest v2/v3 identity, permissions, background/content-script entry points, Gecko metadata | Structural inventory, not full browser schema validation or signature verification; no extension installation |
| CRX2/CRX3 | Wrapper bounds, public-key/signature metadata, CRX3 protobuf proof and claimed-ID fields, nested ZIP | No signature authentication; CRX fixtures contain intentionally invalid placeholder proofs and are never called signed/valid |
| VSIX | Namespace-aware 2011 manifest identity/assets/dependencies, VS Code package.json entry points and engine requirements | Newer/legacy schemas may be unsupported; no extension installation or script execution |
| WAR / EAR | Deployment-descriptor metadata and nested ZIP members | Inventory only; no Java class execution or bytecode analysis added here |
| JMOD 1.0 | Header and nested ZIP containing classes/resources | Container only; no semantic module-info.class decoder or jimage support |
| Python zipapp .pyz / wheel | Entry-point inventory; wheel METADATA identity and dependency fields | No import, evaluation, Python bytecode parser or package installation; not a complete email-header parser |
| Squirrel RELEASES | SHA-1/name-or-URL/size claims, full/delta metadata and comments | No downloads, hash attestation, delta application, installation or Squirrel setup.exe unpacking |

## Shared entry points

The four new implementation modules are `archive_provider.cpp`,
`certificate_provider.cpp`, `typelib_provider.cpp` and `package_metadata.cpp`.
The Registry registers seven providers; the ZIP provider also calls bounded
package-specific metadata readers. The same source files are linked by the
main build, ImGui application, unified host, portable static target and fuzz
build. No external analyzer is needed at runtime for these readers.

The existing GUI Artifacts tree displays the structured results and container
children. Home, File > Open and Artifacts > Browse now share categorized file
filters including archives, type libraries, extension packages and public
certificates. Selecting a file for analysis does not execute its contents.

Examples using the existing `child` syntax:

```text
sample.deb ! data.tar.gz ! payload ! usr/share/cortex/hello.txt
package.tgz ! payload ! package/package.json
sample3.crx ! archive.zip ! manifest.json
bundle.pem ! certificate-0.der
signed.p7m ! content.der
sample.ear ! sample.war ! WEB-INF/web.xml
jdk.jmod ! archive.zip ! classes/module-info.class
```

The exclamation marks separate child names, not filesystem extraction paths.
MCP artifact-root authorization and read-only tool classification are preserved.
Certificates, scripts, URLs and update manifests are untrusted data.

## Fixture provenance

- `tests/fixtures/make_format_next_fixtures.py` uses Python tarfile/gzip/zlib
  and OpenSSL. OpenSSL signs owned test content; the generator independently
  reads it back. Private keys exist only in a temporary directory and are
  not shipped. Public fixture fingerprints and OpenSSL output are retained.
- `tests/fixtures/typelib/make_fixture.cpp` calls Windows **ICreateTypeLib2**
  to create both SYS_WIN32 and SYS_WIN64 metadata files with known interfaces,
  methods and parameters. No target type library is activated or registered.
- `tests/fixtures/make_package_next_fixtures.py` uses zipfile for owned
  extension/package fixtures, including negative descriptors and CRX headers.
  Placeholder CRX signatures are intentionally not authenticated.
- `tests/fixtures/package-next/make_jmod_fixture.py` compiles an owned Java
  module using the installed JDK, creates a real JMOD and records `jmod
  describe`. The compiled Java code is never executed.

Generators and external reference utilities are test dependencies only. The
production parsers use C++17 and the repository's existing JSON/ZIP helpers.

## Verification recorded locally

- `cortex_format_next_tests`: **159 checks, zero failures**, in both Windows
  x64 and independent x86 builds under AddressSanitizer and
  UndefinedBehaviorSanitizer. LeakSanitizer is disabled on Windows.
- Six CTest entries pass on each architecture, including prior artifact,
  .NET, PDB/ELF/CAB/MSI/APPX tests. The new crash-replay directory is initially
  empty; a zero-input replay is not counted as a fuzz campaign.
- The dedicated `fuzz_format_next` harness completed **48 seeds and 20,000
  deterministic mutations on each architecture**, x64 and x86, under ASan/UBSan
  without a reported failure. Both use the same reproducible mutation seed;
  this is not 40,000 unique cases or coverage-guided libFuzzer execution.
- `tests/formats_next_e2e.py`: **129 passing checks across 24 fixture cases**,
  using the actual rebuilt `cortex.exe` CLI and MCP. Checks cover all new
  families, nested provider transitions, exact bytes and SHA-256 identity,
  missing/outside-root refusal, unsafe child refusal, bounds and explicit
  unverified-signature results. No write authority was granted; all 56
  analyzed fixture files retained their original hashes.
- The earlier `tests/formats_wave1_e2e.py` continues to pass after integration.
  Full application build, `--smoke-test` and `--window-smoke-test` pass.
  The GNU root artifact/.NET suites also pass with the built PE fixture.
  Final logs are retained under `build/format-next-verification` in the
  development checkout.
- Post-review integration checks also pass: the root `cortex_setup_tests`
  target links the new ZIP-package metadata dependency and passes its setup/
  update regression suite. The .NET fixture generator preserves the intended
  UTF-8 filename; seven regenerated ZIP members match the reference byte for
  byte, and regenerated ZIP/WinMD fixtures pass .NET tests on both sanitizer
  architectures. Existing DLL/oracle/NuGet fixtures were reused; this is not
  a fresh .NET SDK build.

The GUI build/smoke checks are not comprehensive interactive visual acceptance
for every new format. Native Linux Clang/libFuzzer jobs are wired in
`formats-portable.yml`, but no remote CI or Linux execution is claimed here.
The portable file readers are not a Linux live-debugger or Android port.

## Primary format references

- TAR/ustar/PAX: GNU tar format documentation and Python tarfile-produced fixtures.
- gzip: RFC 1952 (https://www.rfc-editor.org/rfc/rfc1952).
- X.509: RFC 5280 (https://www.rfc-editor.org/rfc/rfc5280); CMS: RFC 5652
  (https://www.rfc-editor.org/rfc/rfc5652); PEM: RFC 7468
  (https://www.rfc-editor.org/rfc/rfc7468).
- MSFT type-library layout: Wine oleaut32 `typelib.h` and `typelib.c`, with
  independent Windows ICreateTypeLib2 fixtures. No COM runtime API is used
  to parse analyzed files.
- CRX3: Chromium `components/crx_file/crx3.proto`; VSIX: Microsoft's VSIX 2.0
  schema reference; WebExtension: manifest.json specification.
- Squirrel: the upstream Squirrel.Windows `ReleaseEntry.cs` line parser;
  Java JMOD: the JDK jmod command reference and generated fixture.

## Still open

NSIS/Inno/InstallShield/WiX payload readers, complete catalogue entries and
cryptographic trust validation, SLTG and full COM type resolution, CAB LZX,
7z/xz/zstd/bzip2 readers, standalone Java class analysis, Python bytecode,
WebView2 runtime testing and Linux/Android process backends remain separate
work. Nothing in this document claims those are implemented.

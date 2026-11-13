# Known limitations

This page collects, in one place, the limits that are documented across
Cortex's docs. Each item links to the page that explains it and is kept up to
date there. If something here contradicts that page, the page wins: please
report the mismatch.

## Scope

- **Native injected runtime and debugger: Windows targets only.** The runtime and debugger attach to Windows x86 and
  x64 processes. ELF and Mach-O are read statically by built-in readers; decompiling
  them needs the optional Ghidra engine ([analysis providers](analysis-providers.md)).
- **No protection bypass.** Cortex does not get around anti-cheat, DRM,
  licence checks, obfuscation, anti-debugging or an Electron app's disabled
  inspector. A program that refuses a debugger stays refused
  ([SECURITY.md](../SECURITY.md)).
- **No autonomous gameplay.** The launcher is limited to profiles the operator
  configures, and the test runner sends bounded window-message input. Not
  every program handles window messages
  ([automated tests](automated-tests.md)).
- **Formats.** Cortex reads more than fifty formats with built-in parsers
  (see the [CHANGELOG](../CHANGELOG.md) and the [formats roadmap](formats-roadmap.md)).
  They are read, never run: no unpacking, no decryption, no unpickling, no
  loading into an interpreter. The limits of each reader are listed below;
  the others planned are in the roadmap, by priority.

- **WinRT .winmd support is structural and partial.** Cortex inventories CLR tables and tdWindowsRuntime types, but does not validate all WinRT-specific GuidAttribute/VersionAttribute and type projection rules. The fixture is synthetic and is not a full SDK conformance oracle. No WinRT activation or code execution.
- **Windows driver files (.sys) are classified heuristically.** A Native-subsystem PE plus a .sys extension, WDM flag or recognized kernel import is labelled a driver candidate. The entry RVA is not a verified DriverEntry symbol, and Cortex does not check whether Windows would load the file.

## Windows format reader limits

See [implementation and verification](formats-wave1-implementation.md). PDB is an MSF7/common-record reader, not full recursive type resolution. CAB expansion supports stored/MSZIP only, not LZX/Quantum or split cabinets. MSI is database inspection only, with bounded codepage support. APPX block hashes are compared to an untrusted manifest; signatures and publisher trust are not validated. The portable static target is not a Linux process debugger or Android port.

## Additional archive, certificate and package reader limits

See [additional implementation scope](formats-next-implementation.md).

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

## Binaries, bytecode, data, captures, documents and game archives

Every reader below was checked against an independent reference on fixtures
produced by the real toolchain (`tests/format_more_tests.cpp`, generators in
`tests/fixtures/format-more`), and fuzzed with ASan and UBSan.

- **Mach-O:** validated on clang objects (x86_64, arm64) and a fat container.
  Linked executables and dylibs, and code-signature blobs (parsed, never
  verified) have not been checked against a signed Apple binary.
- **Go:** build info is read for Go 1.18 and later (earlier versions store
  pointers, reported as unresolved). Function names come from the pclntab
  (layouts of Go 1.2 to 1.24); validated on Go 1.24 for Linux and Windows.
- **Python `.pyc`:** headers and code-object trees for 2.7 and 3.x;
  instruction listing for CPython 3.13 only (its opcode table is generated
  from CPython 3.13). No decompilation.
- **Java `.class`:** constant pool, members and instructions; no
  verification and no decompilation (use the Vineflower engine).
- **Lua:** official Lua 5.1-5.4 chunks and LuaJIT 2.0/2.1 prototypes; game
  engines that modify the Lua bytecode format are not decoded.
- **WebAssembly and SPIR-V:** module structure, imports, exports, entry points
  and names; no disassembly. The WebAssembly component model is not read.
- **SQLite:** schema and row counts by walking the b-trees; row values are
  not listed. A cut database is reported with a warning. WAL frames are
  counted, not replayed.
- **Pickles:** disassembled by a symbolic stack, never unpickled. Imports
  computed at run time (`STACK_GLOBAL` of a non-literal) are shown as
  `<computed>`. The danger assessment is a list of known risky modules, not
  proof that a pickle is safe.
- **NumPy, safetensors, GGUF, ONNX:** headers, shapes and tables are checked;
  tensor data is never loaded. ONNX subgraphs and attribute values are not
  decoded.
- **pcap/pcapng:** Ethernet, raw IP, Linux cooked and loopback links; IPv4
  fragments are not reassembled and TCP streams are not rebuilt, so a TLS
  ClientHello or HTTP request split across segments is missed. TLS is never
  decrypted, even when a capture embeds its keys.
- **PDF:** objects, actions and JavaScript are listed; content streams other
  than FlateDecode object streams and JavaScript are not decoded (no other
  filters, no predictors), and encrypted documents keep their strings
  encrypted. Nothing is rendered or run.
- **`.lnk`:** header, link info, strings, shell-item target and extra-data
  blocks; property stores are listed, not decoded.
- **Firmware:** Intel HEX, S-record and UF2 ranges are rebuilt from records;
  nothing is flashed. Device trees are read as stored, without overlays.
- **Game archives:** VPK entries kept in `_NNN.vpk` files are listed, not
  readable. The Ren'Py index is read as plain pickle data only; split entries
  are listed, not readable. Encrypted or obfuscated game archives are not
  supported and never decrypted.

## Unknown files (reader of last resort)

- A signature names what a file **most likely** is; it does not validate the
  file. Only formats with a built-in reader are parsed further.
- Strings and embedded files are searched in the first 64 MiB, the text
  profile in the first 4 MiB; the entropy covers up to 1 GiB. Strings and notable values are
  capped (64 of each kind), and so is the number of embedded files.
- Embedded files are found by strong signatures and confirmed by a dedicated
  reader where one exists; an embedded file without one (an image, a 7-Zip
  archive) is listed but its end is only estimated (the next hit or the end
  of the file). A compressed or encrypted region is never decompressed or
  decrypted, so files hidden inside it are not seen.
- The language of a text file is a guess from its first lines.

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

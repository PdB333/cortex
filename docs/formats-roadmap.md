# Formats roadmap

Every file format and live target Cortex reads today, and every one it could
add, with its state and priority. It is the plan for static analysis after
v2.0; it is updated as formats land.

**State:** ✅ shipped · 🟡 partial · ⬜ planned
**Priority:** P1 next (Windows-first, reuses existing parsers) · P2 games ·
P3 languages and packaging · P4 AI, security, network, embedded
**Engine:** *built-in* means a parser in Cortex with no external tool;
*engine* means an external program the person installs (see
[analysis providers](analysis-providers.md)).

Rules that apply to every row:

- Formats are **read, never executed**. A format that can carry code (Python
  pickles, PyTorch checkpoints, scripts) is disassembled or listed, not run.
- **No decryption** of protected assets or code and **no unpacking** of
  commercial protectors (Themida, VMProtect, Denuvo and the like): Cortex
  detects them and stops.
- Each format lands with a fixture, a test and a fuzz harness, like the
  existing parsers ([tests/fuzz](../tests/fuzz/README.md)).

How each shipped reader was verified: [Windows formats](formats-wave1-implementation.md),
[archives, certificates, type libraries and packages](formats-next-implementation.md),
and, for the rest, `tests/format_more_tests.cpp` with the generators in
`tests/fixtures/format-more`. Their limits are in [limitations](limitations.md).

Any other file opens with the **reader of last resort**: likely type from
about 150 signatures or the extension, text or binary profile, entropy,
strings (URLs, IP addresses, paths, registry keys, GUIDs) and embedded files
confirmed by their own readers. See [limitations](limitations.md#unknown-files-reader-of-last-resort).

## 1. Executables and native binaries

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| PE32 / PE32+ | headers, sections, imports, exports, relocations, x64 unwind, code catalog, xrefs, CFG | ✅ | | built-in |
| PE Authenticode | WIN_CERTIFICATE, PKCS#7 signer and certificates, timestamp presence, file digest match (chain not verified) | ✅ | | built-in |
| PE import hash | imphash (pefile-compatible), delay-load imports | ✅ | | built-in |
| PE Rich header | product ids, builds, counts, checksum, hash | ✅ | | built-in |
| PE TLS callbacks | callback addresses | ✅ | | built-in |
| PE resources | resource tree, version info and strings, manifest (execution level, DPI) | ✅ | | built-in |
| PE debug directory | CodeView PDB path, GUID and age | ✅ | | built-in |
| PE hardening | load config; ASLR, high-entropy VA, DEP, CFG, SafeSEH, CET, /GS | ✅ | | built-in |
| PE overlay, entropy, packer detection | appended data, section entropy, 16 packer families and heuristics (detection only) | ✅ | | built-in |
| PE ARM64, ARM64EC, CHPE | Windows on ARM images | ⬜ | P3 | built-in |
| ELF | Native ELF32/64 LE/BE, headers, symbols, REL/RELA, notes, dependencies and hardening metadata; Ghidra optional for decompilation | ✅ |  | built-in |
| Mach-O (thin and fat) | load commands, symbols, code signature, entitlements, chained fixups | ✅ |  | built-in |
| ar static-library containers (.lib/.a) | Common/GNU/BSD member inventory and child bytes; thin external references refused; symbol indexes not decoded | ✅ |  | built-in |
| COFF objects (.obj/.exp) | object symbols, sections, relocations (not implied by ar container support) | 🟡 no .exp |  | built-in |
| DOS MZ, NE, LE/LX | 16-bit Windows, VxD, OS/2 headers | ⬜ | P4 | built-in (code via Ghidra) |
| Windows drivers (.sys) | Native-subsystem PE, candidate entry RVA, WDM flag and direct kernel imports (heuristic; not a proven DriverEntry) | ✅ |  | built-in PE |
| UEFI images (.efi, TE) | PE/TE headers, GUIDs | ⬜ | P4 | built-in |
| Intel HEX, Motorola S-record, UF2, raw images | load to an address; ARM vector table detection | 🟡 no raw images |  | built-in |

## 2. Symbols and debug information

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| PDB (MSF) | MSF7 GUID/age, DBI modules, symbols, common TPI/IPI records and C13 lines; no full type resolver | ✅ |  | built-in (dbghelp remains live) |
| DWARF (incl. split DWARF, .dSYM) | functions, types, lines | ⬜ | P3 | built-in |
| CodeView, .map files, Breakpad .sym | symbols | ⬜ | P3 | built-in |

## 3. Dumps and crashes

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| Windows minidumps (.dmp) | system, process, exception with module+offset and access, threads with registers, modules with versions and PDB ids, memory ranges | ✅ | | built-in |
| Windows Error Reporting reports | report fields | ⬜ | P3 | built-in |
| Cortex crash reports and snapshots | crash bundles, memory snapshots | ✅ | | built-in |
| Linux core files | threads, registers, mappings | ⬜ | P4 | built-in |
| Apple .ips crash reports | threads, images, registers | ⬜ | P4 | built-in |

## 4. Runtimes and compiled languages

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| .NET assemblies | metadata, CIL | ✅ | | built-in |
| .NET to C# | decompiled project or type | ✅ | | ILSpy |
| .NET ReadyToRun, single-file bundles, NativeAOT detection | bundle members, R2R headers | ⬜ | P1 | built-in |
| .NET resources (.resources) | resource names and values | ⬜ | P3 | built-in |
| .NET strong name, obfuscator detection | signature, known obfuscator markers (detection only) | ⬜ | P3 | built-in |
| WinRT metadata (.winmd) | CLR metadata, Windows Runtime version marker, TypeDef flags, type/interface list, assembly name checks; no WinRT semantic projection yet | ✅ |  | built-in .NET |
| COM type libraries (.tlb) | MSFT GUIDs, types/interfaces, methods and parameters, tested on Windows-generated x86/x64 files; no SLTG/full type resolver | ✅ |  | built-in |
| Java .class | constant pool, methods, bytecode | ✅ |  | built-in |
| Java .jar to Java | decompiled sources | ✅ | | Vineflower |
| .war and .ear | ZIP inventory and XML deployment-descriptor metadata; no execution | ✅ |  | built-in |
| JMOD | Version 1.0 wrapper and nested ZIP (real JDK fixture); class bytes only, no module semantics | ✅ |  | built-in |
| jimage | module image format | ⬜ | P3 | built-in |
| Android DEX, ODEX, VDEX, OAT, ART | classes, methods, bytecode | ⬜ | P3 | built-in |
| Python .pyc (2.7 to 3.13) | code objects, bytecode disassembly | ✅ |  | built-in |
| PyInstaller | CArchive members, Python version, entry scripts | ⬜ | P3 | built-in |
| Python zipapps (.pyz), wheels | ZIP inventory, __main__.py entry point and wheel METADATA dependencies; no Python import or execution | ✅ |  | built-in ZIP |
| py2exe, cx_Freeze, Nuitka detection | packaged executable inventory and markers | 🟡 .pyz and wheels |  | built-in |
| Lua bytecode (5.1 to 5.4, LuaJIT) | functions, constants, disassembly | ✅ |  | built-in |
| Go binaries | buildinfo, modules, pclntab function names (also when stripped) | ✅ |  | built-in |
| Rust binaries | cargo-auditable crates, crate names from paths and panics | ⬜ | P3 | built-in |
| Swift and Objective-C | runtime metadata, classes and selectors | ⬜ | P4 | built-in |
| Delphi and C++Builder | RTTI, DFM forms | ⬜ | P3 | built-in |
| Visual Basic 5/6 | VB structures, P-code | ⬜ | P4 | built-in |
| AutoIt (.a3x) and compiled AutoHotkey | embedded script | ⬜ | P3 | built-in |
| Dart AOT and Flutter | snapshot hash, engine provenance, symbols | ⬜ | P3 | built-in |
| Erlang BEAM | atoms, exports, imports, code | ⬜ | P4 | built-in |
| WebAssembly | sections, imports, exports, names, DWARF, disassembly | 🟡 no disassembly or DWARF |  | built-in |

## 5. JavaScript, web and desktop apps

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| JavaScript and TypeScript with source maps | modules, imports, routes, IPC | ✅ | | built-in |
| Electron .asar | file tree, integrity, package.json, preload, IPC channels | ✅ | | built-in |
| Node single executable apps, pkg, nexe | embedded scripts and assets | ⬜ | P3 | built-in |
| Bun and Deno compiled executables | embedded bundles | ⬜ | P3 | built-in |
| V8 bytecode (bytenode .jsc) | header, functions | ⬜ | P3 | built-in |
| Hermes bytecode (.hbc, React Native) | functions, strings, disassembly | ⬜ | P3 | built-in |
| webpack and Vite bundles | modules and chunk graph | ⬜ | P3 | built-in |
| Tauri apps | assets embedded in the executable | ⬜ | P3 | built-in |
| NW.js apps | package and scripts | ⬜ | P4 | built-in |
| Browser and editor extensions (.crx, .xpi, .vsix) | CRX2/3 header/proof inventory, WebExtension permissions/scripts and VSIX/VS Code manifests; signatures unverified, no install | ✅ |  | built-in |
| npm packages (.tgz) | gzip -> TAR -> package.json, dependencies/scripts as data and nested files; no install | ✅ |  | built-in |

## 6. Game engines

Reading and listing only. Encrypted archives stay encrypted.

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| Unity IL2CPP `global-metadata.dat` (v24 to v39) | assemblies, types, methods | ⬜ | P2 | built-in |
| IL2CPP to GameAssembly.dll mapping | method addresses for the live module | ⬜ | P2 | built-in |
| Unity Mono (Assembly-CSharp.dll) | .NET metadata and CIL | ✅ | | built-in |
| Unity AssetBundles (UnityFS), .assets, .resS | object inventory | ⬜ | P2 | built-in |
| Unreal .pak, IoStore (.utoc, .ucas) | file inventory (unencrypted) | ⬜ | P2 | built-in |
| Unreal .uasset, .uexp | names, imports, exports | ⬜ | P2 | built-in |
| Godot .pck, GDScript bytecode (.gdc) | inventory, tokens | ⬜ | P2 | built-in |
| GameMaker data.win | chunks, code entries | ⬜ | P2 | built-in |
| RPG Maker (.rgssad, .rvdata2), Ren'Py (.rpa, .rpyc) | archives, Ruby Marshal, Python bytecode | 🟡 Ren'Py .rpa |  | built-in |
| Source engine (.vpk, .bsp) | file inventory, map lumps | 🟡 .vpk |  | built-in |
| id Tech (.pak, .pk3, .wad) | file inventory | ✅ |  | built-in |
| Bethesda (.bsa, .ba2, .esp) | archive and plugin records | ⬜ | P4 | built-in |
| CryEngine .pak, XNA/MonoGame .xnb, LÖVE .love | inventory | ⬜ | P4 | built-in |
| Direct3D shaders (DXBC, DXIL, .cso) | signatures, resources, disassembly | ⬜ | P2 | built-in |

## 7. Archives, installers and packages

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| ZIP, JAR, APK, NuGet, wheels (stored and deflate) | entries, nested inspection | ✅ | | built-in |
| TAR (v7, ustar, PAX, GNU long names) | headers/checksums and bounded ordinary files; links/sparse/unsafe children not followed | ✅ |  | built-in |
| gzip (.gz/.tgz) | concatenated members, optional headers, CRC/ISIZE and bounded DEFLATE to a payload child | ✅ |  | built-in |
| 7z, bzip2, xz, zstd, lz4 | compressed archive readers | ⬜ | P3 | built-in |
| RAR | entries (read only) | ⬜ | P4 | built-in |
| CAB | Stored/MSZIP inventory and bounded expansion, block checksums/history; LZX/Quantum/spanned files unavailable | ✅ |  | built-in |
| OLE Compound File (CFB v3/v4) | Bounded FAT/DIFAT/MiniFAT, storage tree, child streams; no COM activation | ✅ |  | built-in |
| MSI | CFB/FAT/MiniFAT, schemas and table values, CustomAction listed only, embedded CAB | ✅ |  | built-in |
| MSIX, APPX, appxbundle | Namespaced manifests, identity/dependencies/apps, bundles, SHA-256 block comparison; no signature trust | ✅ |  | built-in ZIP + XML |
| NSIS, Inno Setup, InstallShield, WiX Burn | script, file list (no running) | ⬜ | P1 | built-in |
| Squirrel (Electron updates) | RELEASES hash/name-or-URL/size claims, full/delta metadata; NuGet uses existing ZIP reader; no download/apply/setup.exe unpacking | ✅ |  | built-in |
| ISO/UDF, VHD/VHDX, WIM, CHM | file trees | ⬜ | P3 | built-in |
| Debian .deb | ar members and nested control/data tar.gz; no installation, scripts or fetching | ✅ |  | built-in |
| rpm, AppImage, snap (squashfs) | control data, files | ⬜ | P4 | built-in |
| DMG, PKG (xar) | file trees | ⬜ | P4 | built-in |
| Docker and OCI images | layers, configuration | ⬜ | P4 | built-in |


## 8. Mobile

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| APK binary manifest (AXML), resources.arsc | permissions, components, strings | ⬜ | P3 | built-in |
| APK signatures (v1 to v4) | signers, certificate fingerprints, key rotation | ⬜ | P3 | built-in |
| AAB, split APKs, XAPK | modules, splits | ⬜ | P3 | built-in |
| iOS IPA | Info.plist, entitlements, provisioning profile, frameworks | ⬜ | P4 | built-in |
| Xamarin and MAUI (assemblies.blob, XALZ) | assemblies | ⬜ | P3 | built-in |
| Cordova and Capacitor | web assets | ⬜ | P4 | built-in |
| React Native bundles | Hermes or JS bundle | ⬜ | P3 | built-in |

## 9. Data and serialization

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| SQLite (with WAL) | schema, rows | ✅ |  | built-in |
| LevelDB and IndexedDB | keys and values (Electron and Chrome storage) | ⬜ | P3 | built-in |
| Protobuf | schema-less decoding; .proto from descriptors embedded in binaries | ⬜ | P3 | built-in |
| FlatBuffers (.bfbs), Cap'n Proto, MessagePack, CBOR, BSON, Thrift, Avro | structure | ⬜ | P4 | built-in |
| Property lists (binary, XML), NSKeyedArchiver | values | 🟡 binary |  | built-in |
| Windows registry hives (REGF) | base block, key tree, typed values (bounded) | ✅ | | built-in |
| Registry exports (.reg) | keys and values | ⬜ | P3 | built-in |
| Java serialization, .NET BinaryFormatter (detection), Python pickle (disassembly), Ruby Marshal, PHP serialize | structure, never instantiated | 🟡 Python pickle |  | built-in |
| X.509 DER/PEM and PKCS#7/CMS | issuer/subject, validity, algorithms, fingerprints, signer/certificate/content inventory; no signature or chain trust | ✅ |  | built-in |
| Catalog files (.cat) | CMS CTL OID recognition and raw content; catalog-entry decoding and signature trust not implemented | ✅ |  | built-in |

## 10. AI models

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| GGUF, safetensors | metadata, tensors | ✅ |  | built-in |
| ONNX | graph, operators, inputs and outputs | ✅ |  | built-in |
| PyTorch .pt / .pth | ZIP plus pickle opcodes, flagged imports (never loaded) | ✅ |  | built-in |
| TensorFlow SavedModel, .pb, TFLite | graph, signatures | ⬜ | P4 | built-in |
| Keras (.h5, .keras), NumPy (.npy, .npz) | layers, arrays | 🟡 NumPy |  | built-in |
| Core ML, OpenVINO IR, TensorRT engine headers | model metadata | ⬜ | P4 | built-in |

## 11. GPU code and shaders

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| CUDA fatbin, CUBIN, PTX; HIP code objects | targets, kernels, parameters | ⬜ | P4 | built-in |
| DXBC, DXIL, .cso | see game engines | ⬜ | P2 | built-in |
| SPIR-V | modules, entry points, disassembly | ✅ |  | built-in |
| Metal .metallib, Unity shader blobs | inventory | ⬜ | P4 | built-in |

## 12. Documents and scripts

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| PDF | objects, JavaScript, embedded files | ✅ |  | built-in |
| Office OOXML and OLE, VBA macros (source and P-code) | parts, macros | ⬜ | P4 | built-in |
| RTF (OLE objects), OneNote, .lnk shortcuts | objects, targets | 🟡 .lnk |  | built-in |
| PowerShell, VBScript, JScript, HTA, batch | parsed structure, never run | ⬜ | P4 | built-in |

## 13. Network and traces

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| pcap, pcapng | packets, flows, TLS SNI, HTTP | ✅ |  | built-in |
| HAR, Fiddler SAZ, mitmproxy flows | requests and responses | ⬜ | P3 | built-in |
| ETW traces (.etl), Process Monitor (.pml), event logs (.evtx) | events | ⬜ | P4 | built-in |
| Chrome traces (JSON) | events | ⬜ | P4 | built-in |

## 14. Firmware and embedded

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| UEFI capsules, firmware volumes, FFS | files, GUIDs, PE/TE images | ⬜ | P4 | built-in |
| U-Boot images (uImage, FIT), device trees (DTB) | headers, nodes | 🟡 device trees |  | built-in |
| SquashFS, CramFS, JFFS2, UBIFS, ext4 | file trees | ⬜ | P4 | built-in |
| Android boot.img, vbmeta, sparse images | headers | ⬜ | P4 | built-in |
| ESP32 and ESP8266 images, SVD files | segments, peripherals | ⬜ | P4 | built-in |
| sigrok captures (.sr) | channels, samples | ⬜ | P4 | built-in |

## 15. Kernel and system

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| Windows .inf, .cat, .mui, side-by-side manifests | sections, catalogs, resources | 🟡 .cat |  | built-in |
| Linux kernel modules (.ko), eBPF objects | modinfo, programs, maps | ⬜ | P4 | built-in |
| macOS kexts | bundle and Mach-O | ⬜ | P4 | built-in |

## 16. Smart contracts

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| EVM bytecode | dispatch selectors, disassembly | ⬜ | P4 | built-in |
| Solana BPF, Move bytecode, CosmWasm and NEAR WASM | programs, functions | ⬜ | P4 | built-in |

## Live targets

| Target | What Cortex does | State | Priority |
|---|---|---|---|
| Windows x86 and x64 processes | memory scan and edit, debugger, breakpoints, trace, runtime | ✅ | |
| Node.js and Electron (CDP) | scripts, log breakpoints, console | ✅ | |
| Python (debugpy), .NET (netcoredbg), Java (JDWP) | log breakpoints | ✅ | |
| WebView2 and Chromium renderers (CDP) | scripts, log breakpoints | ⬜ | P1 |
| Unreal reflection (GNames, GObjects) on an attached game | object and class names, live values | ⬜ | P2 |
| Unity Mono and IL2CPP on an attached game | classes, methods, fields at run time | ⬜ | P2 |
| Lua embedded in a game | states, globals, functions | ⬜ | P2 |
| Windows services, COM objects | service and class inventory | ⬜ | P3 |
| Linux processes (process_vm_readv, ptrace) | memory scan and edit, hardware breakpoints, trace | ⬜ | see Platforms |
| Android devices (adb) | packages, processes, logs | ⬜ | P4 |

## Waves

1. **P1, Windows first.** PE completed (Authenticode, imphash, Rich header,
   TLS, resources, debug directory, hardening, packer detection), PDB,
   minidumps, .winmd, .tlb, drivers, CAB, MSI, MSIX, NSIS, Inno Setup,
   Squirrel, registry hives, certificates, native ELF, WebView2 live.
2. **P2, games.** IL2CPP with GameAssembly mapping, Unity assets, Unreal .pak
   and IoStore, .uasset, Godot, GameMaker, RPG Maker, Ren'Py, Lua bytecode,
   DXBC/DXIL, SPIR-V; live Unreal, Unity and Lua.
3. **P3, languages and packaging.** .pyc and PyInstaller, Go, Rust, Delphi,
   AutoIt, Java class files, DEX, Node SEA and bundlers, Hermes, Tauri, WASM,
   archives, APK internals, Protobuf, LevelDB, SQLite.
4. **P4, the rest.** AI models, documents and macros, network captures and
   traces, firmware, Mach-O and Apple formats, smart contracts.

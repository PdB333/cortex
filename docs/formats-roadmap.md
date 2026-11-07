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
| ELF | sections, segments, symbols, relocations, notes, hardening | 🟡 Ghidra only | P1 | built-in |
| Mach-O (thin and fat) | load commands, symbols, code signature, entitlements, chained fixups | ⬜ | P3 | built-in |
| COFF objects, .lib and .a archives, .exp | symbols, sections, archive members | ⬜ | P3 | built-in |
| DOS MZ, NE, LE/LX | 16-bit Windows, VxD, OS/2 headers | ⬜ | P4 | built-in (code via Ghidra) |
| Windows drivers (.sys) | PE plus driver entry, imports from ntoskrnl | ⬜ | P1 | built-in |
| UEFI images (.efi, TE) | PE/TE headers, GUIDs | ⬜ | P4 | built-in |
| Intel HEX, Motorola S-record, UF2, raw images | load to an address; ARM vector table detection | ⬜ | P4 | built-in |

## 2. Symbols and debug information

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| PDB (MSF) | public and private symbols, types, line numbers | 🟡 through dbghelp in the live symbolizer | P1 | built-in |
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
| WinRT metadata (.winmd) | types and members (ECMA-335, same parser as .NET) | ⬜ | P1 | built-in |
| COM type libraries (.tlb) | interfaces, methods, GUIDs | ⬜ | P1 | built-in |
| Java .class | constant pool, methods, bytecode | ⬜ | P3 | built-in |
| Java .jar to Java | decompiled sources | ✅ | | Vineflower |
| .war, .ear, JMOD, jimage | archive members, modules | ⬜ | P3 | built-in |
| Android DEX, ODEX, VDEX, OAT, ART | classes, methods, bytecode | ⬜ | P3 | built-in |
| Python .pyc (2.7 to 3.13) | code objects, bytecode disassembly | ⬜ | P3 | built-in |
| PyInstaller | CArchive members, Python version, entry scripts | ⬜ | P3 | built-in |
| py2exe, cx_Freeze, Nuitka detection, .pyz, wheels | members and markers | ⬜ | P3 | built-in |
| Lua bytecode (5.1 to 5.4, LuaJIT) | functions, constants, disassembly | ⬜ | P2 | built-in |
| Go binaries | buildinfo, modules, pclntab function names (also when stripped) | ⬜ | P3 | built-in |
| Rust binaries | cargo-auditable crates, crate names from paths and panics | ⬜ | P3 | built-in |
| Swift and Objective-C | runtime metadata, classes and selectors | ⬜ | P4 | built-in |
| Delphi and C++Builder | RTTI, DFM forms | ⬜ | P3 | built-in |
| Visual Basic 5/6 | VB structures, P-code | ⬜ | P4 | built-in |
| AutoIt (.a3x) and compiled AutoHotkey | embedded script | ⬜ | P3 | built-in |
| Dart AOT and Flutter | snapshot hash, engine provenance, symbols | ⬜ | P3 | built-in |
| Erlang BEAM | atoms, exports, imports, code | ⬜ | P4 | built-in |
| WebAssembly | sections, imports, exports, names, DWARF, disassembly | ⬜ | P3 | built-in |

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
| Browser and editor extensions (.crx, .xpi, .vsix) | manifest, permissions, scripts | ⬜ | P3 | built-in |
| npm packages (.tgz) | package.json, files | ⬜ | P4 | built-in |

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
| RPG Maker (.rgssad, .rvdata2), Ren'Py (.rpa, .rpyc) | archives, Ruby Marshal, Python bytecode | ⬜ | P2 | built-in |
| Source engine (.vpk, .bsp) | file inventory, map lumps | ⬜ | P4 | built-in |
| id Tech (.pak, .pk3, .wad) | file inventory | ⬜ | P4 | built-in |
| Bethesda (.bsa, .ba2, .esp) | archive and plugin records | ⬜ | P4 | built-in |
| CryEngine .pak, XNA/MonoGame .xnb, LÖVE .love | inventory | ⬜ | P4 | built-in |
| Direct3D shaders (DXBC, DXIL, .cso) | signatures, resources, disassembly | ⬜ | P2 | built-in |

## 7. Archives, installers and packages

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| ZIP, JAR, APK, NuGet, wheels (stored and deflate) | entries, nested inspection | ✅ | | built-in |
| 7z, TAR, gzip, bzip2, xz, zstd, lz4 | entries | ⬜ | P3 | built-in |
| RAR | entries (read only) | ⬜ | P4 | built-in |
| CAB | entries | ⬜ | P1 | built-in |
| MSI | OLE compound file, tables, custom actions, embedded CABs | ⬜ | P1 | built-in |
| MSIX, APPX, appxbundle | manifest, signature, files | ⬜ | P1 | built-in |
| NSIS, Inno Setup, InstallShield, WiX Burn | script, file list (no running) | ⬜ | P1 | built-in |
| Squirrel (Electron updates) | packages, releases file | ⬜ | P1 | built-in |
| ISO/UDF, VHD/VHDX, WIM, CHM | file trees | ⬜ | P3 | built-in |
| deb, rpm, AppImage, snap (squashfs) | control data, files | ⬜ | P4 | built-in |
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
| SQLite (with WAL) | schema, rows | ⬜ | P3 | built-in |
| LevelDB and IndexedDB | keys and values (Electron and Chrome storage) | ⬜ | P3 | built-in |
| Protobuf | schema-less decoding; .proto from descriptors embedded in binaries | ⬜ | P3 | built-in |
| FlatBuffers (.bfbs), Cap'n Proto, MessagePack, CBOR, BSON, Thrift, Avro | structure | ⬜ | P4 | built-in |
| Property lists (binary, XML), NSKeyedArchiver | values | ⬜ | P4 | built-in |
| Windows registry hives (REGF) | base block, key tree, typed values (bounded) | ✅ | | built-in |
| Registry exports (.reg) | keys and values | ⬜ | P3 | built-in |
| Java serialization, .NET BinaryFormatter (detection), Python pickle (disassembly), Ruby Marshal, PHP serialize | structure, never instantiated | ⬜ | P3 | built-in |
| X.509, PKCS#7, catalog files (.cat) | certificates and hashes | ⬜ | P1 | built-in |

## 10. AI models

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| GGUF, safetensors | metadata, tensors | ⬜ | P4 | built-in |
| ONNX | graph, operators, inputs and outputs | ⬜ | P4 | built-in |
| PyTorch .pt / .pth | ZIP plus pickle opcodes, flagged imports (never loaded) | ⬜ | P4 | built-in |
| TensorFlow SavedModel, .pb, TFLite | graph, signatures | ⬜ | P4 | built-in |
| Keras (.h5, .keras), NumPy (.npy, .npz) | layers, arrays | ⬜ | P4 | built-in |
| Core ML, OpenVINO IR, TensorRT engine headers | model metadata | ⬜ | P4 | built-in |

## 11. GPU code and shaders

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| CUDA fatbin, CUBIN, PTX; HIP code objects | targets, kernels, parameters | ⬜ | P4 | built-in |
| DXBC, DXIL, .cso | see game engines | ⬜ | P2 | built-in |
| SPIR-V | modules, entry points, disassembly | ⬜ | P2 | built-in |
| Metal .metallib, Unity shader blobs | inventory | ⬜ | P4 | built-in |

## 12. Documents and scripts

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| PDF | objects, JavaScript, embedded files | ⬜ | P4 | built-in |
| Office OOXML and OLE, VBA macros (source and P-code) | parts, macros | ⬜ | P4 | built-in |
| RTF (OLE objects), OneNote, .lnk shortcuts | objects, targets | ⬜ | P4 | built-in |
| PowerShell, VBScript, JScript, HTA, batch | parsed structure, never run | ⬜ | P4 | built-in |

## 13. Network and traces

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| pcap, pcapng | packets, flows, TLS SNI, HTTP | ⬜ | P4 | built-in |
| HAR, Fiddler SAZ, mitmproxy flows | requests and responses | ⬜ | P3 | built-in |
| ETW traces (.etl), Process Monitor (.pml), event logs (.evtx) | events | ⬜ | P4 | built-in |
| Chrome traces (JSON) | events | ⬜ | P4 | built-in |

## 14. Firmware and embedded

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| UEFI capsules, firmware volumes, FFS | files, GUIDs, PE/TE images | ⬜ | P4 | built-in |
| U-Boot images (uImage, FIT), device trees (DTB) | headers, nodes | ⬜ | P4 | built-in |
| SquashFS, CramFS, JFFS2, UBIFS, ext4 | file trees | ⬜ | P4 | built-in |
| Android boot.img, vbmeta, sparse images | headers | ⬜ | P4 | built-in |
| ESP32 and ESP8266 images, SVD files | segments, peripherals | ⬜ | P4 | built-in |
| sigrok captures (.sr) | channels, samples | ⬜ | P4 | built-in |

## 15. Kernel and system

| Format | What Cortex reads | State | Priority | Engine |
|---|---|---|---|---|
| Windows .inf, .cat, .mui, side-by-side manifests | sections, catalogs, resources | ⬜ | P1 | built-in |
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

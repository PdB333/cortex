# Cortex crash symbolization

Milestone 3 adds exact build identification, PDB symbol loading, source-line resolution, stack walking and an offline MinGW/DWARF fallback.

## Crash output

When diagnostics and symbolization are enabled, a crash directory contains the milestone 1 and 2 files plus:

```text
stack.json
build_info.json
report.txt
```

`stack.json` contains each instruction address, module, RVA, function, displacement, source file, line and symbol-verification status. It remains useful without symbols because module paths and RVAs are always preserved.

`build_info.json` records the PE timestamp, image size, preferred image base, CodeView PDB GUID and age, configured symbol path and the PDB actually loaded by DbgHelp.

`report.txt` is a readable summary with the suspected mod, resolved crash location, stack, active scopes and recent diagnostic values.

## Exact PDB matching

Cortex reads the PE CodeView `RSDS` record and compares its PDB GUID and age with the values reported by DbgHelp. A report is marked `pdb_guid_age_match` only when both values match.

A PDB with the right filename but the wrong GUID or age is not treated as trusted source information. The report retains the raw module RVA and emits a mismatch warning.

## Configuration

Add these keys to `cortex.ini`:

```ini
diagnostics_symbolize = true
diagnostics_symbol_path = C:\path\to\cortex_symbols
diagnostics_external_symbolizer = C:\path\to\llvm-symbolizer.exe
diagnostics_max_stack_frames = 64
```

When `diagnostics_symbol_path` is empty, Cortex uses a `cortex_symbols` directory next to `cortex_core.dll`. `_NT_SYMBOL_PATH`, the module directory and each mod's registered `symbol_path` are also considered.

The external symbolizer is not launched inside the crashing process. It is only used by the offline `cortex_symbolize` utility.

## MSVC and clang-cl mods

Build the mod with debug information and keep the DLL and PDB from the same build:

```cmake
if(MSVC)
    target_compile_options(MyMod PRIVATE /Zi)
    target_link_options(MyMod PRIVATE /DEBUG)
endif()
```

Register the PDB path through the milestone 2 SDK:

```cpp
CortexDiagModInfo info{};
info.struct_size = sizeof(info);
info.abi_version = CORTEX_DIAG_ABI_VERSION;
info.module = module;
info.name = "MyMod";
info.version = "1.2.0";
info.git_commit = MY_GIT_COMMIT;
info.source_root = MY_SOURCE_ROOT;
info.symbol_path = "C:\\mods\\symbols\\MyMod.pdb";
CortexDiagRegisterMod(&info);
```

The registered source root is used to remap PDB paths from a build machine to local source files when possible.

## MinGW and DWARF

Build with DWARF information:

```bash
g++ -g -O0 -shared mod.cpp -o MyMod.dll
```

Use the offline utility with `llvm-symbolizer.exe` or `addr2line.exe`:

```powershell
cortex_symbolize.exe `
  --image C:\mods\MyMod.dll `
  --rva 0x1832 `
  --tool C:\mingw64\bin\addr2line.exe
```

The tool emits one JSON object containing the backend, function, source file and line. With no `--tool`, it tries DbgHelp/PDB first, then automatically searches for `llvm-symbolizer.exe` and `addr2line.exe`.

Build the utility independently with:

```powershell
cmake -S tools -B build-symbolizer
cmake --build build-symbolizer --config Release
```

## API

`GET /symbols/resolve?address=0x...` now returns module information, RVA, build ID, function, line, loaded PDB and exact-match status.

`GET /symbols/module?address=0x...` returns the complete PE/PDB identity and verification result for the containing module.

## Safety behavior

DbgHelp is globally serialized because it is not thread-safe. Crash-time stack capture uses a non-blocking lock. When another thread owns DbgHelp, Cortex skips symbol resolution rather than deadlocking and still writes the crash instruction as `module+RVA`.

The external DWARF tools are intentionally excluded from the in-process exception handler because creating child processes while the game is crashing is unsafe. Offline symbolization can be run after the game exits.

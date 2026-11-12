# Parser fuzzing

Files Cortex inspects are untrusted input. These libFuzzer harnesses run the
file parsers under AddressSanitizer and UndefinedBehaviorSanitizer:

| Harness | What it drives |
|---|---|
| `fuzz_artifact` | The provider registry as `cortex.exe inspect` uses it: detection, `Inspect`, the list of children and up to six children two levels deep (PE, native-subsystem .sys driver candidates, WinMD/WinRT metadata, .NET, ZIP/JAR with DEFLATE, ASAR, JavaScript/TypeScript, source maps). The first byte picks the file name, since some providers also look at the extension. |
| `fuzz_pe_code` | `ParsePe`, CLR metadata and the CIL of up to 16 methods, and the native-code index (Zydis): functions, strings, xrefs, CFG and fingerprints of the first functions. Inputs are capped at 1 MiB. |
| `fuzz_text` | The JavaScript lexer and analysis, source maps, base64 data URLs and DEFLATE. The first byte picks the decoder. |
| `fuzz_protocols` | The live debug protocols as the sessions decode them (`core/live/protocol_frames`): WebSocket messages for CDP, DAP's `Content-Length` framing, JDWP packets, and the typed accessors (`core/live/json_safe.h`) applied at every node of each message. Also checks properties: cleaned text always serializes, `ValidUtf8` is idempotent, `Clip` respects its bound. |
| `fuzz_stores` | Evidence and investigation records read from disk (list, get, export, start, approve), and knowledge records a client sends (`Validate`, `Prepare` with a fuzzed previous revision). No exception may leave a store. |

## Running

Linux with clang and its sanitizer runtimes (`libclang-rt-18-dev` on Ubuntu):

```bash
cmake -S tests/fuzz -B build/fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/fuzz
python3 tests/fuzz/make_seeds.py build/fuzz-seeds
build/fuzz/fuzz_artifact -max_total_time=300 -max_len=65536 build/fuzz-work build/fuzz-seeds/artifact
```

CI (`contracts.yml`, job `fuzz-parsers`) runs each harness for 60 seconds on
every pull request, and uploads any crashing input as an artifact.

## Keeping a crash

When a harness finds a crash, fix the parser, then add the input to
`regressions/<harness>/`. `-DCORTEX_FUZZ_REPLAY=ON` builds the harnesses as
plain executables, and `ctest` replays every file there on each run.

## Not covered

- The project store (`core/project/project.cpp`) is Windows-only; its loader
  is covered by unit tests, not by a fuzzer.
- The capture code that runs at a breakpoint hit (stack and locals requests)
  sends requests to the debugger, so it is exercised by the live tests, not
  by `fuzz_protocols`. It reads replies through the same typed accessors.
- Python bytecode, Unity `global-metadata.dat`, class files and WebAssembly
  have no parser yet.

## Wave 1 harness and Windows mutation runs

`fuzz_wave1` exercises PDB, ELF, CAB, CFB/MSI, bounded XML, ZIP/APPX and dictionary-aware DEFLATE, including JSON serialization and bounded child reads. `make_seeds.py` supplies independent-producer and structural fixture inputs. The original full fuzz CMake project and `tools/static` both build the harness.

LLVM-MinGW currently rejects libFuzzer instrumentation for its target. `tools/static` therefore also provides `cortex_fuzz_wave1_mutations`: the same harness with fixed-seed mutations under ASan/UBSan, explicitly **not coverage-guided**. The local run completed 20,000 mutations after fixing `zip-invalid-utf8-name`, now a retained regression. The workflow `formats-portable.yml` provides native Linux Clang/libFuzzer jobs, not a claim of a run that has not occurred.

## Additional formats harness

`fuzz_format_next` selects TAR, gzip, ar, certificate/CMS, MSFT type-library,
ZIP package metadata, CRX/JMOD wrapper or Squirrel RELEASES parsing with its
first byte. It checks structured output serialization and bounded readable
children. Inputs are capped at 1 MiB. `make_seeds.py` builds the 48-input
`format_next` corpus from Python/OpenSSL/Windows/JDK fixtures.

`tools/static` provides `cortex_fuzz_format_next_mutations` for deterministic
ASan/UBSan sweeps on Windows and `fuzz_format_next` for native Clang libFuzzer.
The latter is wired into `formats-portable.yml`; providing a workflow is not
proof that it ran. The additional-format crash-replay directory is initially
empty, so a passing zero-input replay is not a fuzz-coverage claim.

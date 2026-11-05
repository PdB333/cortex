# Parser fuzzing

Files Cortex inspects are untrusted input. These libFuzzer harnesses run the
file parsers under AddressSanitizer and UndefinedBehaviorSanitizer:

| Harness | What it drives |
|---|---|
| `fuzz_artifact` | The provider registry as `cortex.exe inspect` uses it: detection, `Inspect`, the list of children and up to six children two levels deep (PE, .NET, ZIP/JAR with DEFLATE, ASAR, JavaScript/TypeScript, source maps). The first byte picks the file name, since some providers also look at the extension. |
| `fuzz_pe_code` | `ParsePe`, CLR metadata and the CIL of up to 16 methods, and the native-code index (Zydis): functions, strings, xrefs, CFG and fingerprints of the first functions. Inputs are capped at 1 MiB. |
| `fuzz_text` | The JavaScript lexer and analysis, source maps, base64 data URLs and DEFLATE. The first byte picks the decoder. |

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

- The live protocol decoders (WebSocket frames for CDP, DAP's
  `Content-Length` framing, JDWP packets) read from sockets inside a session
  and are not fuzzed. They talk only to a debug interface on 127.0.0.1 that
  the operator started.
- Project, knowledge and evidence files are parsed with nlohmann/json, with
  Cortex's checks on top. They are covered by unit tests, not by a fuzzer.
- Python bytecode, Unity `global-metadata.dat`, class files and WebAssembly
  have no parser yet.

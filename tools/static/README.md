# Portable static Cortex readers

This target builds the same artifact Registry used by Cortex's GUI/CLI/MCP,
without the Windows runtime, debugger, graphics stack or injection code.
It is a file-inspection frontend, **not a full Linux port**. The only build
library is Cortex's existing pinned nlohmann/json dependency.

## Build and inspect

```sh
cmake -S tools/static -B build/static -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/static --parallel 2
ctest --test-dir build/static --output-on-failure
build/static/cortex-inspect tests/fixtures/elf/actor.elf
build/static/cortex-inspect tests/fixtures/compound/microsoft.msi --children
```

Use `--child MEMBER` to inspect one in-memory member and `--max N` to bound
listed items (0..65536). All artifacts are treated as bytes; no fixture is
executed or installed. PE, CLR, archive and script readers are the same as
in the main product, so this does not invent a parallel format implementation.

## Native Clang sanitizers and libFuzzer

```sh
cmake -S tools/static -B build/static-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS_DEBUG=-O1 -g' \
  -DCORTEX_STATIC_SANITIZERS=ON -DCORTEX_STATIC_FUZZ=ON
cmake --build build/static-fuzz --parallel 2
ctest --test-dir build/static-fuzz --output-on-failure
python3 tests/fuzz/make_seeds.py build/seeds
mkdir -p build/fuzz-work build/findings
build/static-fuzz/fuzz_wave1 -max_total_time=120 -max_len=1048576 \
  -artifact_prefix=build/findings/ build/fuzz-work build/seeds/wave1
```

The Linux workflow is `.github/workflows/formats-portable.yml`. It has not
been run merely because this file exists. On Windows, LLVM-MinGW's target
rejects libFuzzer instrumentation; configure `CORTEX_STATIC_FUZZ=OFF` and
`CORTEX_STATIC_SANITIZERS=ON`, then use the deterministic mutation runner:

```sh
cortex_fuzz_wave1_mutations build/seeds/wave1 20000
```

That runner is **not coverage-guided**. It uses the same harness under
ASan/UBSan, logs case numbers, and optionally writes the latest input with
`CORPUS ITERATIONS ONLY_CASE REPRO_FILE`. Use `18446744073709551615` as
ONLY_CASE for all cases. A fixed seed and sorted corpus make a failing case
reproducible as long as the corpus is unchanged.

Windows x64 and independent x86 sanitizer builds were tested locally.
Runtime DLLs for an LLVM-MinGW build must match its target architecture.
An `x64 build parsing an x86 file` is not an x86 parser-executable build;
both were exercised separately.

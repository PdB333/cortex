# Cortex Remote implementation verification

Recorded on 2026-11-11 in the `feat/formats-cortex-remote` worktree, based on
`3cdc7cb97e8d59221a9aca13c388b4d2500d146d`. These are observed local results,
not a claim of universal security or console compatibility.

This is the initial implementation record. Subsequent changes and revalidation
are recorded in [the hardening/query report](cortex-remote-hardening.md).

## Windows client and own agent

Toolchain: llvm-mingw 20261104, Clang 23.1.5, separate x86_64 and i686 builds.
Both the Cortex code and Mbed TLS were built with AddressSanitizer and
UndefinedBehaviorSanitizer (`CORTEX_REMOTE_SANITIZERS=ON`).

| Check | Observed result |
|---|---|
| Pure protocol contracts | 49 checks passed on each architecture |
| Deterministic decoder mutations | 10,000 bounded header/JSON inputs on each architecture |
| Own C++ client against own C++ agent | 42 end-to-end checks passed on x64 and x86 |
| Full `cortex.exe` CLI/MCP against own agent | 51 checks passed |
| GUI application workspace smoke | 33 workspaces / 9 presets; passed, including the remote tab |
| Native Win32/D3D11 window smoke | Passed |

The server uses its explicitly selected `--fixture` backend: an owned byte
vector, not a fabricated PS5 backend. All test connections are loopback only.
The independent Python peer verifies the published frame layout over actual
mutual TLS, not by calling the C++ codec.

Negative tests cover missing client certificates, unrelated CAs, wrong server
name, certificate-purpose mismatch, agent/profile write policy, stale target
IDs, replayed sessions, fragmented input, malformed/oversized/deep JSON,
duplicate keys, unexpected sequence/version, idle timeout, expected-byte/name
checks, dropped write responses and audit-log non-disclosure of malformed
input contents. No console payload or firmware was installed or executed.

## Native PS5 build

`cortex-agent.elf` was cross-compiled and linked with the external open SDK
v0.45 and LLVM/Clang 23.1.5. The SDK ZIP was SHA-256 checked:

`5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c`

ELF inspection confirmed x86-64 ELF64 little-endian PIE, an SDK `_start` entry,
native-backend symbols and the expected platform-library dependencies. The
PS5 build does **not** link the fixture backend. No third-party debugger
server/protocol is required by this binary.

**Not tested:** execution on a physical console, actual firmware structure
layouts, native process permissions, native ptrace attach/read/write/detach,
platform entropy availability and clock setup. The agent refuses missing
permissions, entropy, credentials or incompatible layouts instead of guessing.
A cross-build alone does not validate any firmware version.

## Previous format work retained and retested

The final desktop binary also passed:

- `formats_wave1_e2e.py`: 38 CLI/MCP checks.
- `formats_next_e2e.py`: 129 CLI/MCP checks, 56 fixture hashes, 24 cases.
- Six portable-format CTest suites on Windows x64 and six on Windows x86,
  including the .NET/WinMD, PDB/ELF/CAB/MSI and subsequent package-reader tests.

Those tests authorized no writes and executed none of the files being analyzed.
Their detailed support boundaries remain in the format roadmap and the two
format implementation reports; this work does not turn planned formats into
implemented ones.

## Reproduction

```text
cmake -S tools/remote -B build/remote -G Ninja -DCORTEX_REMOTE_SANITIZERS=ON
cmake --build build/remote
ctest --test-dir build/remote --output-on-failure
```

Use a sanitizer-capable compiler and OpenSSL for temporary credentials. For the
full desktop integration:

```text
python tests/remote_agent_e2e.py --client <cortex.exe> --agent <cortex-agent.exe> --cortex --work <ignored-temporary-directory>
```

Local logs and generated credentials stay under `build/cortex-remote-verification/`
and are not committed. The new GitHub workflow separately runs native Linux
sanitizers, a coverage-guided decoder campaign and a PS5 cross-build. **These
local results describe that initial run; later GitHub outcomes are recorded on
the PR and in the separate hardening report, not inferred from these counts.**

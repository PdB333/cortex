# Contributing to Cortex

Thanks for helping. Cortex attaches to processes and changes them, so the bar
for safety and evidence is higher than for most tools. Please read this page
before opening a pull request.

## Ground rules

- **Authorized targets only.** Develop and test against software you own,
  the repository's fixtures (`test_target/`, `tests/fixtures/`) or offline
  test builds. Do not add features whose purpose is to bypass anti-cheat,
  DRM, licence checks or access controls. Such pull requests are closed.
- **Reading stays read-only.** A feature that only observes must work without
  write authority. Anything that changes a target (memory, breakpoints,
  input, loading the runtime, starting or stopping a process) must require
  the operator's authority (`Writes allowed`, `--allow-writes`) *and*, through
  MCP, `mutation_permission: true` in the call.
- **No claim without a test.** A feature counts as working only after it has
  been checked against a real case or a representative fixture. Say which one
  in the pull request, and record anything that is not validated as a
  limitation in the docs.
- **Never weaken a test to get green.** Do not skip, disable, loosen or
  quarantine a failing test. Fix the cause, or explain in the pull request why
  the test is wrong.
- **Reuse what exists.** The scanner, debugger, structure view, input UI,
  MCP layer, evidence store and providers already have primitives; extend
  them rather than adding a parallel path.

## Build

Windows x64 with MSYS2 MinGW-w64, CMake 3.21+ and Ninja (the CI setup):

```powershell
# Desktop application and its tests
cmake -S app_imgui -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/gui --parallel

# Runtime and test target, x64 (use a MINGW32 shell for x86)
cmake -S . -B build/runtime-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/runtime-x64 --parallel
```

Cross-compiling from Linux with `x86_64-w64-mingw32` and
`i686-w64-mingw32` works for development, and most suites run under Wine.
CI on real Windows is still the reference.

## Test before you open a pull request

At minimum:

```powershell
ctest --test-dir build/gui --output-on-failure
ctest --test-dir build/runtime-x64 --output-on-failure
```

Then the end-to-end suites that cover what you touched, from a staged bundle
(see `.github/workflows/imgui-gui-tests.yml` for the exact arguments):

| You changed | Run |
|---|---|
| Anything reachable from MCP | `tests/security_no_writes_regression.py` (no call may change a target without `--allow-writes`) |
| Investigation context, targets | `tests/context_isolation_e2e.py` |
| Scanner, watches, breakpoints, navigation | `tests/manual_demo_e2e.py` |
| Static parsers (PE, ASAR, JS, ZIP) | `tests/artifact_mcp_e2e.py`, `tests/native_static_e2e.py` |
| .NET metadata or CIL | `tests/dotnet_e2e.py` |
| Evidence, investigations | `tests/evidence_e2e.py`, `tests/investigation_e2e.py` |
| External engines | `cortex_analysis_provider_tests`, `tests/analysis_e2e.py` |
| Live providers | `cortex_cdp_tests`, `cortex_dap_tests`, `tests/live_e2e.py` |

CI runs the same suites on Windows for x64 and x86 targets. Do not merge on a
red or pending run.

## Pull requests

- Work on a feature branch. Keep unrelated changes in separate commits or
  pull requests.
- Describe what changed, how you tested it (commands, fixture, platform), and
  what is still not validated.
- Update the docs that describe the behaviour: `docs/`, `agent/`, and the
  `[Unreleased]` section of `CHANGELOG.md`.
- New parsers handle untrusted input: check every bound, cap sizes and counts,
  and add a fixture with malformed cases.
- New MCP tools declare their risk (`tests/fixtures/mcp_tool_risks.json`), keep
  a bounded output, and refuse changes without authority.

## Reporting bugs

Use the issue templates. Include the Cortex version or commit, Windows version,
target architecture, the exact steps, and logs from beside `cortex.exe`
(`cortex_denied_mutations.jsonl`, `cortex_debugger_events.jsonl`) where
relevant. Remove anything private first. Report security problems as described
in [SECURITY.md](SECURITY.md), not in a public issue.

## Licence

By contributing you agree that your contribution is licensed under the
project's [MIT licence](LICENSE).

# Cortex Remote hardening and bounded queries

Recorded on 2026-11-11, continuing `feat/formats-cortex-remote` from
`cfaf3e4b38ed98ce45d2dc8eca37b198487fb0ed`. This report distinguishes tested
behavior from physical-console compatibility. It is not a security certification.

## Concrete defects addressed

| Finding | Change | Regression evidence |
|---|---|---|
| The x86 GUI fixture CI job failed before compilation because `mingw-w64-i686-ccache` was unavailable in that installation | Remove the optional cache dependency, restore step and compiler launchers from that job; retain compiler/runtime/fixture builds | Original run `43105039523`, job `130806058225`; subsequent GitHub outcome must be checked on the new commit |
| A 64 KiB replacement plus 64 KiB expected bytes, both hex encoded, could not fit the 256 KiB request frame | Explicit 32 KiB write cap, separate from 64 KiB read/search cap; CLI/MCP docs and advertised capabilities agree | Contract verifies maximum request fits; independent TLS peer writes/reads/restores 32 KiB in the fixture and checks oversized refusal |
| Authenticated replies were insufficiently checked before GUI/client consumption | Shared capability/result validators reject wrong types, missing flags, duplicate identities, overlapping/out-of-range maps, inconsistent counts/cursors and forged scan addresses | Pure negative contracts and independent hostile TLS server using a valid certificate |
| An unexpected exception after write dispatch could be described as definitely not written | Backend tracks whether a write was attempted; dispatcher treats unexpected post-dispatch exceptions conservatively | Refusal paths remain `not_written`; lost or invalid write responses remain `may_have_changed` and never recommend automatic retry |
| A generated credential directory received its ignore guard only after success | Create `.gitignore` before keys; isolate provisioning from global OpenSSL config paths | Forced provisioning failure retains the guard; Windows OpenSSL with missing compiled-in config now provisions through the explicit local config |
| Remote MCP schema generation lived in the large host translation unit | Move definitions to `mcp_bridge/ps5_tools.cpp`, keeping a small declaration header | Desktop relinks; all full-client MCP tests pass |

The native backend additionally rechecks the expected process name while the
process is stopped, rather than relying solely on its pre-attach value. TLS
private-key temporary buffers use the library's non-optimizable zeroization.

## Added functionality

**Bounded pattern search.** `scan` / `cortex_ps5_scan` reads one explicitly
selected range, up to 64 KiB, and searches fixed-byte/`??` patterns. Patterns
contain at most 256 bytes and at least one fixed byte. Results have ordered
addresses, offsets, exact total count and an explicit result cap. No whole-process
scan, repeated attach loop, write, or atomic snapshot claim is introduced.

**Revision-bound map pages.** Maps return `next_offset`, `map_revision` and counts.
Continuation requires the same revision; changes anywhere in the described map
invalidate it. The revision includes target identity, ranges, backing offsets,
permissions and bounded backend names. A Python `hashlib` implementation checks
the canonical encoding independently of the C++ hash code.

**Exact peer certificates.** New profiles pin the server certificate and allowlist
the client leaf fingerprint. A valid certificate from the same CA but outside the
allowlist is rejected before the application hello. CA/name/purpose/expiry checks
remain required. Existing profiles without the optional fields retain their
previous dedicated-CA trust; they are not silently upgraded or rewritten.

The desktop exposes search, clickable matches and next-page controls; profile,
process and stale-generation transitions clear incompatible state and write
permission. CLI and MCP use the same implementation. Native scans have the same
explicit temporary-ptrace-stop effect as native reads.

## Observed local verification

Both independent Windows builds used llvm-mingw/Clang 23.1.5 with ASan/UBSan.
The desktop was built separately with its configured GNU toolchain.

| Check | x64 | x86 |
|---|---|---|
| Protocol contracts | 49 passed | 49 passed |
| Deterministic bounded protocol mutations | 10,000 completed | 10,000 completed |
| New query/response contracts | 76 passed | 76 passed |
| Existing own-client/own-agent E2E | 42 passed | 42 passed |
| New hardening/query E2E | 41 passed | 41 passed |
| Remote CTest suites | 4/4 passed | 4/4 passed |
| Previous portable-format CTest suites | 6/6 passed | 6/6 passed |

The final desktop executable also passed:

- 51 existing remote CLI/MCP checks and 46 hardening/query CLI/MCP checks.
- 38 wave-1 and 129 additional-format CLI/MCP checks; analyzed fixtures were
  not executed and their bytes were not changed.
- Application smoke over 33 workspaces / 9 presets and the Win32/D3D11 window
  smoke. This establishes rendering/startup, not a mouse-driven real-console run.

All remote E2E traffic used loopback and the agent's explicit fixture backend.
Large test writes modify and restore only that owned byte vector. An independent
Python TLS peer supplies protocol/hash references and malformed replies.
The first full desktop build exceeded its launcher timeout; its compiler state
was inspected before resuming. The subsequent complete build and tests passed.

The updated native `cortex-agent.elf` cross-linked with SDK v0.45. Symbol inspection
found the native backend and no fixture backend. Build hashes for this validation:

```text
cortex.exe       2892f9f6a5d352412e3268747c3c396469234fdc837d0f484ad5d4590559190f
cortex-agent.elf 557790c46d892f4dd03268616ea55f8b7ae7d295c7713c5626799e8008389a81
```

## CI and reproduction

The dedicated workflow now adds independent Windows x64/x86 jobs, alongside
Linux ASan/UBSan plus coverage-guided fuzzing and the PS5 cross-build. The fuzzer
corpus now includes complete query and result structures, not only empty
argument objects. Workflow YAML was parsed locally. GitHub job outcomes are
separate evidence: consult PR #44 and the exact head commit; local results above
do not establish that a newly pushed workflow passed.

```text
cmake -S tools/remote -B build/remote -G Ninja -DCORTEX_REMOTE_SANITIZERS=ON
cmake --build build/remote
ctest --test-dir build/remote --output-on-failure
python tests/remote_hardening_e2e.py --client <cortex.exe> --agent <cortex-agent.exe> --cortex --work <ignored-directory>
```

Logs and freshly generated credentials are local under ignored build directories;
only source, public deterministic seeds and documentation belong in the commit.

## Still not established

No physical PS5 or firmware was tested. Native ABI/permissions, entropy, real
ptrace stop/read/write/detach behavior and hardware timing need console validation.
Maps are metadata consistency checks, not a frozen process; memory can change
between calls. There is still no breakpoint/register/step channel or full parity
with the local Windows scanner. Neither the unit counts nor a short fuzz campaign
prove the absence of vulnerabilities. Automatic retries of uncertain writes
remain deliberately excluded.

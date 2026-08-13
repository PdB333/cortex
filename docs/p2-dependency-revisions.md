# P2 dependency revision audit

This file records the immutable commits currently referenced by Cortex's dependency versions.
It is an audit baseline, not yet a replacement for the `GIT_TAG` values in the root CMake file.
A future CMake cleanup can switch the remaining version tags to these commits without guessing which
revision a release tag resolved to during the P2 hardening pass.

| Dependency | Declared version | Verified commit |
| --- | --- | --- |
| Dear ImGui | v1.90.9 | `cb16be3a3fc1f9cd146ae24d52b615f8a05fa93d` |
| MinHook | commit | `d94c64d32ea37bc4f5ee47d580709f70c6fb6080` |
| cpp-httplib | v0.15.3 | `5c00bbf36ba8ff47b4fb97712fc38cb2884e5b98` |
| nlohmann/json | v3.11.3 | `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03` |
| Zycore | v1.5.0 | `74620eefd233bec20daeb66e78e744ff06e273b7` |
| Zydis | v4.1.0 | `569320ad3c4856da13b9dbf1f0d9e20bda63870e` |
| Lua | v5.4.7 | `1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3` |
| stb | commit | `31c1ad37456438565541f4919958214b6e762fb4` |

## P2 follow-up rule

When the root build is split into smaller CMake modules, use immutable commit SHAs for every
`FetchContent_Declare`. Remove `GIT_SHALLOW TRUE` for declarations pinned to commits when needed by
Git to make the exact object reachable. The x86/x64 build matrix must pass after each pin migration.

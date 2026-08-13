# P3 runtime validation

P3 moves Cortex from build-level confidence toward repeatable runtime evidence across renderers,
architectures, and failure modes.

## Evidence levels

A target is tracked at one of four levels:

1. **build** — the fixture and Cortex compile for the architecture.
2. **launch** — the fixture creates its renderer/device/context and stays alive on CI.
3. **instrumented** — `cortex_core.dll` can be injected and `/health` reports the expected hook.
4. **capture** — the renderer produces a valid screenshot through Cortex and survives shutdown.

A renderer is not considered validated merely because its hook source compiles.

## Current automated matrix

| Target | x86 | x64 | Current evidence |
|---|---:|---:|---|
| Win32 generic fixture | yes | yes | instrumented E2E |
| D3D9 fixture | yes | yes | capture E2E |
| D3D11 fixture | yes | yes | capture E2E |
| OpenGL fixture | yes | yes | P3 build + launch smoke test |
| D3D8 fixture | planned | n/a | x86-only backend |
| D3D12 fixture | n/a | planned | x64-only backend |

The OpenGL fixture is `test_target/main_opengl.cpp`. It deliberately uses only Win32/WGL/OpenGL 1.x
entry points so it can create a deterministic context on a stock Windows CI runner without a third-
party windowing library.

## P3 next increments

- promote OpenGL from **launch** to **capture** by injecting Cortex and validating a PNG screenshot;
- add a D3D8 x86 fixture and capture scenario;
- add a D3D12 x64 fixture and capture scenario;
- add a non-destructive diagnostics probe reporting process liveness, shared diagnostic channel,
  target/host bitness, window responsiveness, and heartbeat age;
- record renderer/backend information in validation evidence so failures can be attributed to hook
  installation versus capture;
- keep a small representative real-application matrix outside CI and record incompatibilities rather
  than silently excluding them.

## Acceptance rule

P3 changes should not promote a matrix cell to a stronger evidence level unless the corresponding
x86/x64 CI scenario actually executes that behavior. Unsupported combinations such as D3D8 x64 and
D3D12 x86 stay explicit instead of being treated as missing tests.

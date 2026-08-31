# Unified Cortex application architecture

Cortex is one user-facing application. The historical host, bridge, injector, ASI and injected DLL are implementation details or compatibility paths, not separate products.

## Product rule

The user launches **Cortex**. Target discovery, attach/detach, UI, MCP, CLI, projects, permissions, diagnostics and orchestration belong to the application. In-process code is loaded automatically only when a capability genuinely requires execution inside the target.

A feature is not removed during migration unless its replacement is available from the unified application and covered by validation.

## User-facing runtime

`cortex.exe` is the Windows product surface and also owns MCP stdio mode. The portable bundle carries architecture-specific instrumentation assets internally so same-bitness and cross-bitness targets can be handled without asking the user to operate a second program.

The same application services feed Qt, MCP and command-line paths. UI code must not duplicate business logic or loop through HTTP merely to reuse a feature.

## UI

The official human UI is Qt 6 + Qt Quick/QML. It is a dense IDE/debugger workspace with persistent navigation, target context and explicit observe/mutate state.

Current dedicated workspaces include memory, scans, pointers, disassembly/CFG/xrefs, structures, modules, symbols, snapshots, debugger, breakpoints, traces, patches, watches/freezes, instrumentation hooks, network, screenshots, diagnostics, scripts, input, actions, projects, sessions, MCP and semantic tools.

Generic MCP execution remains intentionally available only in the MCP workspace. Feature workspaces use dedicated controllers/services.

## Runtime layering

```text
Qt/QML UI        MCP stdio        CLI
      \             |             /
       +------ application services ------+
                       |
                session / target model
                       |
                 capability layer
                       |
                    backends
             /          |          \
         external   instrumentation   remote
                       |
                     target
```

Core concepts are platform-neutral: `Target`, `Node`, `Backend`, `Catalog`, `Architecture` and `Capabilities`. Windows is the concrete production runtime today; Linux is progressively implemented through the same contracts, while PS4 remains a future backend rather than a separate product architecture.

## MCP

MCP is integrated into `cortex.exe`. The normal local path is stdio -> authenticated Named Pipe -> runtime executor; it does not loop back through HTTP.

Primitive and semantic execution share the same route/executor contracts used by the application. Mutating/control/native operations require explicit mutation permission. Semantic execution additionally supports bounded execution, cancellation, evidence and transactional rollback where a safe compensation contract exists.

Human prompt answering is kept off the public MCP tool surface. The Desktop communicates with prompt state over a private local channel so an agent cannot answer its own human-verification prompt.

## Instrumentation boundary

Renderer/input/debug hooks remain only for capabilities that need in-process execution. The payload is not a second application.

Dear ImGui is no longer a Cortex dependency. Human prompts are presented by the Qt Desktop over the authenticated private channel. If no Desktop presenter is available, prompt creation fails explicitly instead of opening an injected fallback window. Paused-thread recovery is handled by the Qt debugger or explicit headless debugger APIs.

Renderer/capture instrumentation remains independent from the human UI. Present/SwapBuffers hooks only provide capture points, target-window discovery and game-thread work pumping; they do not create or render an injected UI context and do not subclass the target WndProc for Cortex UI input.

## Persistence and safety

Projects persist named addresses, pointer paths, notes and structure definitions. The Desktop also persists workspace state. State-changing operations are separated visually and technically from observation, require Mutation permission, and feed the action journal when reversible.

## Validation

The branch `next/unified-cortex-ui` is validated through:

- Windows x64 application build and QML smoke;
- Windows x64 and x86 instrumentation/runtime builds;
- integrated MCP E2E against x64 and x86 targets;
- Qt private-channel E2E for human prompts and runtime events;
- portable clean-PATH GUI smoke;
- Linux Qt application build/QML smoke;
- focused MCP/schema/contract tests.

The preview workflow produces a testable portable Cortex artifact but does not publish a GitHub Release.

## Current completion state

The unified Windows product surface, Qt/QML workspaces, native MCP integration, x64/x86 portable packaging and removal of the historical Dear ImGui dependency are complete on the migration branch and covered by automated gates.

Remaining work is release/validation work rather than a second application migration:

1. Manually test the portable Windows application against representative authorized real targets, including x64 and x86 where possible.
2. Fix runtime/UX defects found by real-target validation.
3. Continue Linux runtime parity through the common target/backend contracts.
4. Implement the future PS4 backend through the same product architecture rather than creating a separate UI/product.
5. Only after explicit manual approval: merge to `master`, update release packaging and publish a unified release.

No merge to `master` and no release publication is part of the migration branch workflow before that approval.

# Unified Cortex application architecture

This branch is the migration path from the historical injected-DLL-centered product to one user-facing Cortex application.

## Product rule

The user launches **Cortex**. The UI, MCP server, CLI entry points, sessions, projects, target selection and orchestration belong to the application. Injected code remains an internal instrumentation mechanism only where a capability genuinely requires execution inside a target.

The migration must preserve the complete feature set. A historical feature is not removed from the injected runtime until its replacement path is available from the application and covered by tests.

## UI

The application UI is Qt 6 + Qt Quick/QML. Dear ImGui is not part of the target architecture for the human-facing product.

Visual direction:

- dense IDE/debugger workspace inspired by Visual Studio Code;
- dark neutral surfaces with a single blue Cortex accent;
- monospace rendering for addresses, bytes, registers, disassembly and logs;
- orange reserved for mutation/risk state, red for errors and breakpoints;
- panels, tabs, command palette and a persistent target/status bar;
- capability-aware navigation rather than platform-specific UI branching.

## Runtime layering

```text
Qt/QML UI       MCP       CLI
      \          |        /
       +---- application services ----+
                    |
              target/session model
                    |
               capability layer
                    |
                 backends
          /          |          \
      external   injected     remote
                    |
                  target
```

The application must not call old HTTP routes merely to reuse business logic. REST, MCP and QML are adapters over shared services/executors.

## ImGui removal rule

The injected runtime currently owns status UI, prompts and paused-thread controls through Dear ImGui. Removal is staged:

1. reproduce the control/visibility path in the Qt application;
2. expose required state and commands through shared services/IPC;
3. keep renderer hooks only when required for capture/instrumentation;
4. remove ImGui input capture, WndProc UI handling and rendering;
5. remove Dear ImGui from the build once no runtime feature depends on it.

Renderer hooks are not automatically removed with ImGui. Screenshot, frame capture and other instrumentation capabilities must continue to work.

## Preview milestone

`app/` is intentionally buildable as a standalone Qt target while the legacy runtime remains untouched. The preview proves the new application shell, local target discovery, target selection, mutation-state UX, workspace navigation and command palette before service migration begins.

The workflow `.github/workflows/ui-preview.yml` builds a Windows x64 portable test artifact from `next/unified-cortex-ui`. It never creates a GitHub Release. Public publication happens only after manual testing and an explicit merge/release decision.

## Migration sequence

1. Application shell and target selection.
2. Session/target manager backed by the existing common model.
3. Shared application services for status, modules and read-only memory.
4. Memory/scanner/disassembly UI.
5. Debugger, watches, breakpoints and traces.
6. MCP activity/session UI and shared executor integration.
7. Mutation journal, permissions and rollback UI.
8. Prompt and paused-thread controls moved out of ImGui.
9. Instrumentation IPC boundary made explicit.
10. Remove ImGui rendering/input code while retaining required renderer instrumentation.
11. Fold historical host/bridge/injector entry points behind the unified application/CLI surface.
12. Add Linux/remote backends through the same target/capability contracts.

At every phase, existing release functionality remains available until the replacement has equivalent coverage.

# P4 generic targets, capabilities and nodes

P4 starts the architectural shift from a Windows-game-centric runtime toward a generic Cortex
controller that can describe software targets consistently across machines and platforms.

## Core rule

The public Cortex model should talk about a **target** and the capabilities available on that target,
not about the OS primitive used to implement the operation.

Examples of stable concepts:

- `process.info`
- `modules`
- `memory.read`
- `memory.scan`
- `debug`
- `trace`
- `symbols`
- `network.observe`
- `window.capture`
- `diagnostics`

A backend can expose only the capabilities it actually supports. Clients must not infer capabilities
from the platform name.

## Node model

A `NodeDescriptor` represents the machine or device that owns targets. The initial model distinguishes
local and remote transports without implementing a remote control protocol yet.

The intended progression is:

1. Windows local node and generic Windows application targets.
2. Multiple authorized Windows nodes.
3. Linux node/backend using the same target contract.
4. Experimental PS4 node/backend on a controlled jailbreak/homebrew environment.

Platform-specific APIs stay behind the backend. They must not leak into the generic target identity.

## Target identity

Process targets use a platform-qualified identity of the form:

```text
<node-id>:<platform>:process:<process-id>
```

This is deliberately independent from process names because names are not unique and can change.
Persistent cross-restart identity will require higher-level selectors later (module fingerprint,
executable path/hash, project binding, or platform-specific application identifiers).

## P4 first acceptance checks

- target/capability model compiles as plain C++17 with no Win32 dependency;
- the same enums represent Windows, Linux and PS4;
- local nodes and process targets have deterministic IDs;
- x86/x64 CI executes the model contract tests;
- mutation support remains a capability, not an assumption attached to every target.

## Next P4 increments

- add JSON serialization for nodes/targets;
- add a Windows local-node enumerator;
- describe current Cortex process capabilities through the generic model;
- expose read-only target discovery through REST/MCP;
- introduce a backend interface after the Windows descriptor path is stable;
- only then add remote-node transport and Linux implementation.

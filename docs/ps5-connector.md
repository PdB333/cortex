# Our Cortex agent for PS5 (experimental)

Cortex now implements **both ends**: the PC client and the native agent, using
our [Cortex Remote protocol](cortex-remote-protocol.md). The former ps5debug-NG
client has been removed. No command IDs, framing, server, library or fallback
from that debugger is used.

The agent is built as a PS5-targeted ELF with the open PS5 SDK. It still needs
an operator-provided environment that can run it and permits the requested OS
operations. This change does not provision that environment, alter firmware or
install a jailbreak. Building an ELF does **not** establish firmware compatibility.

## Delivered implementation

- Mutual TLS authentication, encrypted transport, dedicated CA and separate
  agent/client keys. No default credentials or insecure fallback.
- Our own framed, versioned protocol, strict JSON schema, fresh connection
  session IDs, bounded payloads/deadlines and no automatic write retry.
- Native process identities from PID and process start time, tied to a random
  agent instance. Reusing a token after process/agent restart is refused.
- Agent-side PID allowlist, process names and memory maps through `sysctl`.
- Up to 64 KiB per memory request using native `ptrace` I/O. The agent must
  explicitly permit the temporary process stop used by this backend.
- Writes require permissions on both ends, per-call confirmation, current
  target identity, expected name and bytes, and post-write readback.
- Shared GUI, CLI and MCP implementation; a standalone agent/client build.

The agent checks SDK structure sizes and does not guess firmware offsets when
an ABI differs. Permission or ABI failures are reported; no privilege changes
are attempted. Native backend source: `agent/remote/backend_freebsd.cpp`.

## Build on the PC

```powershell
cmake -S tools/remote -B build/remote -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/remote
```

This builds `cortex-remote` and `cortex-agent` for the build host. On Windows the
agent refuses native mode; `--fixture` explicitly selects the test-vector
backend. It never identifies that backend as a PS5. The desktop build includes
the same client via `cmake/CortexRemote.cmake`.

## Cross-build the PS5 agent

Install the open [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)
separately. The checked configuration uses SDK **v0.45**, archive SHA-256
`5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c`.
Use an LLVM installation containing Clang, `ld.lld`, `llvm-ar` and `llvm-ranlib`.

```powershell
cmake -S tools/remote -B build/ps5 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/cortex/cmake/ps5-clang-toolchain.cmake `
  -DCORTEX_PS5_SDK=C:/path/to/ps5-payload-sdk `
  -DCORTEX_LLVM_BIN=C:/path/to/llvm/bin `
  -DCORTEX_REMOTE_PS5=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/ps5 --target cortex-agent
```

Result: `build/ps5/cortex-agent.elf`. The PS5 target excludes the fixture backend.
Its SDK startup/runtime and platform symbol stubs are external build dependencies,
not code imported from a debugger. SDK-linked binary distribution has separate
licence obligations; see [third-party notices](third-party.md). No such binary is
automatically published by CI.

## Provision distinct identities

OpenSSL is used only for generating credentials, not at runtime:

```powershell
python tools/remote/create_credentials.py `
  --out C:/Cortex/private-remote `
  --endpoint 192.168.1.50:47731 --platform ps5
```

The output directory must be new. It contains:

| Directory | Owner / purpose |
|---|---|
| `authority/` | Dedicated CA private key; keep offline, do not copy to console |
| `agent/` | Agent certificate, private key, CA certificate, `agent.json` |
| `client/` | Separate client certificate, private key, CA certificate, `client.json` |

Restrict filesystem permissions to the operator (including Windows ACLs).
Never commit generated keys or reuse the test certificates. Certificates default
to 30 days; plan their renewal and make sure both clocks are correct.

Edit `agent.json` locally before deployment: choose the listening address,
`allowed_pids` (maximum 256), and `allow_ptrace_stop` when authorizing live memory
reads. `allow_writes` defaults to false. The PC profile also defaults to false.

On the console, the agent accepts `--config <path>`; with no arguments, its PS5
build reads `/data/cortex/agent.json`. Copy only the agent identity/configuration
and the separately built ELF through your existing authorized environment.
This repository does not install or start software on your console automatically.

## Desktop

Open **AI Investigation > PS5 (experimental)**, select the path to `client.json`,
then **Probe Cortex agent** and **List remote processes**. Selecting a process
records its exact target ID. Maps and reads use that ID. The write checkbox is
session-local and a confirmation dialog is required; profile and agent policy
are enforced separately.

Changing the profile clears the selected target and write permission. Remote
addresses/PIDs are not inserted into the local Windows debugger sessions.

## CLI and MCP

```powershell
.\cortex.exe ps5 info --config C:/Cortex/private-remote/client/client.json
.\cortex.exe ps5 processes --config C:/Cortex/private-remote/client/client.json
.\cortex.exe ps5 maps --config C:/Cortex/private-remote/client/client.json `
  --pid 42 --target-id "<exact target_id returned by processes>"
.\cortex.exe ps5 read --config C:/Cortex/private-remote/client/client.json `
  --pid 42 --target-id "<exact target_id>" --address 0x100000 --length 64
```

Addresses and PIDs above are illustrative. A write additionally requires
`--allow-writes --confirm-write`, `--expected-name`, `--expected-hex` and `--hex`.
The two hex strings must have identical nonzero length.

For MCP, configure the profile at startup:

```json
{
  "mcpServers": {
    "cortex": {
      "command": "C:/Cortex/cortex.exe",
      "args": ["mcp", "--ps5-config", "C:/Cortex/private-remote/client/client.json"]
    }
  }
}
```

Tools remain `cortex_ps5_info`, `cortex_ps5_processes`, `cortex_ps5_maps`,
`cortex_ps5_read` and `cortex_ps5_write`. Tools cannot select a different endpoint,
credential, profile or server name. No profile means no connection. MCP writes
also require the server's `--allow-writes` and the call's `mutation_permission`.

The previous `--ps5-endpoint` option is deliberately unsupported. It cannot
silently connect to the old third-party server.

## Validation and remaining limits

The C++ client and C++ agent are tested together on localhost with the explicitly
selected fixture backend; a separate Python TLS peer supplies independent framing
and authentication checks. There are also pure decoder tests, deterministic
mutations and a coverage-guided harness. See the recorded verification results
in [Cortex Remote validation](validation/cortex-remote.md).

**No physical PS5 has been tested.** SDK cross-compilation/linking is established
separately from runtime compatibility. Firmware-specific process metadata or
permissions may make the native backend refuse requests. A kernel call may
outlive the network deadline. Reads are not zero-effect: ptrace briefly stops
the process. A failed detach reports `target_may_be_stopped` and quarantines it.

Not implemented: kernel access, register/step/breakpoint channels, Remote Play,
controller input, remote function execution, code loading, or remote parity for
the Windows scanner/freeze panels. Compare/write/readback is not globally atomic;
`may_have_changed` must never be interpreted as an unchanged target or retried
blindly. The protocol document specifies the complete failure semantics.

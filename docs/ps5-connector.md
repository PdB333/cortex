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
  agent/client keys, server certificate pin and agent-side client certificate
  allowlist in newly generated profiles. No default credentials or insecure fallback.
- Our own framed, versioned protocol, strict JSON schema, fresh connection
  session IDs, bounded payloads/deadlines and no automatic write retry.
- Native process identities from PID and process start time, tied to a random
  agent instance. Reusing a token after process/agent restart is refused.
- Agent-side PID allowlist, process names and memory maps through `sysctl`.
- Up to 64 KiB per read/search and 32 KiB per write using native `ptrace` I/O.
  The agent must explicitly permit the temporary process stop used by this backend.
- Bounded byte-pattern searches with `??` wildcards, exact total matches and
  a capped result list. One read per query, not a whole-process scan.
- Memory-map pagination tied to a SHA-256 map revision, so changed layouts
  cannot silently be combined across pages.
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

## Certificate pinning and renewal

Newly generated `client.json` includes `server_certificate_sha256`, the lowercase
SHA-256 of the agent leaf certificate in DER form. `agent.json` includes
`allowed_client_sha256`, initially containing the one generated client leaf
fingerprint. The server accepts at most 32 distinct fingerprints and rejects a
CA-valid but unlisted client before sending application data. Pinning supplements,
not replaces, CA validation, validity dates, key purpose and server-name checks.

For deliberate renewal, update the peer fingerprints through the operator's
configuration workflow; restart the agent to reload its trust and allowlist.
The CA private key is not needed at runtime. Do not remove checks to work around
an expired or replaced certificate.

**Compatibility:** both new fields are optional for pre-existing configurations.
An absent pin/allowlist retains dedicated-CA trust, not certificate-specific trust.
An explicitly empty or malformed allowlist is rejected. Existing profiles are
not silently rewritten. The provisioning script writes its `.gitignore` before
any key generation and uses its own minimal OpenSSL configuration.

## Desktop

Open **AI Investigation > PS5 (experimental)**, select the path to `client.json`,
then **Probe Cortex agent** and **List remote processes**. Selecting a process
records its exact target ID. **Read first map page** and **Next map page** retain
the map revision. **Search this memory range** accepts a pattern such as
`48 8b ?? 00`; selecting a match copies its address into the read field.
Maps, reads and searches use the selected target ID. The write checkbox is
session-local and a confirmation dialog is required; profile and agent policy
are enforced separately.

Changing the profile or selecting a different process clears the selected map
cursor, expected bytes and write permission. A refresh that no longer finds the
selected target also clears it. Remote
addresses/PIDs are not inserted into the local Windows debugger sessions.

## CLI and MCP

```powershell
.\cortex.exe ps5 info --config C:/Cortex/private-remote/client/client.json
.\cortex.exe ps5 processes --config C:/Cortex/private-remote/client/client.json
.\cortex.exe ps5 maps --config C:/Cortex/private-remote/client/client.json `
  --pid 42 --target-id "<exact target_id returned by processes>"
.\cortex.exe ps5 read --config C:/Cortex/private-remote/client/client.json `
  --pid 42 --target-id "<exact target_id>" --address 0x100000 --length 64
.\cortex.exe ps5 scan --config C:/Cortex/private-remote/client/client.json `
  --pid 42 --target-id "<exact target_id>" --address 0x100000 --length 4096 `
  --pattern "48 8b ?? 00" --max-matches 64
.\cortex.exe ps5 maps --config C:/Cortex/private-remote/client/client.json `
  --pid 42 --target-id "<exact target_id>" --limit 64 `
  --offset 64 --expected-revision "<map_revision from the previous page>"
```

Addresses and PIDs above are illustrative. A write additionally requires
`--allow-writes --confirm-write`, `--expected-name`, `--expected-hex` and `--hex`.
The two hex strings must have identical nonzero length, at most 32 KiB each.
This lower write cap leaves room for both hex strings inside the 256 KiB frame.
Reads and searches still support 64 KiB. The desktop write editor currently
accepts 256 bytes; the CLI/MCP limit is larger.

Use the returned `next_offset` rather than assuming a page size. A null cursor
ends pagination; `map_revision_changed` means restart at offset zero. A revision
describes map metadata, not an atomic process snapshot. Search results include
`total_matches`, `truncated` and offsets relative to the requested address;
patterns require at least one fixed byte and support up to 256 bytes.

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
`cortex_ps5_read`, `cortex_ps5_scan` and `cortex_ps5_write`. Tools cannot select a different endpoint,
credential, profile or server name. No profile means no connection. MCP writes
also require the server's `--allow-writes` and the call's `mutation_permission`.

The previous `--ps5-endpoint` option is deliberately unsupported. It cannot
silently connect to the old third-party server.

## Validation and remaining limits

The C++ client and C++ agent are tested together on localhost with the explicitly
selected fixture backend; a separate Python TLS peer supplies independent framing
and authentication checks. There are also pure decoder tests, deterministic
mutations and a coverage-guided harness. See the recorded verification results
in [Cortex Remote validation](validation/cortex-remote.md) and the
[hardening and query report](validation/cortex-remote-hardening.md).

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

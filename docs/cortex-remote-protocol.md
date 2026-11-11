# Cortex Remote Protocol 1

Cortex owns both endpoints and this application protocol. It is not compatible
with ps5debug, ps5debug-NG or their command IDs and has no fallback to them.
TCP and TLS use maintained libraries rather than custom cryptography.

## Transport and credentials

Every connection uses mutual TLS 1.2. Both peers require a certificate chaining
to the operator's dedicated CA. The client additionally validates the configured
server DNS name against the certificate; the TCP endpoint is numeric IPv4.
Only ECDHE-ECDSA/RSA with AES-256-GCM is negotiated. There is no plaintext mode,
certificate-verification bypass, shared default password, discovery or session
resumption. Certificate files are provisioned separately and never accepted as
MCP tool arguments. Keep the CA private key offline and restrict key-file ACLs.

The server's single-client processing loop bounds resource use, not denial of
service from a reachable network. An unauthenticated peer can occupy a handshake
until its timeout. Deploy on a trusted/restricted network, not a public listener.
Certificate expiry requires a correctly set clock. Revocation lists and live
certificate reloading are not implemented: remove/replace trust material and
restart the agent when revoking access.

## Frame

Exactly 16 header bytes, followed by a UTF-8 JSON object:

| Byte offset | Width | Meaning |
|---|---|---|
| 0 | 4 | ASCII `CRTX` |
| 4 | 1 | Protocol version, `1` |
| 5 | 1 | Kind: hello `1`, request `2`, response `3` |
| 6 | 2 | Reserved flags, both zero |
| 8 | 4 | Unsigned sequence ID, big-endian |
| 12 | 4 | Unsigned body length, big-endian, 1..262144 |

Reads accumulate fragmented TLS records until the declared length is reached.
A disconnect before then is an error. JSON nesting is limited to 32, object
keys must be unique, and invalid UTF-8, non-object roots and unknown envelope
fields are rejected. Protocol/parser diagnostics do not echo malformed input.

## Connection state machine

1. TCP connection and mandatory mutual TLS authentication.
2. Agent sends hello, sequence 0. It contains `schema=cortex.remote.hello.v1`,
   `protocol=cortex-remote`, `version=1`, its random 128-bit instance ID,
   a fresh random 128-bit session ID, platform and backend capabilities.
3. Client verifies the expected platform from its operator-controlled profile.
4. Client sends **one** request, sequence 1, echoing instance and session.
5. Agent validates it, executes at most one action, sends response sequence 1
   with the same IDs, then closes the connection.

A captured request from a previous connection fails the session check. Repeated
requests within a connection are not processed. An operator can deliberately
submit a new action, but neither client nor agent automatically retries writes.
TLS-level integrity and authentication remain essential: the IDs are not a
substitute for cryptographic protection.

Request:

```json
{
  "schema": "cortex.remote.request.v1",
  "session": "<32 lowercase hex characters>",
  "instance": "<32 lowercase hex characters>",
  "action": "read",
  "arguments": {
    "pid": 42,
    "target_id": "<exact value returned by processes>",
    "address": "0x100000",
    "length": 64
  }
}
```

Response uses `schema=cortex.remote.response.v1`, `session`, `instance` and
`result`. `result.ok` is a Boolean. Failures have a bounded `error` string.
The client validates successful process lists, mappings, memory length and
requested target identity rather than blindly trusting arbitrary response JSON.

## Actions

| Action | Arguments | Behavior |
|---|---|---|
| `info` | none | Actual backend/platform/capabilities |
| `processes` | optional limit 1..256 | Agent's configured process allowlist and current target IDs |
| `maps` | pid, target_id, optional limit | Bounded user-space mappings |
| `read` | pid, target_id, address, length | 1..65536 bytes as hex |
| `write` | pid, target_id, address, hex, expected_hex, expected_name, mutation_permission | Compare, write, read back |

`target_id` combines the random agent-instance ID and the native PID/start-time
identity. An agent or process restart invalidates it. It is a concurrency guard,
not a secret or permission. The fixture backend identifies itself as `fixture`
and uses a test-vector generation; a client expecting `ps5` rejects it.

Writes require **all** of: the local caller's write authority, client profile
`allow_writes`, agent `allow_writes`, and the call's `mutation_permission=true`.
The native backend additionally requires explicit `allow_ptrace_stop` for both
reads and writes. Each call is revalidated on the agent, including the PID
allowlist, current target ID, expected name and pre-write bytes.

The server does not accept remote configuration changes, process creation,
module loading, function calls, kernel addresses, permission escalation,
code execution, breakpoint channels or file reads through this protocol.

## Native effects and failures

The FreeBSD/PS5 backend uses ordinary `sysctl`, `ptrace`, `waitpid` and socket
interfaces under its **existing** OS privileges. Reading memory briefly stops
and detaches the process. It refuses an already stopped/traced target and checks
the generation again after attaching. A reused PID can be momentarily stopped
before that second check; it is not read or written when the identity differs.
An unexpected stop signal is preserved when detaching. A failed detach
quarantines the PID in the agent and reports `target_may_be_stopped`.

The client and server serialize their own calls. Other debuggers/threads can
still race: compare/write/readback is not a globally atomic transaction.
Kernel calls are not forcibly interrupted by the network deadline; a client
timeout does not prove the agent abandoned a write.

- `not_written`: the server established that no write occurred.
- `readback_matched`: the write was followed by matching readback.
- `may_have_changed`: a partial write, lost response, failed readback or other
  uncertainty prevents proving that memory is unchanged.

The client conservatively reports `may_have_changed` after attempting to send a
write when no valid response arrives. All write results have `retry_safe=false`.
There is no implicit rollback. Confirm the target's actual state before a new
operator-approved action.

## Test scope

`tests/remote_agent_e2e.py` exercises the real C++ client and real C++ server
against its explicitly selected in-process fixture backend. An independent
Python TLS/struct peer tests fragmentation, authentication, malformed frames,
replay rejection and dropped write responses. `remote_protocol_tests.cpp`
provides contract checks and deterministic mutations; `fuzz_remote_protocol`
is a separate coverage-guided harness with no sockets or target access.

Cross-linking `cortex-agent.elf` against the PS5 SDK is a build check, not evidence
that any specific firmware permits these APIs. Real console validation remains
required and must be reported separately.

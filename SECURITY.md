# Security policy

Cortex reads and, with permission, changes other processes. A flaw in it can
let a client do more than the operator allowed, so these reports matter.

## Reporting a vulnerability

Please **do not open a public issue** with the details.

1. Use GitHub's private vulnerability reporting for this repository
   (**Security › Report a vulnerability**).
2. If that is not available to you, open an issue that only asks for a
   private contact, without technical details.

Include the version or commit, Windows version, target architecture, the steps
or a proof of concept against the repository's test target, and what you
expected to happen. You will get an acknowledgement, and a fix or an
explanation, as soon as possible. Releases are published only after the
maintainer's explicit approval, so a fix may first land on a branch.

## What counts

In scope, for example:

- an MCP client, or a page talking to a local debug port, that changes a
  target **without** `--allow-writes`, or without `mutation_permission: true`
  in the call;
- a tool call that reads or writes files outside the roots the operator gave
  (`--artifact-root`, `--test-results`, the project folder);
- an MCP client that starts an executable or arguments not in the operator's
  launch profile;
- a live provider that evaluates code, runs a getter or method, or connects
  somewhere other than `127.0.0.1`;
- a crash, hang, or out-of-bounds read or write in Cortex caused by a crafted
  file (PE, .NET, ASAR, JS, ZIP, class files) or by data read from a target;
- data from a target (strings, memory, logs, file contents) being treated as
  instructions by Cortex itself.

Out of scope:

- anything that needs the operator to grant the authority in question first
  (for example writes after `--allow-writes`);
- the documented limit that the pipe token is a file the same Windows user can
  read: an agent that also has a shell or file access is not contained by
  Cortex;
- vulnerabilities in Ghidra, ILSpy, Vineflower, netcoredbg, debugpy, Node.js
  or the JVM themselves (report those upstream), unless Cortex makes them
  reachable when they otherwise would not be;
- requests to add anti-cheat, DRM or access-control bypasses. These are not
  vulnerabilities and will not be implemented.

## Design guarantees

See [docs/mcp.md](docs/mcp.md) (write authority, routing, the private
desktop channel), [docs/analysis-providers.md](docs/analysis-providers.md)
(external engines and live providers) and
[docs/automated-tests.md](docs/automated-tests.md) (launch profiles). Their
regression tests include `tests/security_no_writes_regression.py`,
`tests/mcp_authority_tests.cpp` and the live provider suites.

## Responsible use

Use Cortex only on software you own or are authorized to analyze. The
maintainers do not support using it to cheat in online games, defeat DRM or
licence checks, or access systems without permission.

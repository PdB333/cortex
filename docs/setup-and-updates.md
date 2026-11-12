# Installing, setting up and updating Cortex

Cortex comes two ways, and both update themselves:

- **The installer** `cortex-vX.Y.Z-windows-setup.exe`: a per-user install
  (no administrator rights) in `%LOCALAPPDATA%\Programs\Cortex`, with a Start
  menu entry and an uninstaller in Settings › Apps.
- **The portable archive** `cortex-vX.Y.Z-windows-portable.zip`: extract it
  anywhere and run `cortex.exe`.

Settings, layouts, recent files, evidence and investigations live in
`%LOCALAPPDATA%\Cortex` in both cases (or beside `cortex.exe` with a
`cortex.portable` file), so they survive updates and uninstalls.

## The installer

| Option (installer page) | What it does |
|---|---|
| Desktop shortcut | Off by default. |
| "Analyze with Cortex" in the right-click menu | Adds an entry for `.exe`, `.dll` and `.asar` files; Cortex opens the file in the Files layout. Per user; removed on uninstall. |
| Register Cortex in my AI clients | Runs `cortex setup --yes` once: Cortex is added, read-only, to the AI clients installed for you. |

Silent install for scripts: `cortex-vX.Y.Z-windows-setup.exe /VERYSILENT
/TASKS=openwith,setupclients` (`/DIR=<folder>` to choose the folder).

Uninstalling removes the program, the right-click entry, and the MCP entries
that start this copy (`cortex setup --remove --only-this-copy`). Entries that
start another copy (a portable one elsewhere) are kept. Your data in
`%LOCALAPPDATA%\Cortex` is kept.

## Setting up AI clients and engines

**Desktop:** Home › *Set up my AI clients automatically*, or Help/Tools ›
*Set up AI clients and engines...*. **Command line:** `cortex setup`.

Both detect, show and only then change:

1. **AI clients** installed for you, and whether Cortex is registered:

   | Client | Configuration file |
   |---|---|
   | Claude Desktop | `%APPDATA%\Claude\claude_desktop_config.json` |
   | Claude Code | `%USERPROFILE%\.claude.json` (user scope) |
   | Cursor | `%USERPROFILE%\.cursor\mcp.json` |
   | Windsurf | `%USERPROFILE%\.codeium\windsurf\mcp_config.json` |
   | VS Code (Copilot) | `%APPDATA%\Code\User\mcp.json` (`servers`, `type: stdio`) |
   | Gemini CLI | `%USERPROFILE%\.gemini\settings.json` |
   | Codex | `%USERPROFILE%\.codex\config.toml` (`[mcp_servers.cortex]`) |
   | LM Studio | `%USERPROFILE%\.lmstudio\mcp.json` |

2. **Analysis engines** where people usually put them: `GHIDRA_INSTALL_DIR`,
   `JAVA_HOME`, `PATH`, `%USERPROFILE%\.dotnet\tools`, and folders named
   `ghidra*`, `jdk*`, `vineflower*`, `netcoredbg*` in Downloads, Desktop,
   `%USERPROFILE%\tools`, Program Files (also JDK vendors: Java, Eclipse
   Adoptium, Microsoft, Zulu, BellSoft, Amazon Corretto) and `C:\tools`.
   What is not found gets its download link.

The entry written for each client is the same:

```json
{ "command": "C:\\...\\cortex.exe", "args": ["mcp"] }
```

plus, when found and allowed, `--ghidra-dir`, `--ilspycmd`, `--dotnet`,
`--vineflower`, `--netcoredbg` and a `JAVA_HOME` environment variable. It is
**read-only**: `--allow-writes` is added only when you tick *Let agents change
targets* (or pass `--allow-writes`), and each change still needs the agent's
`mutation_permission`.

Safety:

- Nothing is written before you click Apply, type `y` or pass `--yes`.
  `cortex setup --dry-run` prints the exact text that would be written.
- Each file that changes is first copied to
  `<file>.cortex-backup-<date-time>`.
- A configuration file that is not plain JSON (for example VS Code's
  `mcp.json` with comments) is never rewritten; the entry to add by hand is
  shown instead.
- Only the `cortex` entry is added, replaced or removed; the client's other
  servers and settings are kept as they are.
- The engines found are also saved for the desktop (Artifacts › Engines);
  paths you set there are kept. Artifacts › Engines › *Find installed
  engines* runs the same detection.

```
cortex setup                    detect, show the plan, ask
cortex setup --yes              apply
cortex setup --dry-run          show the exact changes, write nothing
cortex setup --client cursor    only that client (repeatable)
cortex setup --allow-writes     let agents change targets
cortex setup --no-engines       no engine paths
cortex setup --remove           remove Cortex from the clients
cortex setup --json             machine-readable report
```

Restart the AI clients after a change.

## Updates

**Desktop:** once a day at start-up (Help › *Check for updates...* turns it
off), Cortex reads the latest release of `PdB333/cortex` from the GitHub API.
That is one HTTPS request with no identifier but the program's version in
its user agent. When a newer version exists, an **Update to X.Y.Z** button
appears in the menu bar; the dialog shows the release notes and offers
*Download and install*, *Skip this version* and the release page.
**Command line:** `cortex update --check` (exit code 10 when an update
exists), `cortex update` (asks), `cortex update --yes`.

What an update does:

1. Downloads the asset for this copy: the installer for an installed copy,
   the portable archive for a portable one.
2. Downloads the release's `SHA256SUMS.txt` and compares the file's SHA-256.
   A mismatch deletes the file and stops.
3. **Installed copy:** checks that no other program uses files of the
   installation (an MCP server started from it, a target with its runtime
   loaded); otherwise lists them and stops. Then it runs the verified
   installer silently; the installer closes Cortex, replaces it and opens it
   again.
4. **Portable copy:** unpacks the archive into a new folder beside the current
   one (`cortex-vX.Y.Z`), which is kept; the dialog can start the new version.
   Entries that would leave that folder are refused.

`--feed <url|file>` or the `CORTEX_UPDATE_FEED` variable point at another
feed with the same shape (a mirror, or a local file for tests).

## Limits

- **The release is not code-signed yet.** Windows SmartScreen warns on the
  first run of the installer and of `cortex.exe`, and the update check proves
  that a download is the file the release published (its SHA-256), not who
  built it: the checksum comes from the same release. A code-signing
  certificate would add that.
- The installer is x64; Cortex still analyzes x86 targets through its x86
  runtime.
- Client configuration paths follow each client's documentation at the time
  of writing; a client that moves its file needs a Cortex update. Clients
  configured per project (a `.cursor/mcp.json` in a repository) are not
  touched.
- Engine detection looks in the usual places listed above, not the whole
  disk.

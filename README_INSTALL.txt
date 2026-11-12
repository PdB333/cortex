CORTEX v2.1.0 - INSTALLATION AND QUICK START
============================================

Cortex ships as a single portable Windows application: cortex.exe. The x64
and x86 runtimes live in the runtime folder and are selected automatically
for each target. Nothing else has to be installed (no Qt, no Visual C++
runtime). External analysis engines are optional (see below).

Use Cortex only on software and systems you own or are authorized to
inspect. Anti-cheat, DRM and access-control bypass, and interference with
online services, are outside the scope of this project.

INSTALLATION
------------

Either run the installer (per user, no administrator rights):

       cortex-v2.1.0-windows-setup.exe

  It installs into %LOCALAPPDATA%\Programs\Cortex, adds Cortex to the Start
  menu and, if you tick them, an "Analyze with Cortex" right-click entry and
  your AI clients' configuration. Uninstall from Settings > Apps.

Or use the portable archive:

1. Download cortex-v2.1.0-windows-portable.zip.

2. Extract the whole archive into a normal, writable folder, for example:

       C:\Cortex\

3. Do not move cortex.exe on its own. Keep the runtime folder next to the
   executable.

4. Run:

       .\cortex.exe

UPDATES
-------

Cortex checks for a new release once a day (Help > Check for updates...
turns it off) and installs it when you agree. The download is accepted only
if its SHA-256 matches the release's SHA256SUMS.txt. An installed copy runs
the new installer; a portable copy is unpacked into a new folder beside the
current one. From the command line: .\cortex.exe update --check

SET UP YOUR AI CLIENTS
----------------------

       .\cortex.exe setup

finds Claude Desktop, Claude Code, Cursor, Windsurf, VS Code, Gemini CLI,
Codex and LM Studio, shows what it would change and, once you agree, adds
Cortex to them read-only (each file is backed up first). It also finds
Ghidra, a JDK, ILSpy, Vineflower and netcoredbg. The desktop does the same
from Home > "Set up my AI clients automatically".

TWO WAYS TO USE CORTEX
----------------------

You drive it: the desktop (cortex.exe) and the command line.
An AI agent drives it: your MCP client starts "cortex.exe mcp". The server
is read-only unless YOU add --allow-writes to its arguments; every change
must also be asked for explicitly by the agent (mutation_permission), and
the desktop's AI Activity panel shows each call. Both share the same
targets, projects and evidence.

FIRST SESSION (DESKTOP)
-----------------------

Cortex opens on its start page: Analyze a file (nothing runs), Inspect a
running program, or Work with an AI agent (copies the MCP configuration).
For memory:

1. Click "Select a process..." (or File > Select process...).
2. Select the target and click Attach (or double-click the row).
3. Use the "Value scan" panel of the Memory workspace to scan a value
   (First scan, then Next scan).
4. Double-click a useful result to keep it in the address list.
5. Right-click an address to open it in the Memory viewer, the Disassembler
   or the Debugger, or to find out what writes to it.
6. The Workspace menu rearranges the panels for each task: Home, Files and
   AI need no process; Memory, Debug, RE, Trace, Automation and Runtime
   work on one.
7. Switch the header toggle from "Read-only" to "Writes allowed" only when you
   intend an operation that changes the target or persistent state.

COMMAND LINE
------------

       .\cortex.exe --help
       .\cortex.exe --version

Files, without running them (PE, .NET, Electron .asar, JavaScript, ZIP):

       .\cortex.exe inspect C:\path\to\app.asar
       .\cortex.exe inspect C:\path\to\Library.dll --cil Namespace.Type::Method

Running processes:

       .\cortex.exe probe --pid 1234       (read-only health)
       .\cortex.exe diagnose --pid 1234    (crash and hang watcher)
       .\cortex.exe inject <target>        (loads the runtime: a change)

Engines, live debuggers, evidence and investigations:

       .\cortex.exe provider list|run ...
       .\cortex.exe live targets|scripts|watch ...
       .\cortex.exe evidence export|list
       .\cortex.exe investigation list|show|approve|reject ...

MCP SERVER (FOR AN AI CLIENT)
-----------------------------

"cortex.exe setup" writes this for you. By hand, add to your client's MCP
configuration (read-only):

       { "mcpServers": { "cortex": { "command": "C:/Cortex/cortex.exe",
                                     "args": ["mcp"] } } }

Useful arguments, all chosen by you, never by the agent:

       --allow-writes          let the agent change targets (each call must
                               still say mutation_permission: true)
       --allow-state           let it save only its own records
       --tools all             expose every primitive tool
       --pid 1234              attach at startup
       --artifact-root <dir>   folders whose files the agent may inspect
       --launch-config <file>  programs the agent may start (allowlist)

In MCP mode, stdout is reserved for JSON-RPC; diagnostics go to stderr.

OPTIONAL ENGINES
----------------

Cortex does not bundle these. Install them yourself and point Cortex at them:

       Ghidra 11.x + JDK 21      --ghidra-dir <dir>  (or GHIDRA_INSTALL_DIR)
       ILSpy ilspycmd 9.x        --ilspycmd <path>   (or CORTEX_ILSPYCMD)
       Vineflower 1.11 + a JDK   --vineflower <jar>  (or CORTEX_VINEFLOWER)
       netcoredbg 3.1            --netcoredbg <path> (or CORTEX_NETCOREDBG)
       Node --inspect, debugpy, a JVM with JDWP: allow their port with
                                 --inspect-port <port>

Live debuggers only set log breakpoints: they record the stack and locals
and resume; they never evaluate code. See docs/analysis-providers.md.

WHAT IS IN THE ARCHIVE
----------------------

cortex.exe
    The Cortex application (desktop, command line and MCP server).

runtime\x64\cortex_core.dll, runtime\x86\cortex_core.dll
    Runtimes for 64-bit and 32-bit targets.

runtime\x86\cortex_runtime_helper.exe
    Private helper used automatically for 32-bit targets.

README.md, README_INSTALL.txt, CHANGELOG.md, LICENSE, docs
    Documentation, history, licence and guides (docs/limitations.md lists
    what Cortex does not do or has not validated).

TROUBLESHOOTING
---------------

- If cortex.exe does not start, check that the whole archive was extracted
  and that the runtime folder is present.
- If a target cannot be opened, check its architecture and the process's
  Windows rights. Only use elevated privileges when the target requires them.
- If a write, breakpoint or control operation is refused, switch the header
  to "Writes allowed" (desktop) or start the MCP server with --allow-writes.
- Settings and the window layout are saved in %LOCALAPPDATA%\Cortex. To keep
  them next to cortex.exe instead, create an empty file named cortex.portable
  next to the executable.
- Engines show as unavailable with the reason: "cortex.exe provider list".
- Guides: docs/getting-started.md, docs/ui-guide.md, docs/ui-walkthrough.md,
  docs/mcp.md, docs/analysis-providers.md.

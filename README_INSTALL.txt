CORTEX v1.0.0 - INSTALLATION AND QUICK START
============================================

Cortex v1.0.0 ships as a single portable Windows application: cortex.exe.
The x64 and x86 instrumentation runtimes live in the runtime folder and are
selected automatically for each target. Nothing else has to be installed
(no Qt, no Visual C++ runtime).

INSTALLATION
------------

1. Download the archive:

       cortex-v1.0.0-windows-portable.zip

2. Extract the whole archive into a normal, writable folder, for example:

       C:\Cortex\

3. Do not move cortex.exe on its own. Keep the runtime folder next to the
   executable.

4. Run:

       .\cortex.exe

FIRST SESSION
-------------

1. Click "Select process" in the header (or File > Select process...).
2. Select the target and click Attach (or double-click the row).
3. Use the "Value scan" panel of the Memory workspace to scan a value
   (First scan, then Next scan).
4. Double-click a useful result to prepare an Addresses entry.
5. Use an address's context menu to jump to the Memory viewer, the
   Disassembler, RE, Pointer Maps, Structures or debugger actions.
6. The Memory, Debug, RE, Trace, Automation and Runtime buttons in the header
   rearrange the workspaces for each task.
7. Switch the header toggle from "Read-only" to "Writes allowed" only when you
   intend an operation that changes the target or persistent state.

COMMAND-LINE MODES
------------------

Help:

       .\cortex.exe --help

Version:

       .\cortex.exe --version

Persistent MCP server over stdio (targetless; attach from the AI client):

       .\cortex.exe mcp

Attach MCP to a PID or a process at startup:

       .\cortex.exe mcp --pid 1234
       .\cortex.exe mcp --process game.exe

Also expose the low-level primitive tools:

       .\cortex.exe mcp --tools all

Other built-in commands:

       .\cortex.exe probe --pid 1234
       .\cortex.exe diagnose --pid 1234
       .\cortex.exe analyze <directory>
       .\cortex.exe symbolize [options]
       .\cortex.exe inject <target> [dll]

WHAT IS IN THE ARCHIVE
----------------------

cortex.exe
    The Cortex application (desktop UI and CLI/MCP modes).

runtime\x64\cortex_core.dll
    Instrumentation runtime for 64-bit targets.

runtime\x86\cortex_core.dll
    Instrumentation runtime for 32-bit targets.

runtime\x86\cortex_runtime_helper.exe
    Private helper used automatically for 32-bit targets.

    You never choose between x64 and x86: Cortex reads the target's
    architecture and uses the matching runtime, including for
    "cortex.exe inject <pid>".

README.md, CHANGELOG.md, LICENSE, docs
    Documentation, history, license and technical guides.

TROUBLESHOOTING
---------------

- If cortex.exe does not start, check that the whole archive was extracted
  and that the runtime folder is present.
- If a target cannot be opened, check its architecture and the process's
  Windows rights. Only use elevated privileges when the target requires them.
- If a write, breakpoint or control operation is refused, switch the header
  to "Writes allowed" first.
- Settings and the window layout are saved in %LOCALAPPDATA%\Cortex. To keep
  them next to cortex.exe instead (USB stick, sandbox), create an empty file
  named cortex.portable next to the executable.
- In MCP mode, stdout is reserved for JSON-RPC for the whole session;
  diagnostics go to stderr.
- See docs/getting-started.md, docs/ui-guide.md and docs/ui-walkthrough.md
  for the complete workflow.

AUTHORIZATION
-------------

Use Cortex only on software and systems you own or are authorized to inspect.
Anti-cheat bypass, unauthorized access and interference with online services
are outside the scope of this project.

# Third-party software

## Linked or bundled

The libraries built into Cortex (Dear ImGui, nlohmann/json, Zydis/Zycore,
cpp-httplib, Lua, MinHook, ...) are fetched by CMake at the versions pinned in
`CMakeLists.txt` and `app_imgui/CMakeLists.txt`. Each one keeps its own licence
file in its source tree.

## External engines (not linked, not bundled)

These programs are **not distributed with Cortex** and **not linked** into it.
When the person or the operator installs one and gives its location, Cortex
starts it as a separate process on a file and reads its output (see
`docs/analysis-providers.md`). Their licences apply to the copies their users
install.

| Engine | Licence | Used for | How Cortex uses it |
|---|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) | Apache-2.0 | Native analysis and decompilation (PE) | Runs `support/analyzeHeadless` in a temporary project with a post-script that Cortex writes (`CortexExport.java`, part of Cortex) and reads its JSON |
| [ILSpy / ilspycmd](https://github.com/icsharpcode/ILSpy) | MIT | C# decompilation of .NET assemblies | Runs `ilspycmd` with `--disable-updatecheck` and reads the files or the output it writes |
| [Vineflower](https://github.com/Vineflower/vineflower) | Apache-2.0 | Java decompilation (.jar/.class) | Runs `java -jar vineflower.jar -s <input> <out>` and reads the `.java` files it writes |
| [debugpy](https://github.com/microsoft/debugpy) | MIT | Python live (DAP) | Started by the operator (`python -m debugpy --listen`); Cortex connects to it |
| [netcoredbg](https://github.com/Samsung/netcoredbg) | MIT | .NET live (DAP) | Started by the operator in server mode; Cortex connects and attaches it to a process id |
| Java runtime (JDK 21+) | that of the vendor (e.g. GPLv2 with Classpath Exception for OpenJDK builds) | Needed by Ghidra | Started by Ghidra's launcher, not by Cortex |
| .NET runtime | MIT | Needed by ilspycmd | Started by ilspycmd's host, or by `dotnet <ilspycmd.dll>` |

CI downloads Ghidra 11.4.2 (`ghidra_11.4.2_PUBLIC_20250826.zip`) from the
project's GitHub releases, Vineflower 1.11.1 from Maven Central (SHA-1
checked) and installs ilspycmd 9.1.0.7988 from NuGet, to
test against fixed versions. None of them is uploaded with the build artifacts.

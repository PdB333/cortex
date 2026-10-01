#pragma once

// Entry points of the command-line tools bundled into cortex.exe and
// cortex_host.exe. Each tool keeps its own source file and is dispatched by
// name (`cortex mcp`, `cortex inject`, ...) instead of owning main().

int CortexServeMain(int argc, char** argv);      // host/main.cpp
int CortexInjectMain(int argc, char** argv);     // injector/main.cpp
int CortexMcpMain(int argc, char** argv);        // mcp_bridge/main.cpp
int CortexDiagnoseMain(int argc, char** argv);   // tools/diagnostics_host/main.cpp
int CortexSymbolizeMain(int argc, char** argv);  // tools/symbolize_main.cpp
int CortexProbeMain(int argc, char** argv);      // host/probe_cli.cpp

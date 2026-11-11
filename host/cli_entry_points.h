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
int CortexInspectMain(int argc, char** argv);    // tools/inspect_main.cpp
int CortexEvidenceMain(int argc, char** argv);   // tools/evidence_main.cpp
int CortexInvestigationMain(int argc, char** argv);  // tools/investigation_main.cpp
int CortexProviderMain(int argc, char** argv);   // tools/provider_main.cpp
int CortexPs5Main(int argc, char** argv);        // tools/ps5_main.cpp
int CortexLiveMain(int argc, char** argv);       // tools/live_main.cpp
int CortexSetupMain(int argc, char** argv);      // tools/setup_main.cpp
int CortexUpdateMain(int argc, char** argv);     // tools/setup_main.cpp

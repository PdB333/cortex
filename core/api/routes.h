#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace api {

// Master tool manifest -- single source of truth for /tools, /openapi.json,
// and the MCP tools/list dispatch. Defined in routes_status.cpp.
nlohmann::json BuildToolsManifest();

void RegisterStatusRoutes(httplib::Server& svr);
void RegisterModulesRoutes(httplib::Server& svr);
void RegisterMemoryRoutes(httplib::Server& svr);
void RegisterScanRoutes(httplib::Server& svr);
void RegisterDisasmRoutes(httplib::Server& svr);
void RegisterDebugRoutes(httplib::Server& svr);
void RegisterSymbolsRoutes(httplib::Server& svr);
void RegisterProjectRoutes(httplib::Server& svr);
void RegisterScreenshotRoutes(httplib::Server& svr);
void RegisterPromptRoutes(httplib::Server& svr);
void RegisterPatchRoutes(httplib::Server& svr);
void RegisterInputRoutes(httplib::Server& svr);
void RegisterFreezeRoutes(httplib::Server& svr);
void RegisterStructRoutes(httplib::Server& svr);
void RegisterCallRoutes(httplib::Server& svr);
void RegisterWatchRoutes(httplib::Server& svr);
void RegisterAnalysisRoutes(httplib::Server& svr);
void RegisterDissectRoutes(httplib::Server& svr);
void RegisterBatchRoutes(httplib::Server& svr);
void RegisterActionRoutes(httplib::Server& svr);
void RegisterEventRoutes(httplib::Server& svr);
void RegisterPointerMapRoutes(httplib::Server& svr);
void RegisterTraceRoutes(httplib::Server& svr);
void RegisterGhidraRoutes(httplib::Server& svr);
void RegisterTimelineRoutes(httplib::Server& svr);
void RegisterWindowRoutes(httplib::Server& svr);
void RegisterNetRoutes(httplib::Server& svr);
void RegisterSessionRoutes(httplib::Server& svr);
void RegisterMcpRoutes(httplib::Server& svr);
void RegisterLuaRoutes(httplib::Server& svr);
void RegisterOcrRoutes(httplib::Server& svr);

} // namespace api

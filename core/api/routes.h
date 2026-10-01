#pragma once
#include "native_routes.h"
#ifndef CORTEX_ROUTE_REGISTRAR
#include "runtime_control_routes.h"
#endif
#include <nlohmann/json.hpp>

namespace api {

// Master tool manifest -- single source of truth for /tools, /openapi.json,
// and the MCP tools/list dispatch. Defined in routes_status.cpp.
nlohmann::json BuildToolsManifest();
nlohmann::json BuildOpenApiDocument();
bool ValidateApiContracts(nlohmann::json& report);

void RegisterStatusRoutes(RouteRegistrar& svr);
void RegisterModulesRoutes(RouteRegistrar& svr);
void RegisterMemoryRoutes(RouteRegistrar& svr);
void RegisterScanRoutes(RouteRegistrar& svr);
void RegisterDisasmRoutes(RouteRegistrar& svr);
void RegisterDebugRoutesBase(RouteRegistrar& svr);
#ifndef CORTEX_ROUTE_REGISTRAR
inline void RegisterDebugRoutes(RouteRegistrar& svr) {
    RegisterDebugRoutesBase(svr);
    RegisterRuntimeControlRoutes(svr);
}
#endif
void RegisterSymbolsRoutes(RouteRegistrar& svr);
void RegisterProjectRoutes(RouteRegistrar& svr);
void RegisterScreenshotRoutes(RouteRegistrar& svr);
void RegisterPromptRoutes(RouteRegistrar& svr);
void RegisterPatchRoutes(RouteRegistrar& svr);
void RegisterInputRoutes(RouteRegistrar& svr);
void RegisterFreezeRoutes(RouteRegistrar& svr);
void RegisterStructRoutes(RouteRegistrar& svr);
void RegisterCallRoutes(RouteRegistrar& svr);
void RegisterWatchRoutes(RouteRegistrar& svr);
void RegisterAnalysisRoutes(RouteRegistrar& svr);
void RegisterDissectRoutes(RouteRegistrar& svr);
void RegisterBatchRoutes(RouteRegistrar& svr);
void RegisterActionRoutes(RouteRegistrar& svr);
void RegisterEventRoutes(RouteRegistrar& svr);
void RegisterPointerMapRoutes(RouteRegistrar& svr);
void RegisterTraceRoutes(RouteRegistrar& svr);
void RegisterGhidraRoutes(RouteRegistrar& svr);
void RegisterTimelineRoutes(RouteRegistrar& svr);
void RegisterWindowRoutes(RouteRegistrar& svr);
void RegisterNetRoutes(RouteRegistrar& svr);
void RegisterSessionRoutes(RouteRegistrar& svr);
void RegisterMcpRoutes(RouteRegistrar& svr);
void RegisterLuaRoutes(RouteRegistrar& svr);
void RegisterOcrRoutes(RouteRegistrar& svr);
void RegisterReRoutes(RouteRegistrar& svr);

} // namespace api

// Every routes_*.cpp historically spells its registration parameter as
// `httplib::Server&`. For those translation units only, CMake defines
// CORTEX_ROUTE_REGISTRAR; this compatibility alias keeps all route bodies
// untouched while making their `svr.Get/Post/...` calls hit RouteRegistrar.
// server.cpp is compiled without the define and continues to use the real
// httplib::Server for transport concerns.
#ifdef CORTEX_ROUTE_REGISTRAR
namespace httplib { using RouteRegistrar = ::api::RouteRegistrar; }
#define Server RouteRegistrar
#define RegisterDebugRoutes RegisterDebugRoutesBase
#endif

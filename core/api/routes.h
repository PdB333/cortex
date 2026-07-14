#pragma once
#include <httplib.h>

namespace api {

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

} // namespace api

#pragma once
#include <string>

namespace api {

bool Start(int port, const std::string& configuredToken = {});
void Stop();

int GetPort();
unsigned long long GetUptimeMs();
bool IsRunning();
std::string GetLastError();
std::string GetTokenPath();

// Read the current server token. Only intended for in-process loopback
// dispatchers (e.g. the MCP endpoint) -- never expose this over the wire.
std::string GetToken();

} // namespace api

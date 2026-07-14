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

} // namespace api

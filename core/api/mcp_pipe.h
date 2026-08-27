#pragma once

#include <string>

namespace api::mcp_pipe {

bool Start(const std::string& token);
void Stop();
bool IsRunning();
std::string GetLastError();
std::string GetPipeName();

} // namespace api::mcp_pipe

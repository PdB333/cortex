#include "../core/api/mcp_pipe_protocol.h"

#include <iostream>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    const std::string tokenA(64, 'a');
    const std::string tokenB(64, 'b');
    const std::string nameA1 = api::mcp_pipe_protocol::PipeNameForToken(tokenA);
    const std::string nameA2 = api::mcp_pipe_protocol::PipeNameForToken(tokenA);
    const std::string nameB = api::mcp_pipe_protocol::PipeNameForToken(tokenB);

    check(nameA1 == nameA2, "pipe name must be deterministic for one token");
    check(nameA1 != nameB, "different tokens must produce different pipe endpoints");
    check(nameA1.rfind("\\\\.\\pipe\\cortex-mcp-", 0) == 0,
          "pipe endpoint must use the local Cortex namespace");
    check(nameA1.find(tokenA) == std::string::npos,
          "raw authentication token must never appear in the endpoint name");
    check(api::mcp_pipe_protocol::kMaxFrameBytes == 16u * 1024u * 1024u,
          "native MCP frame limit must remain aligned with the API payload limit");

    if (failures) return 1;
    std::cout << "PASS: MCP named-pipe rendezvous and frame contract\n";
    return 0;
}

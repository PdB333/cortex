#include "../mcp_bridge/policy.h"

#include <iostream>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };

    check(mcp_bridge::policy::IsLoopbackHost("127.0.0.1"), "IPv4 loopback accepted");
    check(mcp_bridge::policy::IsLoopbackHost("localhost"), "localhost accepted");
    check(mcp_bridge::policy::IsLoopbackHost("LOCALHOST"), "loopback comparison is case-insensitive");
    check(mcp_bridge::policy::IsLoopbackHost("::1"), "IPv6 loopback accepted");
    check(!mcp_bridge::policy::IsLoopbackHost("192.168.1.10"), "LAN host rejected");
    check(!mcp_bridge::policy::IsLoopbackHost("example.com"), "remote hostname rejected");

    check(mcp_bridge::policy::IsValidPort(1), "lowest valid port accepted");
    check(mcp_bridge::policy::IsValidPort(65535), "highest valid port accepted");
    check(!mcp_bridge::policy::IsValidPort(0), "zero port rejected");
    check(!mcp_bridge::policy::IsValidPort(65536), "oversized port rejected");

    check(mcp_bridge::policy::IsMessageSizeAllowed(mcp_bridge::policy::kMaxMessageBytes),
          "message exactly at limit accepted");
    check(!mcp_bridge::policy::IsMessageSizeAllowed(mcp_bridge::policy::kMaxMessageBytes + 1),
          "message above limit rejected");

    if (failures) return 1;
    std::cout << "PASS: MCP bridge local-only and message-size policy\n";
    return 0;
}

#define main CortexUnifiedMainUnderTest
#include "../host/unified_main.cpp"
#undef main

#include <iostream>

namespace {
int mcpCalls = 0;
}

int CortexServeMain(int, char**) { return 11; }
int CortexInjectMain(int, char**) { return 12; }
int CortexMcpMain(int, char**) { ++mcpCalls; return 77; }
int CortexDiagnoseMain(int, char**) { return 13; }
int CortexSymbolizeMain(int, char**) { return 14; }

int main() {
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };

    {
        char a0[] = "cortex_host";
        char a1[] = "mcp";
        char a2[] = "--host";
        char a3[] = "example.com";
        char* argv[] = {a0, a1, a2, a3, nullptr};
        mcpCalls = 0;
        check(CortexUnifiedMainUnderTest(4, argv) == 2, "remote MCP host is rejected");
        check(mcpCalls == 0, "remote MCP host never reaches bridge entrypoint");
    }

    {
        char a0[] = "cortex_host";
        char a1[] = "mcp";
        char a2[] = "--port";
        char a3[] = "70000";
        char* argv[] = {a0, a1, a2, a3, nullptr};
        mcpCalls = 0;
        check(CortexUnifiedMainUnderTest(4, argv) == 2, "out-of-range MCP port is rejected");
        check(mcpCalls == 0, "invalid MCP port never reaches bridge entrypoint");
    }

    {
        char a0[] = "cortex_host";
        char a1[] = "mcp";
        char a2[] = "--port";
        char a3[] = "abc";
        char* argv[] = {a0, a1, a2, a3, nullptr};
        mcpCalls = 0;
        check(CortexUnifiedMainUnderTest(4, argv) == 2, "malformed MCP port is rejected");
        check(mcpCalls == 0, "malformed MCP port never reaches bridge entrypoint");
    }

    {
        char a0[] = "cortex_host";
        char a1[] = "mcp";
        char a2[] = "--host";
        char a3[] = "localhost";
        char a4[] = "--port";
        char a5[] = "6969";
        char* argv[] = {a0, a1, a2, a3, a4, a5, nullptr};
        mcpCalls = 0;
        check(CortexUnifiedMainUnderTest(6, argv) == 77, "valid loopback MCP target is forwarded");
        check(mcpCalls == 1, "valid MCP target reaches bridge exactly once");
    }

    if (failures) return 1;
    std::cout << "PASS: unified host MCP target enforcement\n";
    return 0;
}

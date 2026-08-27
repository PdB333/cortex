// cortex_mcp_bridge -- stdio transport adapter for Cortex MCP clients.
//
// Reads newline-delimited JSON-RPC messages from stdin, forwards them to the
// local Cortex runtime, and writes JSON-RPC responses to stdout. Notifications
// intentionally produce no stdout data.
//
// Usage in claude_desktop_config.json:
//   { "mcpServers": { "cortex": {
//       "command": "cortex_host.exe",
//       "args": ["mcp", "--token-file", "C:/games/cortex.token"]
//   } } }
// Optional args: --port 6969, --host 127.0.0.1, --token <hex>,
//                --tools compact|all (default compact).

#include <httplib.h>

#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string ReadTokenFile(const std::string& p) {
    std::ifstream f(p);
    std::string t;
    f >> t;
    return t;
}

int Run(const std::string& host,
        int port,
        const std::string& token,
        const std::string& toolProfile) {
    httplib::Client cli(host.c_str(), port);
    cli.set_read_timeout(60, 0);
    httplib::Headers headers = {
        {"X-Cortex-Token", token},
        {"Host", host},
        {"X-Cortex-MCP-Tools", toolProfile}
    };

    // MCP stdio uses one JSON message per line. stdout must contain protocol
    // data only; diagnostics belong on stderr.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto response = cli.Post("/mcp", headers, line, "application/json");
        if (!response) {
            std::cout << R"({"jsonrpc":"2.0","id":null,"error":{"code":-32000,"message":"cortex_unreachable"}})"
                      << "\n";
            continue;
        }

        // HTTP 202 with an empty body represents a JSON-RPC notification. A
        // stdio MCP server must not write a response for notifications.
        if (response->status == 202 || response->body.empty()) continue;

        std::cout << response->body << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", token, toolProfile = "compact";
    int port = 6969;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--token" && i + 1 < argc) token = argv[++i];
        else if (a == "--token-file" && i + 1 < argc) token = ReadTokenFile(argv[++i]);
        else if (a == "--tools" && i + 1 < argc) toolProfile = argv[++i];
        else {
            std::cerr << "cortex_host mcp: unknown or incomplete argument: " << a << "\n";
            return 2;
        }
    }
    if (toolProfile != "compact" && toolProfile != "all") {
        std::cerr << "cortex_host mcp: --tools must be compact or all\n";
        return 2;
    }
    if (token.empty()) {
        // Try cortex.token next to this exe.
        char self[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string dir = self;
        const size_t s = dir.find_last_of("\\/");
        if (s != std::string::npos) dir.resize(s);
        token = ReadTokenFile(dir + "\\cortex.token");
    }
    if (token.empty()) {
        std::cerr << "cortex_host mcp: no token found. Pass --token or --token-file, "
                     "or place cortex.token beside the executable.\n";
        return 1;
    }
    return Run(host, port, token, toolProfile);
}

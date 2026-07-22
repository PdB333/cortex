// cortex_mcp_bridge -- stdio <-> HTTP bridge for MCP clients (Claude Desktop,
// Cursor, Cline, Continue.dev, ...) that only speak the stdio transport.
//
// Reads newline-delimited JSON-RPC messages from stdin, forwards each to
// POST http://127.0.0.1:<port>/mcp with X-Cortex-Token, writes the response
// (single-line JSON) back to stdout. That's the entire protocol.
//
// Usage in claude_desktop_config.json:
//   { "mcpServers": { "cortex": {
//       "command": "cortex_mcp_bridge.exe",
//       "args": ["--token-file", "C:/games/cortex.token"]
//   } } }
// Optional args: --port 6969 (default), --host 127.0.0.1, --token <hex>.

#include <httplib.h>

#include <windows.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string ReadTokenFile(const std::string& p) {
    std::ifstream f(p);
    std::string t;
    f >> t;
    return t;
}

int Run(const std::string& host, int port, const std::string& token) {
    httplib::Client cli(host.c_str(), port);
    cli.set_read_timeout(60, 0);
    httplib::Headers h = {{"X-Cortex-Token", token}, {"Host", host}};

    // MCP stdio: one JSON message per line. stdout must be unbuffered so
    // the client sees responses immediately.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto r = cli.Post("/mcp", h, line, "application/json");
        if (!r) {
            std::cout << R"({"jsonrpc":"2.0","id":null,"error":{"code":-32000,"message":"cortex_unreachable"}})"
                      << "\n";
            std::cout.flush();
            continue;
        }
        // The server already returns a single-line JSON blob (dump() without
        // pretty-print). Forward as-is with a trailing newline.
        std::cout << r->body << "\n";
        std::cout.flush();
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", token;
    int port = 6969;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--token" && i + 1 < argc) token = argv[++i];
        else if (a == "--token-file" && i + 1 < argc) token = ReadTokenFile(argv[++i]);
    }
    if (token.empty()) {
        // Try cortex.token next to this exe.
        char self[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string dir = self;
        size_t s = dir.find_last_of("\\/");
        if (s != std::string::npos) dir.resize(s);
        token = ReadTokenFile(dir + "\\cortex.token");
    }
    if (token.empty()) {
        std::cerr << "cortex_mcp_bridge: no token found. Pass --token or --token-file, "
                     "or place cortex.token beside the exe.\n";
        return 1;
    }
    return Run(host, port, token);
}

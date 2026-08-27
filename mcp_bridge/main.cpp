// cortex_mcp_bridge -- stdio transport adapter for Cortex MCP clients.
//
// Reads newline-delimited JSON-RPC messages from stdin, forwards them to the
// local Cortex runtime, and writes JSON-RPC responses to stdout. Notifications
// intentionally produce no stdout data.
//
// Usage in claude_desktop_config.json:
//   { "mcpServers": { "cortex": {
//       "command": "cortex_host.exe",
//       "args": ["mcp", "--process", "app.exe"]
//   } } }
// Optional args: --port 6969, --host 127.0.0.1, --token <hex>,
//                --token-file <file>, --tools compact|all (default compact),
//                --process <name> | --pid <pid>, --dll <cortex_core.dll>.

#include <httplib.h>

#include <windows.h>
#include <io.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// The unified cortex_host binary links the injector entry point under this
// name. Keeping injection here lets `cortex_host mcp --process ...` remain a
// single command without duplicating injection logic.
int CortexInjectMain(int argc, char** argv);

namespace {

std::string ReadTokenFile(const std::string& p) {
    std::ifstream f(p);
    std::string t;
    f >> t;
    return t;
}

std::string ExecutableDir() {
    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string dir = self;
    const size_t s = dir.find_last_of("\\/");
    if (s != std::string::npos) dir.resize(s);
    return dir;
}

int RunInjectorQuietly(const std::string& target, const std::string& dllPath) {
    std::vector<std::string> storage = {"cortex_host inject", target};
    if (!dllPath.empty()) storage.push_back(dllPath);
    std::vector<char*> args;
    for (auto& item : storage) args.push_back(item.data());
    args.push_back(nullptr);

    // Injector progress normally goes to stdout. During MCP startup stdout is
    // protocol-only, so route injector chatter to stderr for this invocation.
    std::fflush(stdout);
    const int savedStdout = _dup(_fileno(stdout));
    if (savedStdout >= 0) _dup2(_fileno(stderr), _fileno(stdout));
    const int result = CortexInjectMain(static_cast<int>(storage.size()), args.data());
    std::fflush(stdout);
    if (savedStdout >= 0) {
        _dup2(savedStdout, _fileno(stdout));
        _close(savedStdout);
    }
    return result;
}

std::string WaitForToken(const std::string& path) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string token = ReadTokenFile(path);
        if (!token.empty()) return token;
        Sleep(50);
    }
    return {};
}

bool WaitForRuntime(const std::string& host, int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        httplib::Client client(host.c_str(), port);
        client.set_connection_timeout(0, 100000);
        client.set_read_timeout(0, 250000);
        const auto response = client.Get("/health");
        if (response && response->status >= 200 && response->status < 500) return true;
        Sleep(50);
    }
    return false;
}

int Run(const std::string& host,
        int port,
        const std::string& token,
        const std::string& toolProfile) {
    if (!WaitForRuntime(host, port)) {
        std::cerr << "cortex_host mcp: Cortex runtime did not become reachable on "
                  << host << ':' << port << "\n";
        return 3;
    }

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
    std::string host = "127.0.0.1", token, tokenFile, toolProfile = "compact";
    std::string processTarget, dllPath;
    int port = 6969;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--token" && i + 1 < argc) token = argv[++i];
        else if (a == "--token-file" && i + 1 < argc) tokenFile = argv[++i];
        else if (a == "--tools" && i + 1 < argc) toolProfile = argv[++i];
        else if ((a == "--process" || a == "--pid") && i + 1 < argc) processTarget = argv[++i];
        else if (a == "--dll" && i + 1 < argc) dllPath = argv[++i];
        else {
            std::cerr << "cortex_host mcp: unknown or incomplete argument: " << a << "\n";
            return 2;
        }
    }
    if (toolProfile != "compact" && toolProfile != "all") {
        std::cerr << "cortex_host mcp: --tools must be compact or all\n";
        return 2;
    }

    if (!processTarget.empty()) {
        if (dllPath.empty()) dllPath = ExecutableDir() + "\\cortex_core.dll";
        if (RunInjectorQuietly(processTarget, dllPath) != 0) return 4;
    }

    if (token.empty()) {
        if (tokenFile.empty()) tokenFile = ExecutableDir() + "\\cortex.token";
        token = processTarget.empty() ? ReadTokenFile(tokenFile) : WaitForToken(tokenFile);
    }
    if (token.empty()) {
        std::cerr << "cortex_host mcp: no token found. Pass --token or --token-file, "
                     "or place cortex.token beside the executable.\n";
        return 1;
    }
    return Run(host, port, token, toolProfile);
}

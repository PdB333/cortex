// The process tree (step 7): Chromium/Electron roles from command lines, and
// a live tree of this test's own child processes.
//
//   cortex_process_tree_tests          (spawns three children of itself)

#include "../core/target/process_tree.h"

#include <algorithm>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

using cortex::target::ChromiumRole;

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--child") {
#if defined(_WIN32)
        Sleep(20000);  // killed by the parent long before
#endif
        return 0;
    }

    // Roles from command lines, as Electron and Chromium write them.
    CHECK(ChromiumRole("\"C:\\app\\app.exe\" --type=renderer --renderer-client-id=4 --lang=en") == "renderer");
    CHECK(ChromiumRole("app.exe --type=gpu-process --gpu-preferences=x") == "gpu-process");
    CHECK(ChromiumRole("app.exe --type=utility --utility-sub-type=network.mojom.NetworkService") ==
          "utility:network.mojom.NetworkService");
    CHECK(ChromiumRole("app.exe --type=\"crashpad-handler\"") == "crashpad-handler");
    CHECK(ChromiumRole("\"C:\\app\\app.exe\"").empty());
    CHECK(ChromiumRole("app.exe --not-type=renderer").empty());
    CHECK(ChromiumRole("app.exe --x=1 --type=zygote") == "zygote");

    // A synthetic tree: roots, depth, and a recycled parent PID not followed.
    std::vector<cortex::target::ProcessNode> nodes = {
        {10, 1, 100, "app.exe", "", "app.exe", true},
        {11, 10, 101, "app.exe", "", "app.exe --type=renderer", true},
        {12, 11, 102, "app.exe", "", "app.exe --type=utility --utility-sub-type=audio.mojom.AudioService", true},
        {13, 10, 50, "old.exe", "", "old.exe", true},  // created before pid 10: not its child
    };
    auto tree = cortex::target::ProcessTreeJson(nodes, 10);
    CHECK(tree["ok"] == true && tree["electron"] == true && tree["processes"].size() == 3);
    CHECK(tree["processes"][0]["role"] == "main" && tree["processes"][1]["depth"] == 1 && tree["processes"][2]["depth"] == 2 &&
          tree["processes"][2]["role"] == "utility:audio.mojom.AudioService");
    CHECK(cortex::target::ProcessTreeJson(nodes, 99)["error"] == "process_not_found");

#if defined(_WIN32)
    // Live: three children of this process with Chromium switches.
    wchar_t self[32768];
    GetModuleFileNameW(nullptr, self, 32768);
    const wchar_t* switches[] = {L" --child --type=renderer --renderer-client-id=4",
                                 L" --child --type=gpu-process",
                                 L" --child --type=utility --utility-sub-type=network.mojom.NetworkService"};
    std::vector<PROCESS_INFORMATION> children;
    for (const wchar_t* extra : switches) {
        std::wstring command = std::wstring(L"\"") + self + L"\"" + extra;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
            children.push_back(info);
    }
    CHECK(children.size() == 3);
    Sleep(500);
    tree = cortex::target::ProcessTreeJson(cortex::target::SnapshotProcesses(), GetCurrentProcessId());
    std::vector<std::string> roles;
    for (size_t i = 1; i < tree.value("processes", nlohmann::json::array()).size(); ++i)
        roles.push_back(tree["processes"][i]["role"]);
    std::sort(roles.begin(), roles.end());
    CHECK(tree["ok"] == true && tree["electron"] == true && tree["processes"][0]["role"] == "main");
    CHECK(roles == (std::vector<std::string>{"gpu-process", "renderer", "utility:network.mojom.NetworkService"}));
    if (roles != (std::vector<std::string>{"gpu-process", "renderer", "utility:network.mojom.NetworkService"})) std::cerr << tree.dump(1) << std::endl;
    for (auto& info : children) {
        TerminateProcess(info.hProcess, 0);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
#endif
    if (failures) {
        std::cerr << failures << " process tree check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "process tree tests passed" << std::endl;
    return 0;
}

// DAP live provider (roadmap step 8): Python through debugpy and .NET
// through netcoredbg.
//
//   cortex_dap_tests <app.py|-> [<Cortex.LiveFixture.dll|-> <netcoredbg> <Program.cs> [<LiveApp.java>]]
// (the JDWP part, with a JDK, also lives here: CORTEX_REQUIRE_JDWP=1)
//
// Python (python with debugpy: CORTEX_TEST_PYTHON or python3/python on PATH;
// CORTEX_REQUIRE_DEBUGPY=1 makes it mandatory) runs the fixture under
// `python -m debugpy --listen`, and the checks follow the fixture's source:
//   - a log breakpoint on the health write records hits in apply_damage at
//     that line whose locals follow the rule from one hit to the next, then
//     removes itself after max_hits;
//   - the program keeps running (output keeps arriving), including past a
//     breakpoint() Cortex did not set, which is recorded and continued;
//   - Source() reads only files the debugger reported in a stack;
//   - detaching leaves the process running.

#include "live/live_service.h"
#include "providers/process_runner.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace cortex::live;
using json = nlohmann::json;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

bool WaitFor(const std::function<bool()>& condition, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return condition();
}

uint32_t LineOf(const std::string& file, const std::string& marker) {
    std::ifstream in(file);
    std::string text;
    for (uint32_t n = 1; std::getline(in, text); ++n)
        if (text.find(marker) != std::string::npos) return n;
    return 0;
}

int Pid() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

int64_t LastTick(LiveSession& session) {
    const json output = session.Console(0, 5000);
    int64_t tick = -1;
    static const std::regex pattern(R"(tick (\d+))");
    for (const auto& message : output["messages"]) {
        const std::string text = message.value("text", std::string());
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
            tick = std::max<int64_t>(tick, std::stoll((*it)[1]));
    }
    return tick;
}

void TestRefusals() {
    LiveService none({}, false);
    CHECK(none.AttachDap(5678, "debugpy", 0)["error"] == "port_not_allowed");
    LiveService one({1}, true);
    CHECK(one.AttachDap(1, "lldb", 0)["error"] == "unknown_adapter");
    CHECK(one.AttachDap(1, "netcoredbg", 0)["error"] == "process_id_required");
    CHECK(one.AttachDap(1, "debugpy", 0)["error"] == "connect_refused" || one.AttachDap(1, "debugpy", 0)["error"] == "connect_failed");
}

void TestPython(const std::string& fixture) {
    std::string python = std::getenv("CORTEX_TEST_PYTHON") ? std::getenv("CORTEX_TEST_PYTHON") : "";
    if (python.empty()) python = cortex::providers::FindOnPath("python3");
    if (python.empty()) python = cortex::providers::FindOnPath("python");
    const bool require = std::getenv("CORTEX_REQUIRE_DEBUGPY") && std::string(std::getenv("CORTEX_REQUIRE_DEBUGPY")) == "1";
    bool haveDebugpy = false;
    if (!python.empty()) {
        cortex::providers::ProcessSpec probe;
        probe.argv = {python, "-c", "import debugpy; print(debugpy.__version__)"};
        probe.timeout = std::chrono::seconds(60);
        const auto outcome = cortex::providers::RunProcess(probe);
        haveDebugpy = outcome.exitCode == 0;
        if (haveDebugpy) std::cout << "debugpy " << outcome.output;
    }
    if (!haveDebugpy) {
        std::cout << "debugpy: SKIPPED (no python with debugpy)" << std::endl;
        CHECK(!require);
        return;
    }
    const uint16_t port = static_cast<uint16_t>(5800 + Pid() % 500);
    std::atomic<bool> stop{false};
    std::thread runner([&] {
        cortex::providers::ProcessSpec spec;
        spec.argv = {python, "-m", "debugpy", "--listen", "127.0.0.1:" + std::to_string(port), fixture};
        spec.timeout = std::chrono::minutes(5);
        cortex::providers::RunProcess(spec, &stop);
    });
    LiveService service({port}, false);
    json attached;
    CHECK(WaitFor([&] { return (attached = service.AttachDap(port, "debugpy", 0)).value("ok", false); }, 30));
    std::cout << "attach: " << attached.dump() << std::endl;
    auto session = service.Get(attached.value("session_id", std::string()));
    if (session) {
        const uint32_t line = LineOf(fixture, "CORTEX_HEALTH_WRITE");
        CHECK(line > 0);
        CHECK(session->Source(fixture, 1, 10)["error"] == "path_not_reported_by_debugger");
        CHECK(session->Scripts("", 0, 10)["error"] == "not_supported");
        CHECK(WaitFor([&] { return LastTick(*session) > 0; }, 15));
        const json bp = session->SetLogBreakpoint(fixture, line, 0, 6, 4);
        CHECK(bp.value("ok", false) && bp["verified"] == true);
        const int id = bp.value("breakpoint", 0);
        json hits;
        CHECK(WaitFor([&] { return (hits = session->Hits(id, 0, 100))["hits"] == 6; }, 20));
        CHECK(hits["active"] == false);
        std::cout << "first hit: " << (hits["records"].empty() ? json() : hits["records"][0]).dump() << std::endl;
        static const std::regex health(R"('health': (-?\d+))");
        int64_t previous = INT64_MIN, previousDelta = 0;
        bool rule = true;
        for (const auto& record : hits["records"]) {
            const json top = record["frames"][0];
            CHECK(top["function"] == "apply_damage" && top["line"] == line);
            CHECK(fs::equivalent(fs::u8path(top.value("path", std::string("?"))), fs::u8path(fixture)));
            const json locals = top.value("locals", json::object());
            CHECK(locals.value("amount", json::object()).value("value", std::string()) == "9");
            const int absorbed = std::atoi(locals.value("absorbed", json::object()).value("value", std::string("-1")).c_str());
            CHECK(absorbed >= 0 && absorbed <= 3);
            std::smatch match;
            const std::string repr = locals.value("target", json::object()).value("value", std::string());
            CHECK(std::regex_search(repr, match, health));
            const int64_t value = match.empty() ? INT64_MIN : std::stoll(match[1]);
            if (previous != INT64_MIN && value != previous - previousDelta && value != 100) rule = false;
            previous = value;
            previousDelta = 9 - absorbed;
        }
        CHECK(rule);
        // Now the file appeared in a stack: Source reads it.
        const json source = session->Source(fixture, line, 1);
        CHECK(source.value("ok", false) && source["text"].get<std::string>().find("CORTEX_HEALTH_WRITE") != std::string::npos);
        json status;
        CHECK(WaitFor([&] { return (status = session->Status())["foreign_pauses"].get<uint64_t>() >= 1; }, 20));
        const int64_t tick = LastTick(*session);
        CHECK(WaitFor([&] { return LastTick(*session) > tick + 5; }, 15));
        std::cout << "status: " << session->Status().dump() << std::endl;
        CHECK(service.Detach(attached["session_id"])["detached"] == true);
        // Still running: a new session receives new output.
        json again;
        CHECK(WaitFor([&] { return (again = service.AttachDap(port, "debugpy", 0)).value("ok", false); }, 15));
        auto second = service.Get(again.value("session_id", std::string()));
        if (second) CHECK(WaitFor([&] { return LastTick(*second) > 0; }, 15));
        service.Detach(again.value("session_id", std::string()));
    }
    stop = true;
    runner.join();
}

// .NET: the fixture (built in Debug) runs on its own; netcoredbg listens
// in server mode; Cortex attaches it to the fixture's process id.
void TestDotnet(const std::string& dll, const std::string& netcoredbg, const std::string& source) {
    std::string dotnet = std::getenv("CORTEX_TEST_DOTNET") ? std::getenv("CORTEX_TEST_DOTNET") : cortex::providers::FindOnPath("dotnet");
    if (dotnet.empty() || !fs::exists(dll) || !fs::exists(netcoredbg)) {
        std::cout << "netcoredbg: SKIPPED (dotnet, fixture or netcoredbg missing)" << std::endl;
        CHECK(!(std::getenv("CORTEX_REQUIRE_NETCOREDBG") && std::string(std::getenv("CORTEX_REQUIRE_NETCOREDBG")) == "1"));
        return;
    }
    const fs::path pidFile = fs::temp_directory_path() / ("cortex-dotnet-live-" + std::to_string(Pid()) + ".pid");
    fs::remove(pidFile);
    std::atomic<bool> stop{false};
    std::thread app([&] {
        cortex::providers::ProcessSpec spec;
        spec.argv = {dotnet, dll, pidFile.u8string()};
        spec.timeout = std::chrono::minutes(5);
        cortex::providers::RunProcess(spec, &stop);
    });
    const uint16_t port = static_cast<uint16_t>(6400 + Pid() % 500);
    cortex::providers::ProcessOutcome adapterOutcome;
    std::atomic<bool> adapterDone{false};
    std::thread adapter([&] {
        cortex::providers::ProcessSpec spec;
        spec.argv = {netcoredbg, "--interpreter=vscode", "--server=" + std::to_string(port)};
        spec.workingDirectory = fs::u8path(netcoredbg).parent_path().u8string();
        spec.timeout = std::chrono::minutes(5);
        adapterOutcome = cortex::providers::RunProcess(spec, &stop);
        adapterDone = true;
    });
    uint64_t pid = 0;
    CHECK(WaitFor([&] {
        std::ifstream in(pidFile);
        return static_cast<bool>(in >> pid) && pid > 0;
    }, 30));
    LiveService service({port}, false);
    json attached;
    CHECK(WaitFor([&] { return (attached = service.AttachDap(port, "netcoredbg", pid)).value("ok", false); }, 30));
    std::cout << "attach: " << attached.dump() << std::endl;
    if (!attached.value("ok", false)) {
        // Why the adapter is not answering: its own output and exit code.
        stop = true;
        adapter.join();
        std::cerr << "netcoredbg exited=" << adapterDone.load() << " code=" << adapterOutcome.exitCode
                  << " started=" << adapterOutcome.started << " error=" << adapterOutcome.error << "\n"
                  << adapterOutcome.output.substr(0, 4000) << std::endl;
        app.join();
        return;
    }
    auto session = service.Get(attached.value("session_id", std::string()));
    if (session) {
        const uint32_t line = LineOf(source, "CORTEX_HEALTH_WRITE");
        const json bp = session->SetLogBreakpoint(source, line, 0, 5, 4);
        CHECK(bp.value("ok", false));
        const int id = bp.value("breakpoint", 0);
        json hits;
        CHECK(WaitFor([&] { return (hits = session->Hits(id, 0, 100))["hits"] == 5; }, 30));
        CHECK(hits["active"] == false);
        std::cout << "first hit: " << (hits["records"].empty() ? json() : hits["records"][0]).dump() << std::endl;
        for (const auto& record : hits["records"]) {
            const json top = record["frames"][0];
            CHECK(top["function"].get<std::string>().find("ApplyDamage") != std::string::npos && top["line"] == line);
            const json locals = top.value("locals", json::object());
            CHECK(locals.value("amount", json::object()).value("value", std::string()) == "9");
            const int absorbed = std::atoi(locals.value("absorbed", json::object()).value("value", std::string("-1")).c_str());
            CHECK(absorbed >= 0 && absorbed <= 3);
            CHECK(locals.contains("target"));
        }
        const int64_t tick = LastTick(*session);
        std::cout << "status: " << session->Status().dump() << std::endl;
        CHECK(service.Detach(attached["session_id"])["detached"] == true);
        (void)tick;
    }
    // The fixture survives detaching: its pid file is still a live process.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
#if defined(_WIN32)
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    CHECK(handle && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT);
    if (handle) CloseHandle(handle);
#else
    CHECK(pid && fs::exists("/proc/" + std::to_string(pid)));
#endif
    stop = true;
    app.join();
    adapter.join();
    fs::remove(pidFile);
}

std::string JavaTool(const char* name) {
    if (const char* home = std::getenv("JAVA_HOME")) {
#if defined(_WIN32)
        const fs::path candidate = fs::u8path(home) / "bin" / (std::string(name) + ".exe");
#else
        const fs::path candidate = fs::u8path(home) / "bin" / name;
#endif
        if (fs::exists(candidate)) return candidate.u8string();
    }
    return cortex::providers::FindOnPath(name);
}

// JDWP: the fixture compiled with -g runs with the JDWP agent on loopback.
void TestJava(const std::string& source) {
    const std::string javac = JavaTool("javac"), java = JavaTool("java");
    if (javac.empty() || java.empty()) {
        std::cout << "jdwp: SKIPPED (no JDK)" << std::endl;
        CHECK(!(std::getenv("CORTEX_REQUIRE_JDWP") && std::string(std::getenv("CORTEX_REQUIRE_JDWP")) == "1"));
        return;
    }
    const fs::path classes = fs::temp_directory_path() / ("cortex-jdwp-" + std::to_string(Pid()));
    fs::create_directories(classes);
    cortex::providers::ProcessSpec compile;
    compile.argv = {javac, "-g", "-d", classes.u8string(), source};
    compile.timeout = std::chrono::minutes(2);
    const auto compiled = cortex::providers::RunProcess(compile);
    CHECK(compiled.exitCode == 0);
    if (compiled.exitCode != 0) {
        std::cerr << compiled.output << std::endl;
        return;
    }
    const uint16_t port = static_cast<uint16_t>(7000 + Pid() % 500);
    std::atomic<bool> stop{false};
    std::thread app([&] {
        cortex::providers::ProcessSpec spec;
        spec.argv = {java, "-agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=127.0.0.1:" + std::to_string(port), "-cp",
                     classes.u8string(), "LiveApp"};
        spec.timeout = std::chrono::minutes(5);
        cortex::providers::RunProcess(spec, &stop);
    });
    LiveService service({port}, false);
    json attached;
    CHECK(WaitFor([&] { return (attached = service.AttachJdwp(port)).value("ok", false); }, 30));
    std::cout << "attach: " << attached.dump() << std::endl;
    auto session = service.Get(attached.value("session_id", std::string()));
    if (session) {
        const json classesList = session->Scripts("LiveApp", 0, 10);
        CHECK(classesList["total"].get<size_t>() >= 1);
        CHECK(session->SetLogBreakpoint("NoSuchClass", 1, 0, 1, 1)["error"] == "class_not_loaded");
        CHECK(session->SetLogBreakpoint("LiveApp", 1, 0, 1, 1)["error"] == "line_not_in_class");
        const uint32_t line = LineOf(source, "CORTEX_HEALTH_WRITE");
        const json bp = session->SetLogBreakpoint("LiveApp", line, 0, 5, 4);
        CHECK(bp.value("ok", false));
        const int id = bp.value("breakpoint", 0);
        json hits;
        CHECK(WaitFor([&] { return (hits = session->Hits(id, 0, 100))["hits"] == 5; }, 30));
        CHECK(hits["active"] == false);
        std::cout << "first hit: " << (hits["records"].empty() ? json() : hits["records"][0]).dump() << std::endl;
        for (const auto& record : hits["records"]) {
            CHECK(!record["frames"].empty());
            if (record["frames"].empty()) continue;
            const json top = record["frames"][0];
            CHECK(top["function"] == "LiveApp.applyDamage" && top["line"] == line);
            CHECK(record["frames"].size() >= 2 && record["frames"][1]["function"] == "LiveApp.main");
            const json locals = top.value("locals", json::object());
            CHECK(locals.value("amount", json::object()).value("value", std::string()) == "9");
            const int absorbed = std::atoi(locals.value("absorbed", json::object()).value("value", std::string("-1")).c_str());
            CHECK(absorbed >= 0 && absorbed <= 3);
            CHECK(locals.value("target", json::object()).value("type", std::string()) == "LiveApp$Actor");
        }
        // The thread was resumed after each hit: a breakpoint in main's loop
        // keeps being hit, with the String local read without calling Java.
        const json loop = session->SetLogBreakpoint("LiveApp", LineOf(source, "System.out.println"), 0, 3, 2);
        CHECK(loop.value("ok", false));
        json loopHits;
        CHECK(WaitFor([&] { return (loopHits = session->Hits(loop.value("breakpoint", 0), 0, 10))["hits"] == 3; }, 30));
        if (!loopHits["records"].empty() && !loopHits["records"][0]["frames"].empty())
            CHECK(loopHits["records"][0]["frames"][0]["locals"].value("label", json::object()).value("value", std::string()) == "tick");
        CHECK(service.Detach(attached["session_id"])["detached"] == true);
        // After Dispose the VM runs on: a new connection attaches again.
        json again;
        CHECK(WaitFor([&] { return (again = service.AttachJdwp(port)).value("ok", false); }, 15));
        service.Detach(again.value("session_id", std::string()));
    }
    stop = true;
    app.join();
    std::error_code ec;
    fs::remove_all(classes, ec);
}

} // namespace

int main(int argc, char** argv) {
    TestRefusals();
    if (argc > 1 && std::string(argv[1]) != "-") TestPython(fs::absolute(argv[1]).u8string());
    if (argc > 4 && std::string(argv[2]) != "-") TestDotnet(fs::absolute(argv[2]).u8string(), fs::absolute(argv[3]).u8string(), fs::absolute(argv[4]).u8string());
    if (argc > 5) TestJava(fs::absolute(argv[5]).u8string());
    if (failures) {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "dap tests passed" << std::endl;
    return 0;
}

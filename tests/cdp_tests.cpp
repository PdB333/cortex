// CDP live provider (roadmap step 8): Node.js through its inspector.
//
//   cortex_cdp_tests [tests/fixtures/node-live/app.js]
//
// Always: SHA-1/Base64 (the WebSocket handshake), and refusals (port not
// allowed, nothing listening). With node (CORTEX_TEST_NODE, or node on PATH;
// CORTEX_REQUIRE_NODE=1 makes it mandatory) the fixture is started with
// --inspect=127.0.0.1:<port> and checked against its source:
//   - the script and its exact source are listed;
//   - a log breakpoint on the health write records hits whose locals follow
//     the fixture's rule from one hit to the next (health drops by
//     amount - absorbed, or resets to 100), and removes itself after
//     max_hits;
//   - the program keeps running (console ticks continue), including past a
//     `debugger;` statement Cortex did not set, which is recorded and resumed;
//   - detaching leaves the process running.
// CORTEX_TEST_INSPECT_PORT uses an already started fixture instead.

#include "live/cdp.h"
#include "providers/process_runner.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
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

std::string Hex(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) out += {digits[c >> 4], digits[c & 15]};
    return out;
}

void TestPrimitives() {
    CHECK(Hex(Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Hex(Sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    // RFC 6455 section 1.3.
    CHECK(Base64(Sha1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(Base64("f") == "Zg==" && Base64("fo") == "Zm8=" && Base64("foo") == "Zm9v");

    LiveService none({}, false);
    CHECK(none.Attach(9229, "")["error"] == "port_not_allowed");
    CHECK(none.Targets(9229)["error"] == "port_not_allowed");
    LiveService one({1}, false);
    CHECK(one.Targets(1)["error"] == "inspector_unreachable");  // nothing listens on port 1
}

bool WaitFor(const std::function<bool()>& condition, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return condition();
}

int64_t Number(const json& described) {
    if (described.contains("value") && described["value"].is_number()) return described["value"].get<int64_t>();
    return INT64_MIN;
}

int64_t PreviewNumber(const json& described, const std::string& name) {
    for (const auto& property : described.value("preview", json::array()))
        if (property.value("name", std::string()) == name) return std::stoll(property.value("value", std::string("0")));
    return INT64_MIN;
}

int64_t LastTick(CdpSession& session) {
    const json console = session.Console(0, 5000);
    int64_t tick = -1;
    for (const auto& message : console["messages"]) {
        const std::string text = message.value("text", std::string());
        if (text.rfind("tick ", 0) == 0) tick = std::max<int64_t>(tick, std::stoll(text.substr(5)));
    }
    return tick;
}

void TestNode(const std::string& fixture) {
    const char* external = std::getenv("CORTEX_TEST_INSPECT_PORT");
    std::string node = std::getenv("CORTEX_TEST_NODE") ? std::getenv("CORTEX_TEST_NODE") : cortex::providers::FindOnPath("node");
    const bool require = std::getenv("CORTEX_REQUIRE_NODE") && std::string(std::getenv("CORTEX_REQUIRE_NODE")) == "1";
    if (!external && node.empty()) {
        std::cout << "node: SKIPPED (no node on PATH, CORTEX_TEST_NODE unset)" << std::endl;
        CHECK(!require);
        return;
    }
#if defined(_WIN32)
    const uint16_t port = external ? static_cast<uint16_t>(std::atoi(external)) : static_cast<uint16_t>(9300 + GetCurrentProcessId() % 500);
#else
    const uint16_t port = external ? static_cast<uint16_t>(std::atoi(external)) : static_cast<uint16_t>(9300 + getpid() % 500);
#endif
    std::atomic<bool> stop{false};
    std::thread runner;
    if (!external) {
        runner = std::thread([&] {
            cortex::providers::ProcessSpec spec;
            spec.argv = {node, "--inspect=127.0.0.1:" + std::to_string(port), fixture};
            spec.timeout = std::chrono::minutes(5);
            cortex::providers::RunProcess(spec, &stop);
        });
    }
    LiveService service({port}, false);
    json targets;
    CHECK(WaitFor([&] { return (targets = service.Targets(port)).value("ok", false) && !targets["targets"].empty(); }, 20));
    std::cout << "inspector: " << targets.dump() << std::endl;
    if (targets.value("ok", false) && !targets["targets"].empty()) {
        CHECK(targets["targets"][0]["type"] == "node" && targets["targets"][0]["attachable"] == true);

        const json attached = service.Attach(port, "");
        CHECK(attached.value("ok", false) && attached["connected"] == true);
        const std::string id = attached.value("session_id", std::string());
        auto session = service.Get(id);
        CHECK(session != nullptr);
        if (session) {
            json scripts;
            CHECK(WaitFor([&] { return (scripts = session->Scripts("app.js", 0, 10))["total"] == 1; }, 10));
            const std::string url = scripts["scripts"].empty() ? "" : scripts["scripts"][0].value("url", std::string());
            CHECK(url.rfind("file://", 0) == 0);

            // Exact source, and the line of the health write found in it.
            std::ifstream file(fixture, std::ios::binary);
            const std::string expected((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const json source = session->Source(url, 1, 100000);
            CHECK(source.value("ok", false) && source["text"].get<std::string>().find(expected.substr(0, expected.find('\n'))) != std::string::npos);
            CHECK(source.value("bytes", size_t{0}) >= expected.size() - 64);  // Node may wrap CommonJS sources
            uint32_t line = 0;
            {
                std::istringstream lines(expected);
                std::string text;
                for (uint32_t n = 1; std::getline(lines, text); ++n)
                    if (text.find("CORTEX_HEALTH_WRITE") != std::string::npos) line = n;
            }
            CHECK(line > 0);

            const int64_t tickBefore = (WaitFor([&] { return LastTick(*session) > 0; }, 10), LastTick(*session));
            const json bp = session->SetLogBreakpoint(url, line, 0, 6, 4);
            CHECK(bp.value("ok", false) && !bp["resolved"].empty());
            const int bpId = bp.value("breakpoint", 0);
            json hits;
            CHECK(WaitFor([&] { return (hits = session->Hits(bpId, 0, 100))["hits"] == 6; }, 15));
            CHECK(hits["active"] == false);  // removed itself after max_hits
            std::cout << "first hit: " << (hits["records"].empty() ? json() : hits["records"][0]).dump() << std::endl;
            int64_t previousHealth = INT64_MIN, previousDelta = 0;
            bool rule = true;
            for (const auto& record : hits["records"]) {
                const json top = record["frames"][0];
                CHECK(top["function"] == "applyDamage" && top["line"] == line && top["url"] == url);
                CHECK(record["frames"].size() >= 2);  // the interval callback called it
                const json locals = top.value("locals", json::object());
                CHECK(Number(locals.value("amount", json::object())) == 9);
                const int64_t absorbed = Number(locals.value("absorbed", json::object()));
                CHECK(absorbed >= 0 && absorbed <= 3);
                const int64_t health = PreviewNumber(locals.value("target", json::object()), "health");
                CHECK(health != INT64_MIN);
                // Before the write: the previous hit's health minus its delta,
                // or a reset to 100.
                if (previousHealth != INT64_MIN && health != previousHealth - previousDelta && health != 100) rule = false;
                previousHealth = health;
                previousDelta = 9 - absorbed;
            }
            CHECK(rule);

            // The program keeps running: more ticks, a `debugger;` resumed.
            json status;
            CHECK(WaitFor([&] { return (status = session->Status())["foreign_pauses"].get<uint64_t>() >= 1; }, 15));
            const int64_t tickAfter = LastTick(*session);
            CHECK(WaitFor([&] { return LastTick(*session) > tickAfter + 5; }, 10));
            std::cout << "ticks " << tickBefore << " -> " << LastTick(*session) << ", status " << session->Status().dump() << std::endl;
            CHECK(status["foreign_pause_records"][0]["reason"] == "other");

            CHECK(session->RemoveBreakpoint(bpId)["already_inactive"] == true);
            CHECK(session->Source("no-such-script", 1, 1)["error"] == "script_not_found");
            CHECK(service.Detach(id)["detached"] == true);
            CHECK(service.Get(id) == nullptr);
            // Still running after detaching: a new session sees new ticks.
            const json again = service.Attach(port, "");
            CHECK(again.value("ok", false));
            auto second = service.Get(again.value("session_id", std::string()));
            if (second) CHECK(WaitFor([&] { return LastTick(*second) > 0; }, 10));
            service.Detach(again.value("session_id", std::string()));
        }
    }
    stop = true;
    if (runner.joinable()) runner.join();
}

} // namespace

int main(int argc, char** argv) {
    TestPrimitives();
    if (argc > 1) TestNode(fs::absolute(argv[1]).u8string());
    if (failures) {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "cdp tests passed" << std::endl;
    return 0;
}

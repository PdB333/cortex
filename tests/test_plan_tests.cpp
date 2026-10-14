#include "../mcp_bridge/test_plan.h"
#include <iostream>
#include <string>

using json = nlohmann::json;
static int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
int main() {
    cortex::test::Plan plan;
    std::string error;
    const json baseline = {
        {"label", "test jump"},
        {"mode", "game"},
        {"steps", json::array({{{"vk", 32}, {"tap_ms", 80}}, {{"delay_ms", 200}}})},
        {"reads", json::array({{{"address", "game.exe+0x123"}, {"type", "float"}}})},
        {"mutation_permission", true}
    };
    Check(cortex::test::ParsePlan(baseline,plan,error), "valid bounded test");
    Check(plan.mode == "game" && plan.steps.size() == 2, "plan parsed");
    json changed = baseline;
    changed["mutation_permission"] = false;
    Check(!cortex::test::ParsePlan(changed,plan,error), "explicit permission");
    changed = baseline;
    changed["steps"] = json::array({{{"vk", 32}, {"down", true}}});
    Check(!cortex::test::ParsePlan(changed,plan,error), "no unbalanced keys");
    changed = baseline;
    changed["steps"] = json::array({{{"delay_ms", 2001}}});
    Check(!cortex::test::ParsePlan(changed,plan,error), "bounded delay");
    changed = baseline;
    changed["reads"] = json::array({{{"address", "0x1000"}, {"type", "bytes"}, {"count", 400}}});
    Check(!cortex::test::ParsePlan(changed,plan,error), "bounded memory reads");
    changed = baseline;
    changed["steps"] = json::array({{{"vk", 32}, {"tap_ms", 80}, {"command", "whoami"}}});
    Check(!cortex::test::ParsePlan(changed,plan,error), "unexpected commands rejected");
    changed = baseline;
    changed["mode"] = "game";
    changed["steps"] = json::array({{{"mouse_move", {{"dx", 30}, {"dy", 0}}}}});
    Check(!cortex::test::ParsePlan(changed,plan,error), "unsupported background movement");
    changed = baseline;
    changed["timeout_ms"] = 1000;
    changed["steps"] = json::array({{{"delay_ms", 900}}});
    Check(!cortex::test::ParsePlan(changed,plan,error), "test timeout includes margin");
    changed = baseline;
    changed["reads"] = json::array();
    Check(!cortex::test::ParsePlan(changed,plan,error), "no test without observation");
    if (failures) return 1;
    std::cout << "PASS: constrained input experiments and bounded memory observations\n";
    return 0;
}

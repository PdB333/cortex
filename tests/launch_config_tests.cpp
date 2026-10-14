#include "../mcp_bridge/launch_config.h"

#include <iostream>
#include <string>

using json = nlohmann::json;
static int failures = 0;
static void Check(bool valid, const char* what) {
    if (!valid) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
int main() {
    using cortex::launch::ParseProfiles;
    using cortex::launch::Profile;
    std::vector<Profile> profiles;
    std::string error;
    const json allowed = {{"profiles", json::array({{
        {"name", "test_game"}, {"executable", "C:/Games/Test/game.exe"},
        {"working_directory", "C:/Games/Test"},
        {"arguments", json::array({"--windowed", "hello world"})},
        {"allow_stop", true}, {"max_runs", 3}
    }})}};
    Check(ParseProfiles(allowed, profiles, error), "valid explicit launch profile");
    Check(profiles.size() == 1 && profiles[0].maxRuns == 3 &&
          profiles[0].arguments.size() == 2, "profile parsed");
    const json noConfig = json::object();
    Check(!ParseProfiles(noConfig, profiles, error), "not enabled without explicit config");
    json changed = allowed;
    changed["profiles"][0]["executable"] = "game.exe";
    Check(!ParseProfiles(changed, profiles, error), "relative executable forbidden");
    changed = allowed;
    changed["profiles"][0]["executable"] = "C:/Windows/System32/cmd.exe";
    changed["profiles"][0]["extra"] = "arbitrary data";
    Check(!ParseProfiles(changed, profiles, error), "unknown config fields forbidden");
    changed = allowed;
    changed["profiles"][0]["max_runs"] = 100;
    Check(!ParseProfiles(changed, profiles, error), "run limit bounded");
    changed = allowed;
    changed["profiles"][0]["arguments"] = json::array({"ok", json::object()});
    Check(!ParseProfiles(changed, profiles, error), "arguments must be strings");
    changed = allowed;
    changed["profiles"].push_back(allowed["profiles"][0]);
    Check(!ParseProfiles(changed, profiles, error), "duplicate profiles rejected");
    changed = allowed;
    changed["profiles"][0]["executable"] = "//server/share/game.exe";
    Check(!ParseProfiles(changed, profiles, error), "network launch path rejected");
    changed = allowed;
    changed["profiles"][0]["name"] = "../escape";
    Check(!ParseProfiles(changed, profiles, error), "unsafe profile name rejected");
    if (failures) return 1;
    std::cout << "PASS: allowlisted launch profile validation\n";
    return 0;
}

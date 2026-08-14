#include "../core/api/mcp_contract.h"

#include <iostream>
#include <string>

using json = nlohmann::json;

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };

    const auto rendered = api::mcp_contract::RenderPath(
        "/objects/{name}",
        {{"_path", {{"name", "player/main #1"}}},
         {"_query", {{"filter", "hp&armor=1"}, {"limit", 25}}}});
    check(static_cast<bool>(rendered), "valid path renders");
    check(rendered.path == "/objects/player%2Fmain%20%231?filter=hp%26armor%3D1&limit=25",
          "URI components are percent encoded");

    const auto missing = api::mcp_contract::RenderPath("/objects/{id}", json::object());
    check(!missing && missing.error == "missing_path_parameters", "missing path values are rejected");

    const auto tooLong = api::mcp_contract::RenderPath(
        "/objects", {{"_query", {{"q", std::string(100, 'a')}}}}, 32);
    check(!tooLong && tooLong.error == "rendered_path_too_long", "rendered URI is bounded");

    const auto booleanSchema = api::mcp_contract::SchemaForProperty(
        "pause_process", "optional bool, default false");
    check(booleanSchema.value("type", std::string()) == "boolean", "boolean property is typed");

    const auto integerSchema = api::mcp_contract::SchemaForProperty(
        "limit", "optional, default 100, max 1000");
    check(integerSchema.value("type", std::string()) == "integer", "integer property is typed");

    const auto addressSchema = api::mcp_contract::SchemaForProperty("address", "required address");
    check(addressSchema.contains("oneOf") && addressSchema["oneOf"].size() == 2,
          "address accepts integer or string");

    const auto arraySchema = api::mcp_contract::SchemaForProperty("items", "array of values");
    check(arraySchema.value("type", std::string()) == "array", "array property is typed");

    check(api::mcp_contract::IsRequiredSpec("required: value"), "required legacy property detected");
    check(!api::mcp_contract::IsRequiredSpec("optional: value"), "optional legacy property detected");

    if (failures) return 1;
    std::cout << "PASS: MCP URI and schema contract\n";
    return 0;
}

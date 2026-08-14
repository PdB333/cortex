#include "../core/api/mcp_contract.h"

#include <iostream>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    const auto requiredQuery = api::mcp_contract::BuildQuerySchema(
        {{"filter", "required: text value"}, {"limit", "optional, default 100"}});
    check(requiredQuery.containerRequired, "required field makes query container required");
    check(requiredQuery.schema.contains("required"), "required list is emitted");
    check(requiredQuery.schema["required"].size() == 1,
          "only required fields enter required list");
    check(requiredQuery.schema["required"][0] == "filter",
          "required field name is preserved");
    check(requiredQuery.schema["properties"]["filter"].value("type", std::string()) == "string",
          "text query property is typed as string");
    check(requiredQuery.schema["properties"]["limit"].value("type", std::string()) == "integer",
          "numeric query property is typed as integer");

    const auto optionalQuery = api::mcp_contract::BuildQuerySchema(
        {{"offset", "optional, default 0"}, {"limit", "optional, default 100"}});
    check(!optionalQuery.containerRequired, "all-optional query container remains optional");
    check(!optionalQuery.schema.contains("required"), "optional query has no required list");

    if (failures) return 1;
    std::cout << "PASS: MCP query schema contract\n";
    return 0;
}

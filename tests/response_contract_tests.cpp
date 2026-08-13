#include "../core/api/response_contract.h"

#include <iostream>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };

    check(api::response::IsStableErrorCode("invalid_request"), "stable snake-case code accepted");
    check(!api::response::IsStableErrorCode("InvalidRequest"), "mixed-case code rejected");
    check(!api::response::IsStableErrorCode("invalid-request"), "hyphenated code rejected");

    const auto success = api::response::Success({{"value", 7}}, "cx-1-2-3");
    check(success.at("ok").get<bool>(), "success sets ok=true");
    check(success.at("data").at("value").get<int>() == 7, "success preserves data");
    check(success.at("request_id").get<std::string>() == "cx-1-2-3", "success carries request id");

    const auto error = api::response::Error("invalid_request", "bad input", "cx-1-2-4");
    check(!error.at("ok").get<bool>(), "error sets ok=false");
    check(error.at("error").get<std::string>() == "invalid_request", "error code stays machine-stable");
    check(error.at("message").get<std::string>() == "bad input", "human message stays separate");
    check(error.at("request_id").get<std::string>() == "cx-1-2-4", "error carries request id");

    if (failures) return 1;
    std::cout << "PASS: stable API response contract\n";
    return 0;
}

#include "../core/api/request_id.h"

#include <iostream>
#include <set>
#include <string>

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    check(api::request_id::Format(0x10, 0x20, 0x30) == "cx-10-20-30",
          "request id formatting must be deterministic");

    std::set<std::string> ids;
    for (uint64_t sequence = 1; sequence <= 1024; ++sequence)
        ids.insert(api::request_id::Format(1234, 42, sequence));
    check(ids.size() == 1024, "sequence must make request ids unique");

    const std::string id = api::request_id::Format(1, 2, 3);
    check(id.rfind("cx-", 0) == 0, "request id prefix must stay stable");
    check(id.find(' ') == std::string::npos, "request id must be header-safe");
    check(id.find('\r') == std::string::npos && id.find('\n') == std::string::npos,
          "request id must not contain line breaks");

    if (failures != 0) return 1;
    std::cout << "PASS: request id format and uniqueness\n";
    return 0;
}

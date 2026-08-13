#include "../core/api/pagination.h"
#include "../core/api/request_limits.h"

#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}

int main() {
    using api::pagination::Error;

    auto page = api::pagination::Normalize(25, 50);
    Check(static_cast<bool>(page), "valid page is accepted");
    Check(page.page.offset == 25 && page.page.limit == 50 && page.page.end == 75,
          "valid page preserves offset/limit and computes end");

    page = api::pagination::Normalize(10, 0, 100, 500);
    Check(static_cast<bool>(page) && page.page.limit == 100,
          "zero requested limit uses configured default");

    page = api::pagination::Normalize(0, 501, 100, 500);
    Check(!page && page.error == Error::LimitTooLarge,
          "limit above hard maximum is rejected");

    page = api::pagination::Normalize((std::numeric_limits<size_t>::max)() - 5, 10);
    Check(!page && page.error == Error::RangeOverflow,
          "offset plus limit overflow is rejected");

    page = api::pagination::Normalize(0, 1, 0, 100);
    Check(!page && page.error == Error::LimitTooLarge,
          "invalid zero default configuration is rejected");

    page = api::pagination::Normalize(0, 1, 200, 100);
    Check(!page && page.error == Error::LimitTooLarge,
          "default larger than maximum is rejected");

    using api::request_limits::ContentLengthError;
    using api::request_limits::ParseContentLength;
    using api::request_limits::kMaxPayloadBytes;

    auto length = ParseContentLength("1024");
    Check(length && length.length == 1024, "normal API payload length is accepted");

    length = ParseContentLength(std::to_string(kMaxPayloadBytes));
    Check(length && length.length == kMaxPayloadBytes, "payload at hard limit is accepted");

    length = ParseContentLength(std::to_string(kMaxPayloadBytes + 1));
    Check(!length && length.error == ContentLengthError::TooLarge,
          "payload above hard limit is rejected");

    length = ParseContentLength("not-a-size");
    Check(!length && length.error == ContentLengthError::Invalid,
          "invalid content length is rejected");

    if (failures) {
        std::cerr << failures << " utility test(s) failed\n";
        return 1;
    }
    std::cout << "PASS: pagination and API request limit policy\n";
    return 0;
}

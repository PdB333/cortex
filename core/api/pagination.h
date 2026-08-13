#pragma once

#include <cstddef>
#include <limits>

namespace api::pagination {

struct Page {
    size_t offset = 0;
    size_t limit = 0;
    size_t end = 0;
};

enum class Error {
    None,
    LimitTooLarge,
    RangeOverflow
};

struct Result {
    Page page;
    Error error = Error::None;
    explicit operator bool() const { return error == Error::None; }
};

inline Result Normalize(size_t offset,
                        size_t requestedLimit,
                        size_t defaultLimit = 100,
                        size_t maxLimit = 1000) {
    if (defaultLimit == 0 || maxLimit == 0 || defaultLimit > maxLimit)
        return {{}, Error::LimitTooLarge};

    const size_t limit = requestedLimit == 0 ? defaultLimit : requestedLimit;
    if (limit > maxLimit) return {{}, Error::LimitTooLarge};
    if (offset > (std::numeric_limits<size_t>::max)() - limit)
        return {{}, Error::RangeOverflow};

    return {{offset, limit, offset + limit}, Error::None};
}

} // namespace api::pagination

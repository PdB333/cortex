// The runtime event ring (core/events): contiguous ids, bounded size and
// explicit reporting of evicted events, so a consumer knows when it must
// resynchronize from state instead of trusting its event stream.

#include "events/events.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

} // namespace

int main() {
    // Empty ring: nothing missed, nothing held.
    auto page = events::Read(0, 16);
    CHECK(page.events.empty() && page.oldestId == 0 && page.latestId == 0 && !page.gap && page.missed == 0);

    const uint64_t first = events::Publish(events::types::RuntimeStarted, "{\"pid\":1}");
    CHECK(first == 1);
    for (int i = 0; i < 9; ++i) events::Publish(events::types::AddressSelected, "{}");
    page = events::Read(0, 16);
    CHECK(page.events.size() == 10 && page.oldestId == 1 && page.latestId == 10);
    CHECK(!page.gap && page.missed == 0);
    CHECK(page.events.front().type == "runtime.started");

    // A caught-up consumer sees no gap and no events.
    page = events::Read(10, 16);
    CHECK(page.events.empty() && !page.gap);

    // Overflow the ring: the oldest events are evicted.
    const size_t total = events::kMaxEvents + 500;
    for (size_t i = 10; i < total; ++i) events::Publish(events::types::ObservationAdded, "{}");
    CHECK(events::LatestId() == total);

    page = events::Read(0, 8);
    CHECK(page.oldestId == total - events::kMaxEvents + 1);
    CHECK(page.gap && page.missed == total - events::kMaxEvents);
    CHECK(page.events.size() == 8 && page.events.front().id == page.oldestId);

    // A consumer that stopped at id 100 lost 101 .. oldest-1.
    page = events::Read(100, 4);
    CHECK(page.gap && page.missed == page.oldestId - 101);

    // One that stopped exactly before the oldest kept everything.
    page = events::Read(page.oldestId - 1, 4);
    CHECK(!page.gap && page.missed == 0 && page.events.front().id == page.oldestId);

    // Ids stay contiguous inside the ring.
    page = events::Read(page.oldestId - 1, 256);
    for (size_t i = 1; i < page.events.size(); ++i) CHECK(page.events[i].id == page.events[i - 1].id + 1);

    // Since() is unchanged for existing callers.
    CHECK(events::Since(total - 3, 16).size() == 3);

    if (failures) {
        std::cerr << failures << " event ring check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "event ring tests passed" << std::endl;
    return 0;
}

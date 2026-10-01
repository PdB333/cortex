#include "pointer_scanner.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace cortex::services {
namespace {

using json = nlohmann::json;

constexpr uint64_t kChunk = 4ull * 1024 * 1024;

struct Slot {
    uint64_t value = 0;    // where the pointer points
    uint64_t address = 0;  // where the pointer is stored
};

struct Range {
    uint64_t begin = 0;
    uint64_t end = 0;
};

bool IsCancelled(const std::atomic_bool* cancelled) {
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

std::vector<Range> Merge(std::vector<Range> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
    std::vector<Range> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) merged.back().end = std::max(merged.back().end, range.end);
        else merged.push_back(range);
    }
    return merged;
}

bool Inside(const std::vector<Range>& ranges, uint64_t value) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), value,
                               [](uint64_t v, const Range& range) { return v < range.begin; });
    if (it == ranges.begin()) return false;
    --it;
    return value < it->end;
}

class Search {
public:
    Search(const std::vector<Slot>& slots, const PointerScanOptions& options, PointerScanResult& result,
           const std::atomic_bool* cancelled)
        : slots_(slots), options_(options), result_(result), cancelled_(cancelled) {
        for (uint32_t i = 0; i < result.modules.size(); ++i) {
            modules_.push_back({result.modules[i].base, result.modules[i].base + result.modules[i].size});
            moduleIndex_.push_back(i);
        }
        std::vector<size_t> order(modules_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return modules_[a].begin < modules_[b].begin; });
        std::vector<Range> sorted;
        std::vector<uint32_t> index;
        for (const auto i : order) {
            sorted.push_back(modules_[i]);
            index.push_back(moduleIndex_[i]);
        }
        modules_ = std::move(sorted);
        moduleIndex_ = std::move(index);
    }

    // Iterative deepening so the shortest chains are found first when the
    // result limit is reached.
    void Run() {
        for (int depth = 1; depth <= options_.maxLevel && !Stop(); ++depth) {
            chain_.clear();
            Visit(options_.target, 1, depth);
        }
    }

private:
    bool Stop() const {
        return result_.paths.size() >= options_.maxResults || visits_ >= options_.maxVisits ||
               IsCancelled(cancelled_);
    }

    int ModuleOf(uint64_t address) const {
        auto it = std::upper_bound(modules_.begin(), modules_.end(), address,
                                   [](uint64_t v, const Range& range) { return v < range.begin; });
        if (it == modules_.begin()) return -1;
        --it;
        if (address >= it->end) return -1;
        return static_cast<int>(moduleIndex_[static_cast<size_t>(it - modules_.begin())]);
    }

    // Returns false when the search stopped early. An address whose subtree
    // produced no path is remembered with the remaining depth it was searched
    // with, so the many chains that reach it again are not searched twice.
    bool Visit(uint64_t address, int level, int depth) {
        const int remaining = depth - level;
        const size_t before = result_.paths.size();
        bool complete = true;
        const uint64_t low = address > options_.maxOffset ? address - options_.maxOffset : 0;
        auto it = std::lower_bound(slots_.begin(), slots_.end(), low,
                                   [](const Slot& slot, uint64_t value) { return slot.value < value; });
        for (; it != slots_.end() && it->value <= address; ++it) {
            if (Stop()) {
                if (visits_ >= options_.maxVisits || result_.paths.size() >= options_.maxResults) result_.truncated = true;
                return false;
            }
            ++visits_;
            const uint64_t slot = it->address;
            if (std::find(chain_.begin(), chain_.end(), slot) != chain_.end()) {
                complete = false;
                continue;
            }
            const auto offset = static_cast<uint32_t>(address - it->value);
            const int module = ModuleOf(slot);
            if (level == depth) {
                if (module < 0) continue;
                PointerPath path;
                path.module = static_cast<uint32_t>(module);
                path.baseOffset = slot - result_.modules[static_cast<size_t>(module)].base;
                path.offsets.reserve(chain_.size() + 1);
                path.offsets.push_back(offset);
                for (auto rit = offsets_.rbegin(); rit != offsets_.rend(); ++rit) path.offsets.push_back(*rit);
                result_.paths.push_back(std::move(path));
                continue;
            }
            // A static slot ends a chain; longer chains through it add nothing.
            if (module >= 0) continue;
            const auto dead = dead_.find(slot);
            if (dead != dead_.end() && (dead->second >> (remaining - 1)) & 1u) continue;
            chain_.push_back(slot);
            offsets_.push_back(offset);
            const bool finished = Visit(slot, level + 1, depth);
            chain_.pop_back();
            offsets_.pop_back();
            if (!finished) return false;
        }
        if (complete && result_.paths.size() == before) dead_[address] |= 1u << remaining;
        return true;
    }

    const std::vector<Slot>& slots_;
    const PointerScanOptions& options_;
    PointerScanResult& result_;
    const std::atomic_bool* cancelled_;
    std::vector<Range> modules_;
    std::vector<uint32_t> moduleIndex_;
    std::vector<uint64_t> chain_;
    std::vector<uint32_t> offsets_;
    // Bit r set: no chain of exactly r + 1 more slots reaches a static base.
    std::unordered_map<uint64_t, uint32_t> dead_;
    uint64_t visits_ = 0;
};

const PointerModule* FindModule(const std::vector<PointerModule>& modules, const std::string& name) {
    for (const auto& module : modules) {
        if (module.name.size() != name.size()) continue;
        bool same = true;
        for (size_t i = 0; i < name.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(module.name[i])) ==
                   std::tolower(static_cast<unsigned char>(name[i]));
        if (same) return &module;
    }
    return nullptr;
}

} // namespace

bool PointerScanner::Scan(const target::SessionPtr& session, const PointerScanOptions& options,
                          PointerScanResult& result, std::string* error, const std::atomic_bool* cancelled,
                          ScanProgress* progress) {
    if (error) error->clear();
    result = PointerScanResult{};
    const auto started = std::chrono::steady_clock::now();
    if (!session || !session->Alive()) {
        if (error) *error = "no_active_session";
        return false;
    }
    if (options.target == 0 || options.maxLevel < 1 || options.maxLevel > 12 ||
        (options.pointerSize != 4 && options.pointerSize != 8)) {
        if (error) *error = "Enter a target address, a level between 1 and 12 and a pointer size of 4 or 8";
        return false;
    }
    if (options.modules.empty()) {
        if (error) *error = "No module to use as a static base";
        return false;
    }
    result.target = options.target;
    result.pointerSize = options.pointerSize;
    result.modules = options.modules;

    const auto regions = session->MemoryRegions();
    std::vector<Range> valid;
    std::vector<target::MemoryRegion> holders;
    for (const auto& region : regions) {
        if (!region.readable || region.size == 0) continue;
        valid.push_back({region.base, region.base + region.size});
        if (!region.writable) continue;
        if (region.type == target::MemoryRegionType::Mapped && !options.includeMapped) continue;
        holders.push_back(region);
    }
    valid = Merge(std::move(valid));
    if (valid.empty() || holders.empty()) {
        if (error) *error = "No readable memory";
        return false;
    }

    struct Piece {
        uint64_t base;
        uint64_t size;
    };
    std::vector<Piece> pieces;
    uint64_t total = 0;
    for (const auto& region : holders) {
        for (uint64_t offset = 0; offset < region.size; offset += kChunk) {
            pieces.push_back({region.base + offset, std::min(kChunk, region.size - offset)});
            total += pieces.back().size;
        }
    }
    if (progress) {
        progress->total.store(total);
        progress->done.store(0);
    }

    // Index every stored pointer into readable memory.
    const unsigned workers = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::vector<Slot>> found(pieces.size());
    std::atomic<size_t> next{0};
    const uint64_t minValid = valid.front().begin;
    const uint64_t maxValid = valid.back().end;
    const unsigned size = options.pointerSize;
    const unsigned step = options.alignedOnly ? size : 1;
    auto work = [&]() {
        std::vector<uint8_t> buffer;
        for (;;) {
            if (IsCancelled(cancelled)) return;
            const size_t index = next.fetch_add(1);
            if (index >= pieces.size()) return;
            const auto& piece = pieces[index];
            buffer.resize(static_cast<size_t>(piece.size));
            size_t read = 0;
            if (session->ReadMemory(piece.base, buffer.data(), buffer.size(), &read)) {
                auto& out = found[index];
                for (uint64_t p = (piece.base % step) ? step - piece.base % step : 0; p + size <= piece.size; p += step) {
                    uint64_t value = 0;
                    std::memcpy(&value, buffer.data() + p, size);
                    if (value < minValid || value >= maxValid || !Inside(valid, value)) continue;
                    out.push_back({value, piece.base + p});
                }
            }
            if (progress) progress->done.fetch_add(piece.size);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 1; i < workers; ++i) threads.emplace_back(work);
    work();
    for (auto& thread : threads) thread.join();
    if (IsCancelled(cancelled)) {
        if (error) *error = "scan_cancelled";
        return false;
    }

    std::vector<Slot> slots;
    size_t count = 0;
    for (const auto& part : found) count += part.size();
    try {
        slots.reserve(count);
    } catch (const std::bad_alloc&) {
        if (error) *error = "Not enough memory to index " + std::to_string(count) + " pointers";
        return false;
    }
    for (auto& part : found) {
        slots.insert(slots.end(), part.begin(), part.end());
        std::vector<Slot>().swap(part);
    }
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
        return a.value < b.value || (a.value == b.value && a.address < b.address);
    });
    result.pointersIndexed = slots.size();

    Search search(slots, options, result, cancelled);
    search.Run();
    if (IsCancelled(cancelled)) {
        if (error) *error = "scan_cancelled";
        return false;
    }
    std::stable_sort(result.paths.begin(), result.paths.end(), [](const PointerPath& a, const PointerPath& b) {
        return a.offsets.size() < b.offsets.size();
    });
    result.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

bool PointerScanner::Resolve(const target::SessionPtr& session, const PointerScanResult& result,
                             const PointerPath& path, const std::vector<PointerModule>& currentModules,
                             uint64_t& address) {
    if (!session || path.module >= result.modules.size()) return false;
    const PointerModule* module = FindModule(currentModules, result.modules[path.module].name);
    if (!module) return false;
    address = module->base + path.baseOffset;
    for (const auto offset : path.offsets) {
        uint64_t value = 0;
        if (!session->ReadMemory(address, &value, result.pointerSize)) return false;
        address = value + offset;
    }
    return true;
}

size_t PointerScanner::Rescan(const target::SessionPtr& session, PointerScanResult& result, uint64_t newTarget,
                              const std::vector<PointerModule>& currentModules) {
    std::vector<PointerPath> kept;
    for (auto& path : result.paths) {
        uint64_t address = 0;
        if (Resolve(session, result, path, currentModules, address) && address == newTarget)
            kept.push_back(std::move(path));
    }
    result.paths = std::move(kept);
    result.target = newTarget;
    // Later resolutions use the bases the paths were just checked against.
    for (auto& module : result.modules) {
        if (const PointerModule* current = FindModule(currentModules, module.name)) {
            module.base = current->base;
            module.size = current->size;
        }
    }
    return result.paths.size();
}

std::string PointerScanner::Format(const PointerScanResult& result, const PointerPath& path) {
    char buffer[40] = {};
    std::string text = path.module < result.modules.size() ? result.modules[path.module].name : std::string("?");
    std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(path.baseOffset));
    text += buffer;
    for (const auto offset : path.offsets) {
        std::snprintf(buffer, sizeof(buffer), " -> %X", offset);
        text += buffer;
    }
    return text;
}

bool PointerScanner::Save(const std::string& file, const PointerScanResult& result, std::string* error) {
    try {
        json document;
        document["format"] = "cortex-pointer-scan";
        document["version"] = 1;
        document["target"] = result.target;
        document["pointer_size"] = result.pointerSize;
        document["modules"] = json::array();
        for (const auto& module : result.modules)
            document["modules"].push_back({{"name", module.name}, {"base", module.base}, {"size", module.size}});
        document["paths"] = json::array();
        for (const auto& path : result.paths)
            document["paths"].push_back({{"m", path.module}, {"b", path.baseOffset}, {"o", path.offsets}});
        std::ofstream output(std::filesystem::u8path(file), std::ios::trunc);
        if (!output) {
            if (error) *error = "Cannot write " + file;
            return false;
        }
        output << document.dump();
        return static_cast<bool>(output);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

bool PointerScanner::Load(const std::string& file, PointerScanResult& result, std::string* error) {
    try {
        std::ifstream input(std::filesystem::u8path(file));
        if (!input) {
            if (error) *error = "Cannot read " + file;
            return false;
        }
        const json document = json::parse(input);
        if (document.value("format", std::string()) != "cortex-pointer-scan") {
            if (error) *error = "Not a Cortex pointer scan file";
            return false;
        }
        PointerScanResult loaded;
        loaded.target = document.value("target", uint64_t{0});
        loaded.pointerSize = document.value("pointer_size", 8u);
        for (const auto& module : document.at("modules"))
            loaded.modules.push_back({module.value("name", std::string()), module.value("base", uint64_t{0}),
                                      module.value("size", uint64_t{0})});
        for (const auto& entry : document.at("paths")) {
            PointerPath path;
            path.module = entry.value("m", 0u);
            path.baseOffset = entry.value("b", uint64_t{0});
            path.offsets = entry.value("o", std::vector<uint32_t>());
            if (path.module < loaded.modules.size() && !path.offsets.empty()) loaded.paths.push_back(std::move(path));
        }
        result = std::move(loaded);
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = std::string("Invalid pointer scan file: ") + ex.what();
        return false;
    }
}

} // namespace cortex::services

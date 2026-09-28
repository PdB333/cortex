#include "memory_workspace.h"
#include "widgets.h"
#include "address_context_menu.h"

#include "process/process_control.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

namespace cortex::ui {
namespace {

using services::ScanCompare;
using services::ScanDataType;
using services::ScanTristate;
using services::ValueScanner;

constexpr ScanDataType kTypes[] = {
    ScanDataType::Byte, ScanDataType::Int16, ScanDataType::Int32, ScanDataType::Int64,
    ScanDataType::Float, ScanDataType::Double, ScanDataType::String, ScanDataType::ByteArray,
    ScanDataType::AllNumeric
};

const char* const kRoundingNames[] = {
    "Rounded (default)", "Rounded (extreme)", "Truncated", "Exact"
};

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool ParseHex(const std::string& raw, uint64_t& value) {
    std::string text = Trim(raw);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.erase(0, 2);
    if (text.empty() || text.size() > 16) return false;
    for (const char ch : text)
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    value = std::strtoull(text.c_str(), nullptr, 16);
    return true;
}

bool IsText(ScanDataType type) {
    return type == ScanDataType::String || type == ScanDataType::ByteArray;
}

// Addresses workspace and live watches name numeric types this way.
std::string PersistentType(ScanDataType type, bool isUnsigned) {
    switch (type) {
        case ScanDataType::Byte: return isUnsigned ? "u8" : "i8";
        case ScanDataType::Int16: return isUnsigned ? "u16" : "i16";
        case ScanDataType::Int32: return isUnsigned ? "u32" : "i32";
        case ScanDataType::Int64: return isUnsigned ? "u64" : "i64";
        case ScanDataType::Float: return "float";
        case ScanDataType::Double: return "double";
        default: return {};
    }
}

ScanDataType TypeFromSetting(const std::string& value) {
    if (value == "byte") return ScanDataType::Byte;
    if (value == "i16") return ScanDataType::Int16;
    if (value == "i64") return ScanDataType::Int64;
    if (value == "f32") return ScanDataType::Float;
    if (value == "f64") return ScanDataType::Double;
    if (value == "string") return ScanDataType::String;
    if (value == "bytes") return ScanDataType::ByteArray;
    if (value == "all") return ScanDataType::AllNumeric;
    return ScanDataType::Int32;
}

int RoundingFromSetting(const std::string& value) {
    if (value == "extreme") return 1;
    if (value == "truncated") return 2;
    if (value == "exact") return 3;
    return 0;
}

// -1, 0 or 1 comparing two stored values of a numeric type.
int CompareStored(ScanDataType type, bool isUnsigned, const std::vector<uint8_t>& left,
                  const std::vector<uint8_t>& right) {
    const size_t size = ValueScanner::TypeSize(type);
    if (!size || left.size() < size || right.size() < size) return 0;
    if (type == ScanDataType::Float || type == ScanDataType::Double) {
        double a = 0.0;
        double b = 0.0;
        if (type == ScanDataType::Float) {
            float fa = 0.0f;
            float fb = 0.0f;
            std::memcpy(&fa, left.data(), 4);
            std::memcpy(&fb, right.data(), 4);
            a = fa;
            b = fb;
        } else {
            std::memcpy(&a, left.data(), 8);
            std::memcpy(&b, right.data(), 8);
        }
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    uint64_t a = 0;
    uint64_t b = 0;
    std::memcpy(&a, left.data(), size);
    std::memcpy(&b, right.data(), size);
    if (!isUnsigned) {
        const unsigned shift = static_cast<unsigned>(64 - size * 8);
        const int64_t sa = shift ? static_cast<int64_t>(a << shift) >> shift : static_cast<int64_t>(a);
        const int64_t sb = shift ? static_cast<int64_t>(b << shift) >> shift : static_cast<int64_t>(b);
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    }
    return a < b ? -1 : (a > b ? 1 : 0);
}

bool TypeCombo(const char* id, int* index, bool includeAll) {
    bool changed = false;
    const auto current = static_cast<ScanDataType>(*index);
    if (ImGui::BeginCombo(id, ValueScanner::TypeName(current))) {
        for (const auto type : kTypes) {
            if (!includeAll && type == ScanDataType::AllNumeric) continue;
            const bool selected = type == current;
            if (ImGui::Selectable(ValueScanner::TypeName(type), selected)) {
                *index = static_cast<int>(type);
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

// ------------------------------------------------------------------ scanning

ScanDataType MemoryWorkspace::SelectedType() const {
    return static_cast<ScanDataType>(typeIndex_);
}

std::vector<ScanCompare> MemoryWorkspace::AvailableCompares() const {
    const ScanDataType type = scan_ ? scan_->type : SelectedType();
    if (!scan_) {
        if (IsText(type)) return {ScanCompare::Exact};
        std::vector<ScanCompare> list = {ScanCompare::Exact, ScanCompare::Bigger, ScanCompare::Smaller,
                                         ScanCompare::Between};
        if (type != ScanDataType::AllNumeric) list.push_back(ScanCompare::Unknown);
        return list;
    }
    if (IsText(type))
        return {ScanCompare::Exact, ScanCompare::Changed, ScanCompare::Unchanged, ScanCompare::SameAsFirst};
    return {ScanCompare::Exact, ScanCompare::Bigger, ScanCompare::Smaller, ScanCompare::Between,
            ScanCompare::Increased, ScanCompare::IncreasedBy, ScanCompare::Decreased,
            ScanCompare::DecreasedBy, ScanCompare::Changed, ScanCompare::Unchanged,
            ScanCompare::SameAsFirst};
}

services::ScanQuery MemoryWorkspace::BuildQuery() const {
    services::ScanQuery query;
    query.type = scan_ ? scan_->type : SelectedType();
    query.compare = compare_;
    query.value = scanValue_;
    query.value2 = scanValue2_;
    query.hex = hex_;
    query.unsignedValues = unsigned_;
    query.invert = invert_;
    query.compareToFirst = compareToFirst_ && scan_ != nullptr;
    query.rounding = static_cast<services::FloatRounding>(std::clamp(rounding_, 0, 3));
    query.utf16 = scan_ ? scan_->firstQuery.utf16 : utf16_;
    query.caseSensitive = caseSensitive_;
    return query;
}

bool MemoryWorkspace::BuildOptions(UiContext& context, services::ScanOptions& options,
                                   std::string& error) const {
    uint64_t start = 0;
    uint64_t stop = 0;
    if (!ParseHex(start_, start) || !ParseHex(stop_, stop)) {
        error = "Start and stop must be hexadecimal addresses";
        return false;
    }
    if (stop < start) {
        error = "The stop address is below the start address";
        return false;
    }
    options.start = start;
    options.stop = stop == std::numeric_limits<uint64_t>::max() ? stop : stop + 1;
    options.writable = writable_;
    options.executable = executable_;
    options.copyOnWrite = copyOnWrite_;
    options.fastScan = fastScan_;
    options.alignment = static_cast<uint32_t>(std::max(0, alignment_));
    options.includePrivate = includePrivate_;
    options.includeImage = includeImage_;
    options.includeMapped = includeMapped_;
    if (context.settings) {
        options.maxResults = static_cast<size_t>(context.settings->Values().maxScanResults);
        options.threads = static_cast<unsigned>(std::max(0, context.settings->Values().scanThreads));
    }
    return true;
}

void MemoryWorkspace::ApplyDefaults(UiContext& context) {
    if (!context.settings) return;
    const auto& values = context.settings->Values();
    typeIndex_ = static_cast<int>(TypeFromSetting(values.defaultScanType));
    fastScan_ = values.scanFastScan;
    pauseWhileScanning_ = values.scanPauseTarget;
    includePrivate_ = values.scanPrivateMemory;
    includeImage_ = values.scanImageMemory;
    includeMapped_ = values.scanMappedMemory;
    rounding_ = RoundingFromSetting(values.scanFloatRounding);
}

void MemoryWorkspace::CancelScan() {
    if (scanRunning_ && scanCancel_) scanCancel_->store(true, std::memory_order_relaxed);
}

void MemoryWorkspace::SetScan(services::ScanStatePtr state) {
    scan_ = std::move(state);
    selected_.assign(scan_ ? scan_->Size() : 0, 0);
    selectedCount_ = 0;
    liveCache_.clear();
}

void MemoryWorkspace::ResetForTarget(UiContext& context, const std::string& targetId) {
    CancelScan();
    SetScan(nullptr);
    undo_.reset();
    addresses_.clear();
    modules_.clear();
    lastModuleRefresh_ = {};
    compare_ = ScanCompare::Exact;
    regionIndex_ = 0;
    editAddressIndex_ = -1;
    activeTargetId_ = targetId;
    ApplyDefaults(context);
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const bool x86 = session && session->Target().architecture == target::Architecture::X86;
    std::snprintf(start_, sizeof(start_), "%s", "0");
    std::snprintf(stop_, sizeof(stop_), "%s", x86 ? "FFFFFFFF" : "7FFFFFFFFFFF");
}

void MemoryWorkspace::NewScan(UiContext& context) {
    CancelScan();
    SetScan(nullptr);
    undo_.reset();
    const auto compares = AvailableCompares();
    if (std::find(compares.begin(), compares.end(), compare_) == compares.end())
        compare_ = ScanCompare::Exact;
    compareToFirst_ = false;
    focusValue_ = true;
    context.status = "Ready for a new scan";
}

void MemoryWorkspace::UndoScan(UiContext& context) {
    if (!undo_ || scanRunning_) return;
    SetScan(undo_);
    undo_.reset();
    context.status = "Last scan undone: " + GroupDigits(scan_->count) + " result(s)";
}

void MemoryWorkspace::PollScan(UiContext& context) {
    if (!scanRunning_ || !scanFuture_.valid()) return;
    if (scanFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;

    ScanTask task = scanFuture_.get();
    scanRunning_ = false;
    scanCancel_.reset();
    scanProgress_.reset();

    if (!task.state) {
        lastScanError_ = task.error;
        context.status = task.error == "scan_cancelled"
            ? "Scan cancelled"
            : (task.error.empty() ? "Scan failed" : "Scan failed: " + task.error);
        return;
    }
    lastScanError_.clear();
    if (!scanWasFirst_) undo_ = scan_;
    SetScan(task.state);
    char timing[64] = {};
    std::snprintf(timing, sizeof(timing), " in %.0f ms", scan_->milliseconds);
    context.status = "Found " + GroupDigits(scan_->count) + timing;
    if (!scan_->Listed()) context.status += " - narrow them down with a next scan";
    if (scan_->limitReached) context.status += " - limit reached, other matches were not kept";
    if (!task.pauseError.empty()) context.status += " (the target could not be paused: " + task.pauseError + ")";
}

void MemoryWorkspace::StartScan(UiContext& context) {
    if (scanRunning_ || !context.sessions) return;
    auto session = context.sessions->Active();
    if (!session) {
        context.status = "Select a process first";
        return;
    }
    const auto query = BuildQuery();
    const bool first = !scan_;
    services::ScanOptions options;
    if (first) {
        std::string error;
        if (!BuildOptions(context, options, error)) {
            context.status = error;
            return;
        }
    }
    if (ValueScanner::NeedsValue(query.compare) && Trim(query.value).empty()) {
        context.status = "Enter a value to scan for";
        return;
    }

    const bool pause = pauseWhileScanning_;
    const uint64_t pid = session->Target().processId;
    auto previous = scan_;
    scanCancel_ = std::make_shared<std::atomic_bool>(false);
    scanProgress_ = std::make_shared<services::ScanProgress>();
    auto cancel = scanCancel_;
    auto progress = scanProgress_;
    scanWasFirst_ = first;
    scanRunning_ = true;
    context.status = first ? "Scanning process memory..." : "Scanning previous results...";

    scanFuture_ = std::async(std::launch::async,
        [session, query, options, previous, first, pause, pid, cancel, progress]() {
            ScanTask task;
            std::unique_ptr<process_control::SuspendGuard> paused;
            if (pause) {
                paused = std::make_unique<process_control::SuspendGuard>(pid);
                if (!paused->Active()) task.pauseError = paused->Error();
            }
            task.state = first
                ? ValueScanner::FirstScan(session, query, options, &task.error, cancel.get(), progress.get())
                : ValueScanner::NextScan(session, previous, query, &task.error, cancel.get(), progress.get());
            return task;
        });
}

// ------------------------------------------------------------------ results

void MemoryWorkspace::RefreshModules(UiContext& context, bool force) {
    if (!context.modules) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && lastModuleRefresh_.time_since_epoch().count() != 0 &&
        now - lastModuleRefresh_ < std::chrono::seconds(5)) return;
    lastModuleRefresh_ = now;
    std::string error;
    auto modules = context.modules->List(&error);
    std::sort(modules.begin(), modules.end(),
              [](const auto& left, const auto& right) { return left.base < right.base; });
    modules_ = std::move(modules);
    if (regionIndex_ > static_cast<int>(modules_.size())) regionIndex_ = 0;
}

const target::ModuleInfo* MemoryWorkspace::ModuleFor(uint64_t address) const {
    auto it = std::upper_bound(modules_.begin(), modules_.end(), address,
                               [](uint64_t value, const auto& module) { return value < module.base; });
    if (it == modules_.begin()) return nullptr;
    --it;
    return address < it->base + it->size ? &*it : nullptr;
}

std::string MemoryWorkspace::AddressText(uint64_t address, bool& isStatic) const {
    const auto* module = ModuleFor(address);
    isStatic = module != nullptr;
    if (!module) return Hex(address);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(address - module->base));
    return module->name + buffer;
}

bool MemoryWorkspace::ResolveAddress(UiContext& context, const std::string& raw, uint64_t& address) {
    const std::string text = Trim(raw);
    if (ParseHex(text, address) && address != 0) return true;
    const auto plus = text.find_last_of('+');
    if (plus == std::string::npos || plus == 0) return false;
    uint64_t offset = 0;
    if (!ParseHex(text.substr(plus + 1), offset)) return false;
    const std::string name = Lower(Trim(text.substr(0, plus)));
    RefreshModules(context, true);
    for (const auto& module : modules_) {
        if (Lower(module.name) != name) continue;
        address = module.base + offset;
        return true;
    }
    return false;
}

const std::vector<uint8_t>& MemoryWorkspace::LiveValue(UiContext& context, size_t index) {
    static const std::vector<uint8_t> kEmpty;
    if (!scan_ || index >= scan_->Size() || !context.memory) return kEmpty;
    auto found = liveCache_.find(index);
    if (found != liveCache_.end()) return found->second;
    std::vector<uint8_t> value;
    std::string error;
    if (!context.memory->Read(scan_->addresses[index], scan_->HitSize(index), value, &error) ||
        value.size() != scan_->HitSize(index))
        value.clear();
    return liveCache_.emplace(index, std::move(value)).first->second;
}

std::string MemoryWorkspace::FormatHit(const uint8_t* data, size_t index) const {
    return ValueScanner::Format(data, scan_->HitSize(index), scan_->HitType(index), hex_, unsigned_,
                                scan_->firstQuery.utf16);
}

std::vector<size_t> MemoryWorkspace::ActionRows(size_t clicked) const {
    if (clicked < selected_.size() && selected_[clicked] && selectedCount_ > 1) {
        std::vector<size_t> rows;
        rows.reserve(static_cast<size_t>(selectedCount_));
        for (size_t i = 0; i < selected_.size(); ++i)
            if (selected_[i]) rows.push_back(i);
        return rows;
    }
    return {clicked};
}

void MemoryWorkspace::ApplySelection(ImGuiMultiSelectIO* io) {
    if (!io) return;
    for (const ImGuiSelectionRequest& request : io->Requests) {
        if (request.Type == ImGuiSelectionRequestType_SetAll) {
            std::fill(selected_.begin(), selected_.end(), request.Selected ? 1 : 0);
            selectedCount_ = request.Selected ? static_cast<int>(selected_.size()) : 0;
        } else if (request.Type == ImGuiSelectionRequestType_SetRange) {
            const auto first = static_cast<size_t>(std::max<ImGuiSelectionUserData>(0, request.RangeFirstItem));
            const auto last = static_cast<size_t>(std::max<ImGuiSelectionUserData>(0, request.RangeLastItem));
            for (size_t i = first; i <= last && i < selected_.size(); ++i) {
                const uint8_t value = request.Selected ? 1 : 0;
                if (selected_[i] != value) selectedCount_ += request.Selected ? 1 : -1;
                selected_[i] = value;
            }
        }
    }
}

void MemoryWorkspace::AddResultsToList(const std::vector<size_t>& rows) {
    if (!scan_) return;
    for (const size_t row : rows) {
        if (row >= scan_->Size()) continue;
        const uint64_t address = scan_->addresses[row];
        const ScanDataType type = scan_->HitType(row);
        if (HasAddress(address, type)) continue;
        AddressEntry entry;
        entry.address = address;
        entry.type = type;
        entry.size = scan_->HitSize(row);
        entry.utf16 = scan_->firstQuery.utf16;
        entry.hex = hex_ && ValueScanner::IsInteger(type);
        entry.unsignedValue = unsigned_;
        entry.description = "No description";
        entry.lastValue.assign(scan_->Value(row), scan_->Value(row) + entry.size);
        entry.frozenValue = entry.lastValue;
        addresses_.push_back(std::move(entry));
    }
}

void MemoryWorkspace::SaveResultsToProject(UiContext& context, const std::vector<size_t>& rows) {
    if (!scan_ || !context.projectModel || !context.mutationAllowed) {
        context.status = "Allow writes to save addresses to the project";
        return;
    }
    size_t saved = 0;
    std::string error;
    for (const size_t row : rows) {
        if (row >= scan_->Size()) continue;
        const std::string type = PersistentType(scan_->HitType(row), unsigned_);
        if (type.empty()) continue;
        const std::string address = Hex(scan_->addresses[row]);
        const std::string notes = "Scan value: " + FormatHit(scan_->Value(row), row);
        if (context.projectModel->SetAddress("Scan " + address, address, type, notes,
                                             context.mutationAllowed, &error))
            ++saved;
    }
    if (saved == 0) {
        context.status = error.empty() ? "Only numeric results can be saved to Addresses"
                                       : "Save to Addresses failed: " + error;
        return;
    }
    context.status = std::to_string(saved) + " address(es) saved to Addresses";
    context.requestWorkspace = "addresses";
}

void MemoryWorkspace::RemoveResults(UiContext& context, const std::vector<size_t>& rows) {
    if (!scan_ || rows.empty() || !scan_->Listed()) return;
    auto reduced = ValueScanner::RemoveResults(scan_, rows);
    undo_ = scan_;
    SetScan(std::move(reduced));
    context.status = std::to_string(rows.size()) + " result(s) removed; Undo scan restores them";
}

void MemoryWorkspace::CopyResults(const std::vector<size_t>& rows) const {
    if (!scan_) return;
    std::string text;
    for (const size_t row : rows) {
        if (row >= scan_->Size()) continue;
        text += Hex(scan_->addresses[row]);
        text += '\t';
        text += FormatHit(scan_->Value(row), row);
        text += '\n';
    }
    ImGui::SetClipboardText(text.c_str());
}

// ------------------------------------------------------------------ address list

bool MemoryWorkspace::HasAddress(uint64_t address, ScanDataType type) const {
    return std::any_of(addresses_.begin(), addresses_.end(), [&](const AddressEntry& entry) {
        return entry.address == address && entry.type == type;
    });
}

std::string MemoryWorkspace::FormatEntry(const AddressEntry& entry) const {
    if (!entry.readable) return "??";
    return ValueScanner::Format(entry.lastValue.data(), entry.lastValue.size(), entry.type, entry.hex,
                                entry.unsignedValue, entry.utf16);
}

std::string MemoryWorkspace::PointerText(const AddressEntry& entry) const {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(entry.baseOffset));
    std::string text = entry.module + buffer;
    for (const auto offset : entry.offsets) {
        std::snprintf(buffer, sizeof(buffer), " -> %X", offset);
        text += buffer;
    }
    return text;
}

bool MemoryWorkspace::ResolvePointer(UiContext& context, AddressEntry& entry) {
    const target::ModuleInfo* module = nullptr;
    for (const auto& candidate : modules_)
        if (Lower(candidate.name) == Lower(entry.module)) module = &candidate;
    if (!module || !context.memory) return false;
    uint64_t address = module->base + entry.baseOffset;
    for (const auto offset : entry.offsets) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!context.memory->Read(address, entry.pointerSize, bytes, &error) || bytes.size() != entry.pointerSize)
            return false;
        uint64_t value = 0;
        std::memcpy(&value, bytes.data(), entry.pointerSize);
        address = value + offset;
    }
    entry.address = address;
    return true;
}

void MemoryWorkspace::TakePendingAddresses(UiContext& context) {
    if (context.pendingAddresses.empty()) return;
    for (auto& pending : context.pendingAddresses) {
        AddressEntry entry;
        entry.description = pending.description.empty() ? "No description" : pending.description;
        entry.type = pending.type;
        entry.size = std::max<size_t>(1, ValueScanner::TypeSize(pending.type));
        entry.address = pending.address;
        entry.pointer = !pending.module.empty();
        entry.module = pending.module;
        entry.baseOffset = pending.baseOffset;
        entry.offsets = pending.offsets;
        entry.pointerSize = pending.pointerSize;
        addresses_.push_back(std::move(entry));
    }
    context.pendingAddresses.clear();
    lastAddressRefresh_ = {};
}

void MemoryWorkspace::RefreshAddressValues(UiContext& context) {
    if (!context.memory) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastAddressRefresh_.time_since_epoch().count() != 0 &&
        now - lastAddressRefresh_ < std::chrono::milliseconds(100)) return;
    lastAddressRefresh_ = now;

    for (auto& entry : addresses_) {
        std::vector<uint8_t> value;
        std::string error;
        if (entry.pointer && !ResolvePointer(context, entry)) {
            entry.readable = false;
            continue;
        }
        entry.readable = context.memory->Read(entry.address, std::max<size_t>(1, entry.size), value, &error) &&
                         value.size() == std::max<size_t>(1, entry.size);
        if (!entry.readable) continue;
        entry.lastValue = std::move(value);

        if (!entry.freeze || !context.mutationAllowed || entry.frozenValue.empty()) continue;
        if (entry.freezeMode != FreezeMode::Always && ValueScanner::IsNumeric(entry.type)) {
            const int order = CompareStored(entry.type, entry.unsignedValue, entry.lastValue, entry.frozenValue);
            if ((entry.freezeMode == FreezeMode::AllowIncrease && order > 0) ||
                (entry.freezeMode == FreezeMode::AllowDecrease && order < 0)) {
                entry.frozenValue = entry.lastValue;
                continue;
            }
        }
        if (entry.lastValue != entry.frozenValue)
            context.memory->Write(entry.address, entry.frozenValue, true, &error);
    }
}

bool MemoryWorkspace::AddManualAddress(UiContext& context) {
    if (!context.memory) return false;
    uint64_t address = 0;
    if (!ResolveAddress(context, addAddress_, address)) {
        context.status = "Enter a hexadecimal address or module+offset";
        return false;
    }
    const auto type = static_cast<ScanDataType>(addTypeIndex_);
    if (HasAddress(address, type)) {
        context.status = "Address is already in the list";
        return false;
    }
    size_t size = ValueScanner::TypeSize(type);
    if (!size) {
        const int length = std::clamp(addLength_, 1, 4096);
        size = static_cast<size_t>(length) * (type == ScanDataType::String && addUtf16_ ? 2 : 1);
    }
    std::vector<uint8_t> value;
    std::string error;
    if (!context.memory->Read(address, size, value, &error)) {
        context.status = "Unable to read address: " + error;
        return false;
    }
    AddressEntry entry;
    entry.address = address;
    entry.type = type;
    entry.size = size;
    entry.utf16 = type == ScanDataType::String && addUtf16_;
    entry.description = *addDescription_ ? std::string(addDescription_) : "No description";
    entry.lastValue = value;
    entry.frozenValue = value;
    addresses_.push_back(std::move(entry));
    context.status = "Address added";
    return true;
}

void MemoryWorkspace::BeginValueEdit(size_t index) {
    if (index >= addresses_.size()) return;
    editAddressIndex_ = static_cast<int>(index);
    const std::string current = FormatEntry(addresses_[index]);
    std::snprintf(editValue_, sizeof(editValue_), "%s", current.c_str());
    openEditValue_ = true;
}

bool MemoryWorkspace::CommitValueEdit(UiContext& context) {
    if (!context.memory || !context.mutationAllowed || editAddressIndex_ < 0 ||
        editAddressIndex_ >= static_cast<int>(addresses_.size())) {
        context.status = "Allow writes before changing a value";
        return false;
    }
    auto& entry = addresses_[static_cast<size_t>(editAddressIndex_)];
    std::vector<uint8_t> bytes;
    std::string error;
    if (!ValueScanner::Encode(editValue_, entry.type, entry.hex, entry.utf16, bytes, &error)) {
        context.status = error;
        return false;
    }
    if (!context.memory->Write(entry.address, bytes, true, &error)) {
        context.status = "Value write failed: " + error;
        return false;
    }
    entry.size = bytes.size();
    entry.lastValue = bytes;
    entry.frozenValue = bytes;
    context.status = "Value written";
    return true;
}

void MemoryWorkspace::SaveEntryToProject(UiContext& context, const AddressEntry& entry) {
    const std::string type = PersistentType(entry.type, entry.unsignedValue);
    if (!context.projectModel || !context.mutationAllowed) {
        context.status = "Allow writes to save addresses to the project";
        return;
    }
    if (type.empty()) {
        context.status = "Only numeric entries can be saved to Addresses";
        return;
    }
    bool isStatic = false;
    const std::string address = AddressText(entry.address, isStatic);
    std::string error;
    if (!context.projectModel->SetAddress(entry.description, isStatic ? address : Hex(entry.address), type,
                                          "From the Memory address list", context.mutationAllowed, &error)) {
        context.status = "Save to Addresses failed: " + error;
        return;
    }
    context.status = "Saved to Addresses";
}

// ------------------------------------------------------------------ drawing

void MemoryWorkspace::DrawWelcome(UiContext& context) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float cardWidth = std::min(Px(540.0f), available.x - Px(30.0f));
    const float cardHeight = Px(220.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (available.x - cardWidth) * 0.5f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(20.0f, (available.y - cardHeight) * 0.28f));

    ImGui::BeginChild("WelcomeCard", ImVec2(cardWidth, cardHeight), ImGuiChildFlags_Borders);
    ImGui::Dummy(ImVec2(0, Px(10)));
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted("Cortex Memory");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    HintText("Select a process, scan a value, then keep useful addresses below.");
    ImGui::Dummy(ImVec2(0, Px(18)));

    const float width = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = std::min(Px(250.0f), width);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - buttonWidth) * 0.5f);
    if (ImGui::Button("Select a process", ImVec2(buttonWidth, Px(42)))) context.requestProcessPicker = true;

    ImGui::Dummy(ImVec2(0, Px(12)));
    HintText("Process  ->  Scan  ->  Address list  ->  Edit / Freeze");
    ImGui::EndChild();
}

void MemoryWorkspace::DrawResults(UiContext& context, float height) {
    ImGui::BeginChild("ScanResults", ImVec2(0, height), ImGuiChildFlags_Borders);
    ImGui::AlignTextToFramePadding();
    const std::string found = "Found: " + (scan_ ? GroupDigits(scan_->count) : std::string("0"));
    ImGui::TextUnformatted(found.c_str());
    if (scan_) {
        char info[96] = {};
        std::snprintf(info, sizeof(info), "scan %u, %.0f ms", scan_->scanNumber, scan_->milliseconds);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", info);
        if (scan_->limitReached) {
            ImGui::SameLine();
            ImGui::TextColored(WarningTextColor(), "limit reached");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The first scan stopped at the Maximum listed scan results setting.\n"
                                  "Other matching addresses were not kept. Raise the limit in\n"
                                  "Settings or scan a more specific value.");
        }
    }
    if (selectedCount_ > 0) {
        const std::string add = "Add " + std::to_string(selectedCount_) + " to list";
        FlowSameLine(ButtonWidth(add.c_str()));
        if (ImGui::Button(add.c_str())) {
            std::vector<size_t> rows;
            for (size_t i = 0; i < selected_.size(); ++i)
                if (selected_[i]) rows.push_back(i);
            AddResultsToList(rows);
        }
    }
    ImGui::Separator();

    if (!scan_) {
        ImGui::Dummy(ImVec2(0, Px(12)));
        HintText(scanRunning_ ? "Scanning..." : "Run a first scan to list matching addresses here.");
        ImGui::EndChild();
        return;
    }
    if (!scan_->Listed()) {
        ImGui::Dummy(ImVec2(0, Px(12)));
        const std::string text = GroupDigits(scan_->count) +
            " candidate addresses are kept as a memory snapshot. Change the value in the target, "
            "then run a next scan (changed, unchanged, increased, decreased...). Results are listed "
            "once at most " + GroupDigits(scan_->options.maxResults) + " remain.";
        HintText(text.c_str());
        ImGui::EndChild();
        return;
    }
    if (scan_->Size() == 0) {
        ImGui::Dummy(ImVec2(0, Px(12)));
        ImGui::TextDisabled("No matching values. Undo scan goes back to the previous results.");
        ImGui::EndChild();
        return;
    }

    const bool showType = scan_->type == ScanDataType::AllNumeric;
    const int columns = showType ? 5 : 4;
    if (BeginDataTable("ResultsTable", columns,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable,
                       ImGui::GetContentRegionAvail(), 90.0f)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("First", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 0.22f);
        if (showType) ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableHeadersRow();

        const auto now = std::chrono::steady_clock::now();
        const int refreshMs = context.settings ? context.settings->Values().scanResultRefreshMs : 500;
        if (now - lastLiveRefresh_ > std::chrono::milliseconds(refreshMs)) {
            liveCache_.clear();
            lastLiveRefresh_ = now;
        }

        const int count = static_cast<int>(scan_->Size());
        ImGuiMultiSelectIO* selection = ImGui::BeginMultiSelect(
            ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_BoxSelect1d, selectedCount_, count);
        ApplySelection(selection);
        ImGuiListClipper clipper;
        clipper.Begin(count);
        if (selection->RangeSrcItem != -1) clipper.IncludeItemByIndex(static_cast<int>(selection->RangeSrcItem));
        bool addRow = false;
        size_t addIndex = 0;
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = static_cast<size_t>(row);
                const uint64_t address = scan_->addresses[index];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(row);
                bool isStatic = false;
                const std::string addressText = AddressText(address, isStatic);
                ImGui::SetNextItemSelectionUserData(row);
                if (isStatic) ImGui::PushStyleColor(ImGuiCol_Text, StaticAddressColor());
                {
                    MonoFont mono;
                    ImGui::Selectable(addressText.c_str(), selected_[index] != 0,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
                }
                if (isStatic) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    addRow = true;
                    addIndex = index;
                }
                if (isStatic && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s (inside a module: stable as module+offset)", Hex(address).c_str());
                if (ImGui::BeginPopupContextItem("ResultMenu")) {
                    const auto rows = ActionRows(index);
                    const std::string suffix = rows.size() > 1 ? " (" + std::to_string(rows.size()) + ")" : "";
                    if (ImGui::MenuItem(("Add to address list" + suffix).c_str(), "Enter")) AddResultsToList(rows);
                    ImGui::BeginDisabled(!context.mutationAllowed || !context.projectModel);
                    if (ImGui::MenuItem(("Save to Addresses" + suffix).c_str())) SaveResultsToProject(context, rows);
                    ImGui::EndDisabled();
                    if (ImGui::MenuItem(("Remove from results" + suffix).c_str(), "Delete")) RemoveResults(context, rows);
                    if (ImGui::MenuItem(("Copy" + suffix).c_str(), "Ctrl+C")) CopyResults(rows);
                    ImGui::Separator();
                    AddressContextOptions options;
                    options.label = "Scan " + Hex(address);
                    options.valueType = PersistentType(scan_->HitType(index), unsigned_);
                    if (options.valueType.empty()) options.valueType = "i32";
                    options.valueSize = static_cast<int>(scan_->HitSize(index));
                    DrawAddressContextActions(context, address, options);
                    ImGui::EndPopup();
                }

                const auto& live = LiveValue(context, index);
                const size_t size = scan_->HitSize(index);
                ImGui::TableSetColumnIndex(1);
                if (live.empty()) {
                    ImGui::TextDisabled("??");
                } else {
                    const bool changed = std::memcmp(live.data(), scan_->Value(index), size) != 0;
                    const std::string text = FormatHit(live.data(), index);
                    if (changed) ImGui::TextColored(ChangedValueColor(), "%s", text.c_str());
                    else ImGui::TextUnformatted(text.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(FormatHit(scan_->Value(index), index).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(FormatHit(scan_->FirstValue(index), index).c_str());
                if (showType) {
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(ValueScanner::TypeName(scan_->HitType(index)));
                }
                ImGui::PopID();
            }
        }
        selection = ImGui::EndMultiSelect();
        ApplySelection(selection);
        ImGui::EndTable();

        if (addRow) {
            AddResultsToList({addIndex});
            context.status = "Added to the address list";
        }
    }

    // Keyboard actions on the selected results.
    if (selectedCount_ > 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        std::vector<size_t> rows;
        auto collect = [&]() {
            rows.clear();
            for (size_t i = 0; i < selected_.size(); ++i)
                if (selected_[i]) rows.push_back(i);
        };
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            collect();
            RemoveResults(context, rows);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            collect();
            AddResultsToList(rows);
            context.status = std::to_string(rows.size()) + " result(s) added to the address list";
        } else if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            collect();
            CopyResults(rows);
            context.status = std::to_string(rows.size()) + " result(s) copied";
        }
    }
    ImGui::EndChild();
}

void MemoryWorkspace::DrawScanOptions(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!ImGui::CollapsingHeader("Memory scan options")) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s memory, %s%s", regionIndex_ == 0 ? "All" : "Module",
                              writable_ == ScanTristate::Yes ? "writable only" : "any protection",
                              fastScan_ ? ", fast scan" : "");
        }
        return;
    }
    ImGui::BeginDisabled(scan_ != nullptr);
    ImGui::TextDisabled("Region");
    ImGui::SetNextItemWidth(-1);
    const std::string current = regionIndex_ == 0 || regionIndex_ > static_cast<int>(modules_.size())
        ? std::string("All memory")
        : modules_[static_cast<size_t>(regionIndex_ - 1)].name;
    if (ImGui::BeginCombo("##ScanRegion", current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("All memory", regionIndex_ == 0)) {
            regionIndex_ = 0;
            const bool x86 = session && session->Target().architecture == target::Architecture::X86;
            std::snprintf(start_, sizeof(start_), "%s", "0");
            std::snprintf(stop_, sizeof(stop_), "%s", x86 ? "FFFFFFFF" : "7FFFFFFFFFFF");
        }
        for (size_t i = 0; i < modules_.size(); ++i) {
            const auto& module = modules_[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(module.name.c_str(), regionIndex_ == static_cast<int>(i + 1))) {
                regionIndex_ = static_cast<int>(i + 1);
                std::snprintf(start_, sizeof(start_), "%llX", static_cast<unsigned long long>(module.base));
                std::snprintf(stop_, sizeof(stop_), "%llX",
                              static_cast<unsigned long long>(module.base + module.size - 1));
                // Module images are mostly read-only or copy-on-write.
                writable_ = ScanTristate::Any;
                copyOnWrite_ = ScanTristate::Any;
                includeImage_ = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const bool pair = half >= Px(110.0f);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Start");
    ImGui::SetNextItemWidth(pair ? half : -1);
    {
        MonoFont mono;
        ImGui::InputText("##ScanStart", start_, sizeof(start_), ImGuiInputTextFlags_CharsHexadecimal);
    }
    ImGui::EndGroup();
    if (pair) ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("Stop");
    ImGui::SetNextItemWidth(pair ? half : -1);
    {
        MonoFont mono;
        ImGui::InputText("##ScanStop", stop_, sizeof(stop_), ImGuiInputTextFlags_CharsHexadecimal);
    }
    ImGui::EndGroup();

    TristateCheckbox("Writable", &writable_, ScanTristate::Yes, ScanTristate::No, ScanTristate::Any);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Checked: writable only. Empty: read-only only. Dash: both.");
    FlowSameLine(CheckboxWidth("Executable"));
    TristateCheckbox("Executable", &executable_, ScanTristate::Yes, ScanTristate::No, ScanTristate::Any);
    FlowSameLine(CheckboxWidth("Copy on write"));
    TristateCheckbox("Copy on write", &copyOnWrite_, ScanTristate::Yes, ScanTristate::No, ScanTristate::Any);

    ImGui::Checkbox("Fast scan", &fastScan_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only test addresses aligned to the alignment (the value size when 0).\n"
                          "Much faster; values are almost always aligned.");
    FlowSameLine(TextWidth("Alignment") + Px(110));
    ImGui::BeginDisabled(!fastScan_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Alignment");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(90));
    if (ImGui::InputInt("##ScanAlignment", &alignment_)) alignment_ = std::clamp(alignment_, 0, 4096);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = the size of the value type");
    ImGui::EndDisabled();

    ImGui::Checkbox("Private", &includePrivate_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Heap and stack memory (MEM_PRIVATE)");
    FlowSameLine(CheckboxWidth("Image"));
    ImGui::Checkbox("Image", &includeImage_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modules: executables and DLLs (MEM_IMAGE)");
    FlowSameLine(CheckboxWidth("Mapped"));
    ImGui::Checkbox("Mapped", &includeMapped_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mapped files and shared memory (MEM_MAPPED)");
    ImGui::EndDisabled();

    ImGui::Checkbox("Pause the target while scanning", &pauseWhileScanning_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Suspends every thread of the target for the duration of each scan,\n"
                          "so values cannot change while they are read.");
}

void MemoryWorkspace::DrawScanPanel(UiContext& context, float height) {
    ImGui::BeginChild("ScanPanel", ImVec2(0, height), ImGuiChildFlags_Borders);
    const bool first = !scan_;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float full = ImGui::GetContentRegionAvail().x;
    const float buttonHeight = ImGui::GetFrameHeight() * 1.2f;

    ImGui::BeginDisabled(scanRunning_);
    if (first) {
        if (ImGui::Button("First scan", ImVec2(full, buttonHeight))) StartScan(context);
    } else {
        const float third = (full - spacing * 2.0f) / 3.0f;
        const bool row = third >= std::max({ButtonWidth("Next scan"), ButtonWidth("New scan"), ButtonWidth("Undo scan")});
        const float half = (full - spacing) * 0.5f;
        if (ImGui::Button("Next scan", ImVec2(row ? third : full, buttonHeight))) StartScan(context);
        if (row) ImGui::SameLine();
        if (ImGui::Button("New scan", ImVec2(row ? third : half, buttonHeight))) NewScan(context);
        ImGui::SameLine();
        ImGui::BeginDisabled(!undo_);
        if (ImGui::Button("Undo scan", ImVec2(row ? third : half, buttonHeight))) UndoScan(context);
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();

    if (scanRunning_) {
        const double total = scanProgress_ ? static_cast<double>(scanProgress_->total.load()) : 0.0;
        const double done = scanProgress_ ? static_cast<double>(scanProgress_->done.load()) : 0.0;
        const float fraction = total > 0.0 ? static_cast<float>(std::min(1.0, done / total)) : 0.0f;
        ImGui::ProgressBar(fraction, ImVec2(full - ButtonWidth("Cancel") - spacing, 0));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) CancelScan();
    }
    ImGui::Spacing();

    const ScanDataType type = scan_ ? scan_->type : SelectedType();
    const bool integer = ValueScanner::IsInteger(type) || type == ScanDataType::AllNumeric;
    const bool real = type == ScanDataType::Float || type == ScanDataType::Double || type == ScanDataType::AllNumeric;
    const bool needsValue = ValueScanner::NeedsValue(compare_);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(compare_ == ScanCompare::Between ? "Values" : "Value");
    if (integer) {
        const float hexWidth = CheckboxWidth("Hex");
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - hexWidth));
        ImGui::Checkbox("Hex", &hex_);
    }
    ImGui::BeginDisabled(!needsValue);
    if (focusValue_) {
        ImGui::SetKeyboardFocusHere();
        focusValue_ = false;
    }
    ImGui::SetNextItemWidth(-1);
    const char* hint = type == ScanDataType::ByteArray ? "8B 05 ?? ?? 4? 00" : (type == ScanDataType::String ? "text" : "100");
    bool submit = ImGui::InputTextWithHint("##ScanValue", hint, scanValue_, sizeof(scanValue_),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    if (compare_ == ScanCompare::Between) {
        ImGui::SetNextItemWidth(-1);
        submit |= ImGui::InputTextWithHint("##ScanValue2", "and", scanValue2_, sizeof(scanValue2_),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    }
    ImGui::EndDisabled();
    if (submit && !scanRunning_) {
        StartScan(context);
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::TextDisabled("Scan type");
    ImGui::SetNextItemWidth(-1);
    const auto compares = AvailableCompares();
    if (std::find(compares.begin(), compares.end(), compare_) == compares.end()) compare_ = compares.front();
    if (ImGui::BeginCombo("##ScanCompare", ValueScanner::CompareName(compare_))) {
        for (const auto compare : compares) {
            if (ImGui::Selectable(ValueScanner::CompareName(compare), compare == compare_)) compare_ = compare;
            if (compare == compare_) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::TextDisabled("Value type");
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(scan_ != nullptr);
    if (scan_) {
        int locked = static_cast<int>(scan_->type);
        TypeCombo("##ScanType", &locked, true);
    } else {
        TypeCombo("##ScanType", &typeIndex_, true);
    }
    ImGui::EndDisabled();

    // Options for the chosen type.
    bool placed = false;
    auto flow = [&](const char* label) {
        if (placed) FlowSameLine(CheckboxWidth(label));
        placed = true;
    };
    if (integer) {
        flow("Unsigned");
        ImGui::Checkbox("Unsigned", &unsigned_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Compare and show integers as unsigned values");
    }
    if (type == ScanDataType::String) {
        flow("UTF-16");
        ImGui::BeginDisabled(scan_ != nullptr);
        bool utf16 = scan_ ? scan_->firstQuery.utf16 : utf16_;
        if (ImGui::Checkbox("UTF-16", &utf16) && !scan_) utf16_ = utf16;
        ImGui::EndDisabled();
        flow("Case sensitive");
        ImGui::Checkbox("Case sensitive", &caseSensitive_);
    }
    flow("Not");
    ImGui::Checkbox("Not", &invert_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the addresses that do NOT match");
    if (scan_ && !IsText(type)) {
        flow("Compare to first scan");
        ImGui::Checkbox("Compare to first scan", &compareToFirst_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Increased, decreased, changed and unchanged compare with\n"
                              "the first scan's values instead of the previous scan's.");
    }
    if (real) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Rounding");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##ScanRounding", &rounding_, kRoundingNames, IM_ARRAYSIZE(kRoundingNames));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How a typed decimal matches a stored float:\n"
                              "Rounded: 1.25 matches 1.245 to 1.255\n"
                              "Extreme: 1.25 matches 0.25 to 2.25\n"
                              "Truncated: 1.25 matches 1.25 to 1.26\n"
                              "Exact: only the nearest float");
    }

    ImGui::Spacing();
    DrawScanOptions(context);

    ImGui::Spacing();
    HintText("Double-click a result to add it to the address list below. Enter in the value box scans again.");
    if (!lastScanError_.empty() && lastScanError_ != "scan_cancelled") {
        ImGui::PushStyleColor(ImGuiCol_Text, WarningTextColor());
        ImGui::TextWrapped("%s", lastScanError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

void MemoryWorkspace::DrawAddressList(UiContext& context) {
    RefreshAddressValues(context);

    ImGui::BeginChild("AddressList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Address list");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", addresses_.size());
    FlowSameLine(ButtonWidth("+ Add address"));
    if (ImGui::Button("+ Add address")) openAddAddress_ = true;
    if (!addresses_.empty()) {
        FlowSameLine(ButtonWidth("Clear"));
        if (ImGui::Button("Clear")) {
            addresses_.clear();
            context.status = "Address list cleared";
        }
    }
    ImGui::Separator();

    if (addresses_.empty()) {
        ImGui::Dummy(ImVec2(0, Px(12)));
        HintText("Double-click scan results to collect them here, then freeze or edit their values. "
                 "Right-click an entry to save it to Addresses.");
        ImGui::EndChild();
        return;
    }

    if (BeginDataTable("AddressTable", 5,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_ScrollY,
                       ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed, Px(56.0f));
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.14f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < addresses_.size();) {
            auto& entry = addresses_[i];
            bool remove = false;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Checkbox("##Freeze", &entry.freeze) && entry.freeze) entry.frozenValue = entry.lastValue;
            ImGui::EndDisabled();
            if (entry.freeze && entry.freezeMode != FreezeMode::Always) {
                ImGui::SameLine(0, Px(2));
                ImGui::TextDisabled(entry.freezeMode == FreezeMode::AllowIncrease ? "+" : "-");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(context.mutationAllowed ? "Freeze: keep writing this value (Space)"
                                                          : "Allow writes to freeze values");

            ImGui::TableSetColumnIndex(1);
            std::array<char, 160> description{};
            std::snprintf(description.data(), description.size(), "%s", entry.description.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##Description", description.data(), description.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                entry.description = description.data();
            if (ImGui::IsItemDeactivatedAfterEdit()) entry.description = description.data();

            ImGui::TableSetColumnIndex(2);
            bool isStatic = false;
            const std::string addressText = entry.pointer
                ? "P->" + (entry.readable ? Hex(entry.address) : std::string("????????"))
                : AddressText(entry.address, isStatic);
            if (isStatic || entry.pointer) ImGui::PushStyleColor(ImGuiCol_Text, StaticAddressColor());
            {
                MonoFont mono;
                ImGui::Selectable(addressText.c_str(), entry.selected);
            }
            if (isStatic || entry.pointer) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", entry.pointer ? PointerText(entry).c_str() : Hex(entry.address).c_str());
            ImGui::OpenPopupOnItemClick("EntryMenu");

            ImGui::TableSetColumnIndex(3);
            const std::string typeText = std::string(ValueScanner::TypeName(entry.type)) +
                (entry.type == ScanDataType::String && entry.utf16 ? " (UTF-16)" : "");
            ImGui::TextUnformatted(typeText.c_str());
            ImGui::OpenPopupOnItemClick("EntryMenu");

            ImGui::TableSetColumnIndex(4);
            const std::string value = FormatEntry(entry);
            const bool changed = entry.readable && entry.freeze && entry.lastValue != entry.frozenValue;
            if (changed) ImGui::PushStyleColor(ImGuiCol_Text, ChangedValueColor());
            if (ImGui::Selectable(value.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (context.mutationAllowed) BeginValueEdit(i);
                else context.status = "Allow writes to edit a value";
            }
            if (changed) ImGui::PopStyleColor();
            if (ImGui::IsItemFocused() && context.mutationAllowed && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                entry.freeze = !entry.freeze;
                if (entry.freeze) entry.frozenValue = entry.lastValue;
            }

            ImGui::OpenPopupOnItemClick("EntryMenu");
            if (ImGui::BeginPopup("EntryMenu")) {
                ImGui::BeginDisabled(!context.mutationAllowed);
                if (ImGui::MenuItem("Edit value...")) BeginValueEdit(i);
                if (ImGui::MenuItem(entry.freeze ? "Unfreeze" : "Freeze", "Space")) {
                    entry.freeze = !entry.freeze;
                    if (entry.freeze) entry.frozenValue = entry.lastValue;
                }
                ImGui::EndDisabled();
                if (ValueScanner::IsNumeric(entry.type) && ImGui::BeginMenu("Freeze mode")) {
                    if (ImGui::MenuItem("Always write the value", nullptr, entry.freezeMode == FreezeMode::Always))
                        entry.freezeMode = FreezeMode::Always;
                    if (ImGui::MenuItem("Allow increases", nullptr, entry.freezeMode == FreezeMode::AllowIncrease))
                        entry.freezeMode = FreezeMode::AllowIncrease;
                    if (ImGui::MenuItem("Allow decreases", nullptr, entry.freezeMode == FreezeMode::AllowDecrease))
                        entry.freezeMode = FreezeMode::AllowDecrease;
                    ImGui::EndMenu();
                }
                if (ValueScanner::IsInteger(entry.type)) {
                    ImGui::MenuItem("Show as hexadecimal", nullptr, &entry.hex);
                    ImGui::MenuItem("Show as unsigned", nullptr, &entry.unsignedValue);
                }
                if (ImGui::BeginMenu("Change type")) {
                    for (const auto type : kTypes) {
                        if (type == ScanDataType::AllNumeric) continue;
                        if (ImGui::MenuItem(ValueScanner::TypeName(type), nullptr, entry.type == type) &&
                            entry.type != type) {
                            const size_t fixed = ValueScanner::TypeSize(type);
                            entry.size = fixed ? fixed : std::max<size_t>(entry.size, 8);
                            entry.type = type;
                            entry.freeze = false;
                            entry.lastValue.clear();
                            lastAddressRefresh_ = {};
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                ImGui::BeginDisabled(!context.mutationAllowed || !context.projectModel ||
                                     PersistentType(entry.type, entry.unsignedValue).empty());
                if (ImGui::MenuItem("Save to Addresses")) SaveEntryToProject(context, entry);
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(Hex(entry.address).c_str());
                if (isStatic && ImGui::MenuItem("Copy module+offset")) ImGui::SetClipboardText(addressText.c_str());
                ImGui::Separator();
                AddressContextOptions options;
                options.label = entry.description;
                options.valueType = PersistentType(entry.type, entry.unsignedValue);
                if (options.valueType.empty()) options.valueType = "i32";
                options.valueSize = static_cast<int>(entry.size);
                DrawAddressContextActions(context, entry.address, options);
                ImGui::Separator();
                if (ImGui::MenuItem("Remove", "Delete")) remove = true;
                ImGui::EndPopup();
            }

            ImGui::PopID();
            if (remove) addresses_.erase(addresses_.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void MemoryWorkspace::DrawDialogs(UiContext& context) {
    if (openAddAddress_) {
        ImGui::OpenPopup("Add address");
        openAddAddress_ = false;
        addAddress_[0] = '\0';
        addDescription_[0] = '\0';
        addTypeIndex_ = static_cast<int>(ScanDataType::Int32);
        addLength_ = 16;
        addUtf16_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(430), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add address", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Address");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ManualAddress", "7FF6A1B20010 or game.exe+1A2B", addAddress_, sizeof(addAddress_));
        ImGui::TextDisabled("Description");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ManualDescription", "Health", addDescription_, sizeof(addDescription_));
        ImGui::TextDisabled("Type");
        ImGui::SetNextItemWidth(-1);
        TypeCombo("##ManualType", &addTypeIndex_, false);
        const auto type = static_cast<ScanDataType>(addTypeIndex_);
        if (IsText(type)) {
            ImGui::TextDisabled(type == ScanDataType::String ? "Length (characters)" : "Length (bytes)");
            ImGui::SetNextItemWidth(Px(140));
            if (ImGui::InputInt("##ManualLength", &addLength_)) addLength_ = std::clamp(addLength_, 1, 4096);
            if (type == ScanDataType::String) {
                ImGui::SameLine();
                ImGui::Checkbox("UTF-16", &addUtf16_);
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(Px(120), Px(34))) && AddManualAddress(context)) ImGui::CloseCurrentPopup();
        FlowSameLine(Px(100));
        if (ImGui::Button("Cancel", ImVec2(Px(100), Px(34)))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openEditValue_) {
        ImGui::OpenPopup("Edit value");
        openEditValue_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(430), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Edit value", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (editAddressIndex_ >= 0 && editAddressIndex_ < static_cast<int>(addresses_.size())) {
            const auto& entry = addresses_[static_cast<size_t>(editAddressIndex_)];
            MonoText("%s  (%s%s)", Hex(entry.address).c_str(), ValueScanner::TypeName(entry.type),
                     entry.hex ? ", hexadecimal" : "");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##EditedValue", editValue_, sizeof(editValue_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Spacing();
            ImGui::BeginDisabled(!context.mutationAllowed);
            if ((ImGui::Button("Write value", ImVec2(Px(130), Px(34))) || (enter && context.mutationAllowed)) &&
                CommitValueEdit(context))
                ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            FlowSameLine(Px(100));
            if (ImGui::Button("Cancel", ImVec2(Px(100), Px(34)))) ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled("Address no longer exists.");
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void MemoryWorkspace::HandleCommand(UiContext& context, const std::string& command) {
    if (command.rfind("scan_range ", 0) == 0) {
        std::istringstream words(command.substr(11));
        std::string start;
        std::string stop;
        uint64_t from = 0;
        uint64_t to = 0;
        if (words >> start >> stop && ParseHex(start, from) && ParseHex(stop, to)) {
            std::snprintf(start_, sizeof(start_), "%llX", static_cast<unsigned long long>(from));
            std::snprintf(stop_, sizeof(stop_), "%llX", static_cast<unsigned long long>(to));
            regionIndex_ = 0;
            writable_ = ScanTristate::Any;
            copyOnWrite_ = ScanTristate::Any;
            if (scan_) NewScan(context);
            context.status = "The next first scan covers " + Hex(from) + " - " + Hex(to);
        }
        return;
    }
    if (command == "scan_cancel") {
        CancelScan();
        return;
    }
    if (command == "scan_undo") {
        UndoScan(context);
        return;
    }
    if (command == "freeze_toggle_all") {
        if (!context.mutationAllowed) {
            context.status = "Allow writes to freeze values";
            return;
        }
        const bool freeze = std::any_of(addresses_.begin(), addresses_.end(),
                                        [](const AddressEntry& entry) { return !entry.freeze; });
        for (auto& entry : addresses_) {
            if (freeze && !entry.freeze) entry.frozenValue = entry.lastValue;
            entry.freeze = freeze;
        }
        context.status = freeze ? "Address list frozen" : "Address list unfrozen";
        return;
    }

    ScanCompare compare = compare_;
    if (command == "scan_exact") compare = ScanCompare::Exact;
    else if (command == "scan_increased") compare = ScanCompare::Increased;
    else if (command == "scan_decreased") compare = ScanCompare::Decreased;
    else if (command == "scan_changed") compare = ScanCompare::Changed;
    else if (command == "scan_unchanged") compare = ScanCompare::Unchanged;
    else if (command != "scan_next") return;

    const bool relative = compare != ScanCompare::Exact && command != "scan_next";
    if (relative && (!scan_ || IsText(scan_->type) && compare != ScanCompare::Changed &&
                                   compare != ScanCompare::Unchanged)) {
        context.status = scan_ ? "This comparison does not apply to the current scan"
                               : "Run a first scan before a relative next scan";
        return;
    }
    if (scanRunning_) {
        context.status = "A scan is already running";
        return;
    }
    compare_ = compare;
    StartScan(context);
}

void MemoryWorkspace::Draw(UiContext& context) {
    PollScan(context);

    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const std::string targetId = session ? session->Target().id : std::string();
    if (targetId != activeTargetId_) ResetForTarget(context, targetId);

    if (!session) {
        DrawWelcome(context);
        DrawDialogs(context);
        return;
    }
    RefreshModules(context);
    TakePendingAddresses(context);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float upperHeight = std::clamp(
        available.y * 0.60f, std::min(Px(260.0f), available.y),
        std::max(Px(260.0f), available.y - ImGui::GetFrameHeightWithSpacing() * 5.0f));
    const float scanPanelWidth = std::clamp(available.x * 0.30f, Px(270.0f), Px(380.0f));
    const float resultsWidth = std::max(Px(160.0f), available.x - scanPanelWidth - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("ResultsColumn", ImVec2(resultsWidth, upperHeight), ImGuiChildFlags_None);
    DrawResults(context, upperHeight);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("ScanColumn", ImVec2(0, upperHeight), ImGuiChildFlags_None);
    DrawScanPanel(context, upperHeight);
    ImGui::EndChild();

    ImGui::Spacing();
    DrawAddressList(context);
    DrawDialogs(context);
}

} // namespace cortex::ui

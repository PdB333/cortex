#include "memory_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cortex::ui {
namespace {

template <typename T>
std::vector<uint8_t> BytesOf(T value) {
    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

template <typename T>
std::string NumberToString(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != sizeof(T)) return "?";
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    std::ostringstream out;
    if constexpr (std::is_floating_point_v<T>) out << std::setprecision(7) << value;
    else out << value;
    return out.str();
}

size_t FixedKindSize(services::ScanValueKind kind) {
    switch (kind) {
        case services::ScanValueKind::I32:
        case services::ScanValueKind::F32: return 4;
        case services::ScanValueKind::I64:
        case services::ScanValueKind::F64: return 8;
        default: return 0;
    }
}

} // namespace

services::ScanValueKind MemoryWorkspace::SelectedKind() const {
    switch (typeIndex_) {
        case 1: return services::ScanValueKind::I64;
        case 2: return services::ScanValueKind::F32;
        case 3: return services::ScanValueKind::F64;
        case 4: return services::ScanValueKind::String;
        case 5: return services::ScanValueKind::Bytes;
        default: return services::ScanValueKind::I32;
    }
}

services::ScanComparison MemoryWorkspace::SelectedComparison() const {
    switch (comparisonIndex_) {
        case 1: return services::ScanComparison::Changed;
        case 2: return services::ScanComparison::Unchanged;
        case 3: return services::ScanComparison::Increased;
        case 4: return services::ScanComparison::Decreased;
        default: return services::ScanComparison::Exact;
    }
}

std::vector<uint8_t> MemoryWorkspace::EncodeText(
        const char* raw, services::ScanValueKind kind, std::string& error) const {
    error.clear();
    const std::string text = raw ? raw : "";
    try {
        switch (kind) {
            case services::ScanValueKind::I32: {
                const long long parsed = std::stoll(text);
                if (parsed < std::numeric_limits<int32_t>::min() ||
                    parsed > std::numeric_limits<int32_t>::max()) {
                    error = "Value is outside the Int32 range";
                    return {};
                }
                return BytesOf(static_cast<int32_t>(parsed));
            }
            case services::ScanValueKind::I64:
                return BytesOf(static_cast<int64_t>(std::stoll(text)));
            case services::ScanValueKind::F32:
                return BytesOf(std::stof(text));
            case services::ScanValueKind::F64:
                return BytesOf(std::stod(text));
            case services::ScanValueKind::String:
                if (text.empty()) {
                    error = "Enter a non-empty string";
                    return {};
                } else {
                    return std::vector<uint8_t>(text.begin(), text.end());
                }
            case services::ScanValueKind::Bytes: {
                std::vector<uint8_t> bytes;
                std::istringstream stream(text);
                std::string token;
                while (stream >> token) {
                    size_t used = 0;
                    const unsigned long parsed = std::stoul(token, &used, 16);
                    if (used != token.size() || parsed > 0xff) {
                        error = "Bytes must look like: DE AD BE EF";
                        return {};
                    }
                    bytes.push_back(static_cast<uint8_t>(parsed));
                }
                if (bytes.empty()) error = "Enter one or more hexadecimal bytes";
                return bytes;
            }
        }
    } catch (...) {
        error = "The value does not match the selected type";
    }
    return {};
}

std::vector<uint8_t> MemoryWorkspace::EncodeValue(std::string& error) const {
    return EncodeText(scanValue_, SelectedKind(), error);
}

std::string MemoryWorkspace::FormatValue(
        const std::vector<uint8_t>& value, services::ScanValueKind kind) const {
    switch (kind) {
        case services::ScanValueKind::I32: return NumberToString<int32_t>(value);
        case services::ScanValueKind::I64: return NumberToString<int64_t>(value);
        case services::ScanValueKind::F32: return NumberToString<float>(value);
        case services::ScanValueKind::F64: return NumberToString<double>(value);
        case services::ScanValueKind::String:
            return std::string(value.begin(), value.end());
        case services::ScanValueKind::Bytes: {
            std::ostringstream out;
            out << std::hex << std::uppercase << std::setfill('0');
            for (size_t i = 0; i < value.size(); ++i) {
                if (i) out << ' ';
                out << std::setw(2) << static_cast<unsigned>(value[i]);
            }
            return out.str();
        }
    }
    return {};
}

void MemoryWorkspace::NavigateAddress(UiContext& context, uint64_t address,
                                      const char* workspace) {
    context.navigationAddress = address;
    context.navigationAddressPending = true;
    context.requestWorkspace = workspace ? workspace : "memory-browser";
}

void MemoryWorkspace::FindWriter(UiContext& context, uint64_t address) {
    context.runtimeToolPreset = "re_find_last_writer";
    context.runtimeArgumentsPreset =
        std::string("{\"address\":") + std::to_string(address) +
        ",\"size\":1,\"timeout_ms\":5000,\"mutation_permission\":true}";
    context.requestWorkspace = "runtime";
}

void MemoryWorkspace::ResetForTarget(const std::string& targetId) {
    if (scanRunning_ && scanCancel_) scanCancel_->store(true, std::memory_order_relaxed);
    scanResults_.clear();
    addresses_.clear();
    firstScanDone_ = false;
    comparisonIndex_ = 0;
    editAddressIndex_ = -1;
    activeTargetId_ = targetId;
}

void MemoryWorkspace::NewScan(UiContext& context) {
    if (scanRunning_ && scanCancel_) scanCancel_->store(true, std::memory_order_relaxed);
    scanResults_.clear();
    firstScanDone_ = false;
    comparisonIndex_ = 0;
    context.status = "Ready for a new scan";
}

void MemoryWorkspace::PollScan(UiContext& context) {
    if (!scanRunning_ || !scanFuture_.valid()) return;
    if (scanFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;

    ScanTaskResult result = scanFuture_.get();
    scanRunning_ = false;
    scanCancel_.reset();

    if (!result.ok) {
        context.status = result.error.empty() ? "Scan failed" : "Scan failed: " + result.error;
        return;
    }

    scanResults_ = std::move(result.results);
    firstScanDone_ = true;
    context.status = std::to_string(scanResults_.size()) + " result(s)";
}

void MemoryWorkspace::StartScan(UiContext& context) {
    if (scanRunning_ || !context.sessions) return;
    auto session = context.sessions->Active();
    if (!session) {
        context.status = "Select a process first";
        return;
    }

    std::string parseError;
    std::vector<uint8_t> exactValue;
    const bool exactNeeded =
        !firstScanDone_ || SelectedComparison() == services::ScanComparison::Exact;
    if (exactNeeded) {
        exactValue = EncodeValue(parseError);
        if (!parseError.empty()) {
            context.status = parseError;
            return;
        }
    }

    const bool refine = firstScanDone_;
    const auto kind = SelectedKind();
    const auto comparison = SelectedComparison();
    const auto previous = scanResults_;
    scanCancel_ = std::make_shared<std::atomic_bool>(false);
    auto cancel = scanCancel_;

    scanRunning_ = true;
    context.status = refine ? "Refining scan..." : "Scanning process memory...";

    scanFuture_ = std::async(std::launch::async,
        [session, exactValue = std::move(exactValue), previous, kind, comparison,
         refine, cancel]() mutable {
            ScanTaskResult result;
            if (refine) {
                result.ok = services::ScanService::Refine(
                    session, previous, kind, comparison, exactValue, result.results,
                    &result.error, cancel.get());
            } else {
                result.ok = services::ScanService::Exact(
                    session, exactValue, result.results, 5000, &result.error, cancel.get());
            }
            return result;
        });
}

bool MemoryWorkspace::HasAddress(uint64_t address) const {
    return std::any_of(addresses_.begin(), addresses_.end(),
                       [address](const AddressEntry& entry) {
                           return entry.address == address;
                       });
}

void MemoryWorkspace::AddAddress(const services::ScanResult& result) {
    if (HasAddress(result.address)) return;
    AddressEntry entry;
    entry.address = result.address;
    entry.kind = SelectedKind();
    entry.description = "Address " + std::to_string(addresses_.size() + 1);
    entry.lastValue = result.value;
    entry.frozenValue = result.value;
    addresses_.push_back(std::move(entry));
}

bool MemoryWorkspace::AddManualAddress(UiContext& context) {
    if (!context.memory) return false;

    uint64_t address = 0;
    try {
        size_t used = 0;
        const std::string text(addAddress_);
        address = std::stoull(text, &used, 0);
        if (used != text.size() || address == 0) throw std::runtime_error("bad address");
    } catch (...) {
        context.status = "Invalid address";
        return false;
    }
    if (HasAddress(address)) {
        context.status = "Address is already in the list";
        return false;
    }

    services::ScanValueKind kind = services::ScanValueKind::I32;
    switch (addTypeIndex_) {
        case 1: kind = services::ScanValueKind::I64; break;
        case 2: kind = services::ScanValueKind::F32; break;
        case 3: kind = services::ScanValueKind::F64; break;
        default: break;
    }

    const size_t size = FixedKindSize(kind);
    std::vector<uint8_t> value;
    std::string error;
    if (!context.memory->Read(address, size, value, &error)) {
        context.status = "Unable to read address: " + error;
        return false;
    }

    AddressEntry entry;
    entry.address = address;
    entry.kind = kind;
    entry.description = *addDescription_
        ? std::string(addDescription_)
        : "Address " + std::to_string(addresses_.size() + 1);
    entry.lastValue = value;
    entry.frozenValue = value;
    addresses_.push_back(std::move(entry));
    context.status = "Address added";
    return true;
}

void MemoryWorkspace::BeginValueEdit(size_t index) {
    if (index >= addresses_.size()) return;
    editAddressIndex_ = static_cast<int>(index);
    const std::string current = FormatValue(addresses_[index].lastValue,
                                             addresses_[index].kind);
    std::snprintf(editValue_, sizeof(editValue_), "%s", current.c_str());
    openEditValue_ = true;
}

bool MemoryWorkspace::CommitValueEdit(UiContext& context) {
    if (!context.memory || !context.mutationAllowed ||
        editAddressIndex_ < 0 ||
        editAddressIndex_ >= static_cast<int>(addresses_.size())) {
        context.status = "Enable writes before changing a value";
        return false;
    }

    auto& entry = addresses_[static_cast<size_t>(editAddressIndex_)];
    std::string parseError;
    auto bytes = EncodeText(editValue_, entry.kind, parseError);
    if (!parseError.empty()) {
        context.status = parseError;
        return false;
    }

    std::string error;
    if (!context.memory->Write(entry.address, bytes, true, &error)) {
        context.status = "Value write failed: " + error;
        return false;
    }

    entry.lastValue = bytes;
    entry.frozenValue = bytes;
    context.status = "Value updated";
    return true;
}

void MemoryWorkspace::RefreshAddressValues(UiContext& context) {
    if (!context.memory) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastAddressRefresh_.time_since_epoch().count() != 0 &&
        now - lastAddressRefresh_ < std::chrono::milliseconds(180)) return;
    lastAddressRefresh_ = now;

    for (auto& entry : addresses_) {
        const size_t size = entry.frozenValue.empty()
            ? std::max<size_t>(1, FixedKindSize(entry.kind))
            : entry.frozenValue.size();
        std::vector<uint8_t> value;
        std::string error;
        if (context.memory->Read(entry.address, size, value, &error)) {
            entry.lastValue = std::move(value);
        }

        if (entry.freeze && context.mutationAllowed && !entry.frozenValue.empty()) {
            context.memory->Write(entry.address, entry.frozenValue, true, &error);
        }
    }
}

void MemoryWorkspace::DrawWelcome(UiContext& context) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float cardWidth = std::min(540.0f, available.x - 30.0f);
    const float cardHeight = 220.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (available.x - cardWidth) * 0.5f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         std::max(20.0f, (available.y - cardHeight) * 0.28f));

    ImGui::BeginChild("WelcomeCard", ImVec2(cardWidth, cardHeight), ImGuiChildFlags_Borders);
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted("Cortex Memory");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextDisabled("Select a process, scan a value, then keep useful addresses below.");
    ImGui::Dummy(ImVec2(0, 18));

    const float width = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = std::min(250.0f, width);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - buttonWidth) * 0.5f);
    if (ImGui::Button("Select a process", ImVec2(buttonWidth, 42))) {
        context.requestProcessPicker = true;
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::TextDisabled("Process  ->  Scan  ->  Address list  ->  Edit / Freeze");
    ImGui::EndChild();
}

void MemoryWorkspace::DrawResults(UiContext& context, float height) {
    ImGui::BeginChild("ScanResults", ImVec2(0, height), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Scan results");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", scanResults_.size());
    ImGui::Separator();

    if (scanResults_.empty()) {
        ImGui::Dummy(ImVec2(0, 18));
        ImGui::TextDisabled(firstScanDone_
            ? "No matching values."
            : "Run a first scan to populate this list.");
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("ResultsTable", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableHeadersRow();

        const auto kind = SelectedKind();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(scanResults_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& result = scanResults_[static_cast<size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                char address[32] = {};
                std::snprintf(address, sizeof(address), "0x%llX",
                              static_cast<unsigned long long>(result.address));
                ImGui::PushID(row);
                if (ImGui::Selectable(address, false,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        AddAddress(result);
                        context.status = "Address added to list";
                    }
                }
                if (ImGui::BeginPopupContextItem("ResultMenu")) {
                    if (ImGui::MenuItem("Add to address list")) AddAddress(result);
                    if (ImGui::MenuItem("Browse memory")) {
                        NavigateAddress(context, result.address, "memory-browser");
                    }
                    if (ImGui::MenuItem("Disassemble here")) {
                        NavigateAddress(context, result.address, "disassembly");
                    }
                    if (ImGui::MenuItem("Find what writes this")) {
                        FindWriter(context, result.address);
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(FormatValue(result.value, kind).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void MemoryWorkspace::DrawScanPanel(UiContext& context, float height) {
    ImGui::BeginChild("ScanPanel", ImVec2(0, height), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Value scan");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("Value");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ScanValue", scanValue_, sizeof(scanValue_));

    ImGui::Spacing();
    ImGui::TextDisabled("Type");
    ImGui::SetNextItemWidth(-1);
    const char* types[] = {"Int32", "Int64", "Float", "Double", "String", "Bytes"};
    ImGui::BeginDisabled(firstScanDone_);
    ImGui::Combo("##ScanType", &typeIndex_, types, IM_ARRAYSIZE(types));
    ImGui::EndDisabled();

    if (firstScanDone_) {
        ImGui::Spacing();
        ImGui::TextDisabled("Compare");
        ImGui::SetNextItemWidth(-1);
        const char* comparisons[] = {
            "Exact value", "Changed", "Unchanged", "Increased", "Decreased"
        };
        ImGui::Combo("##ScanComparison", &comparisonIndex_,
                     comparisons, IM_ARRAYSIZE(comparisons));
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::BeginDisabled(scanRunning_);
    if (ImGui::Button(firstScanDone_ ? "Next scan" : "First scan",
                      ImVec2(-1, 40))) {
        StartScan(context);
    }
    ImGui::EndDisabled();

    if (firstScanDone_) {
        if (ImGui::Button("New scan", ImVec2(-1, 32))) NewScan(context);
    }

    if (scanRunning_) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextDisabled("Scanning...");
        if (ImGui::Button("Cancel", ImVec2(-1, 30)) && scanCancel_) {
            scanCancel_->store(true, std::memory_order_relaxed);
        }
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextWrapped("Double-click a result to add it. Right-click for Memory, "
                       "Disassembler or Find what writes this.");
    ImGui::EndChild();
}

void MemoryWorkspace::DrawAddressList(UiContext& context) {
    RefreshAddressValues(context);

    ImGui::BeginChild("AddressList", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Address list");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", addresses_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Add address")) openAddAddress_ = true;
    if (!addresses_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            addresses_.clear();
            context.status = "Address list cleared";
        }
    }
    ImGui::Separator();

    if (addresses_.empty()) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::TextDisabled("Double-click a scan result or use + Add address.");
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("AddressTable", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableHeadersRow();

        static const char* kindNames[] = {
            "Bytes", "Int32", "Int64", "Float", "Double", "String"
        };

        for (size_t i = 0; i < addresses_.size();) {
            auto& entry = addresses_[i];
            bool remove = false;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Checkbox("##Freeze", &entry.freeze) && entry.freeze) {
                entry.frozenValue = entry.lastValue;
            }
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            std::array<char, 160> description{};
            std::snprintf(description.data(), description.size(), "%s",
                          entry.description.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##Description", description.data(),
                                 description.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                entry.description = description.data();
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                entry.description = description.data();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%llX", static_cast<unsigned long long>(entry.address));

            ImGui::TableSetColumnIndex(3);
            const auto kindIndex = static_cast<int>(entry.kind);
            ImGui::TextUnformatted(kindIndex >= 0 &&
                                   kindIndex < IM_ARRAYSIZE(kindNames)
                                       ? kindNames[kindIndex] : "?");

            ImGui::TableSetColumnIndex(4);
            const std::string value = FormatValue(entry.lastValue, entry.kind);
            if (ImGui::Selectable(value.c_str(), false,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (context.mutationAllowed) BeginValueEdit(i);
                    else context.status = "Enable writes to edit a value";
                }
            }
            if (ImGui::BeginPopupContextItem("AddressMenu")) {
                ImGui::BeginDisabled(!context.mutationAllowed);
                if (ImGui::MenuItem("Edit value")) BeginValueEdit(i);
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Browse memory")) {
                    NavigateAddress(context, entry.address, "memory-browser");
                }
                if (ImGui::MenuItem("Disassemble here")) {
                    NavigateAddress(context, entry.address, "disassembly");
                }
                if (ImGui::MenuItem("Find what writes this")) {
                    FindWriter(context, entry.address);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove")) remove = true;
                ImGui::EndPopup();
            }

            ImGui::PopID();
            if (remove) {
                addresses_.erase(addresses_.begin() +
                                 static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
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
        addTypeIndex_ = 0;
    }
    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Add address", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Address");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ManualAddress", "0x7FF...",
                                 addAddress_, sizeof(addAddress_));
        ImGui::TextDisabled("Description");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ManualDescription", "Health",
                                 addDescription_, sizeof(addDescription_));
        ImGui::TextDisabled("Type");
        ImGui::SetNextItemWidth(-1);
        const char* types[] = {"Int32", "Int64", "Float", "Double"};
        ImGui::Combo("##ManualType", &addTypeIndex_, types, IM_ARRAYSIZE(types));
        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(120, 34))) {
            if (AddManualAddress(context)) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 34))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (openEditValue_) {
        ImGui::OpenPopup("Edit value");
        openEditValue_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Edit value", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (editAddressIndex_ >= 0 &&
            editAddressIndex_ < static_cast<int>(addresses_.size())) {
            const auto& entry = addresses_[static_cast<size_t>(editAddressIndex_)];
            ImGui::Text("0x%llX", static_cast<unsigned long long>(entry.address));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##EditedValue", editValue_, sizeof(editValue_));
            ImGui::Spacing();
            ImGui::BeginDisabled(!context.mutationAllowed);
            if (ImGui::Button("Write value", ImVec2(130, 34))) {
                if (CommitValueEdit(context)) ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 34))) ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled("Address no longer exists.");
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void MemoryWorkspace::Draw(UiContext& context) {
    PollScan(context);

    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const std::string targetId = session ? session->Target().id : std::string();
    if (targetId != activeTargetId_) ResetForTarget(targetId);

    if (!session) {
        DrawWelcome(context);
        DrawDialogs(context);
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float upperHeight = std::clamp(available.y * 0.56f, 260.0f, 520.0f);
    const float scanPanelWidth = std::clamp(available.x * 0.28f, 260.0f, 360.0f);
    const float resultsWidth = std::max(
        300.0f, available.x - scanPanelWidth - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginGroup();
    ImGui::BeginChild("ResultsColumn", ImVec2(resultsWidth, upperHeight),
                      ImGuiChildFlags_None);
    DrawResults(context, upperHeight);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::BeginChild("ScanColumn", ImVec2(0, upperHeight), ImGuiChildFlags_None);
    DrawScanPanel(context, upperHeight);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::Spacing();
    DrawAddressList(context);
    DrawDialogs(context);
}

} // namespace cortex::ui

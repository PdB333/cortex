#include "memory_browser_workspace.h"
#include "widgets.h"
#include "address_context_menu.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cortex::ui {
namespace {

using services::ScanDataType;
using services::ValueScanner;

const char* const kDisplayNames[] = {
    "Byte hex", "2 Bytes hex", "4 Bytes hex", "8 Bytes hex", "4 Bytes decimal", "Float", "Double"
};

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
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

// Inspector and edit types.
struct InspectType {
    const char* label;
    ScanDataType type;
    bool isUnsigned;
};

const InspectType kInspect[] = {
    {"Int8", ScanDataType::Byte, false},     {"UInt8", ScanDataType::Byte, true},
    {"Int16", ScanDataType::Int16, false},   {"UInt16", ScanDataType::Int16, true},
    {"Int32", ScanDataType::Int32, false},   {"UInt32", ScanDataType::Int32, true},
    {"Int64", ScanDataType::Int64, false},   {"UInt64", ScanDataType::Int64, true},
    {"Float", ScanDataType::Float, false},   {"Double", ScanDataType::Double, false},
};

// Hex byte patterns search as bytes, anything else as text.
bool LooksLikeBytes(const std::string& text) {
    bool any = false;
    std::string token;
    for (const char ch : text + " ") {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            token += ch;
            continue;
        }
        if (token.empty()) continue;
        if (token.size() != 2) return false;
        for (const char digit : token)
            if (!std::isxdigit(static_cast<unsigned char>(digit)) && digit != '?') return false;
        any = true;
        token.clear();
    }
    return any;
}

} // namespace

size_t MemoryBrowserWorkspace::CellSize() const {
    switch (static_cast<Display>(display_)) {
        case Display::Word: return 2;
        case Display::Dword:
        case Display::Int32:
        case Display::Float: return 4;
        case Display::Qword:
        case Display::Double: return 8;
        default: return 1;
    }
}

void MemoryBrowserWorkspace::GoTo(uint64_t address, bool select) {
    const uint64_t half = kWindow / 2;
    base_ = (address > half ? address - half : 0) & ~static_cast<uint64_t>(0xFF);
    bytes_.clear();
    previous_.clear();
    pageValid_.clear();
    changedAt_.clear();
    lastRefresh_ = {};
    if (select) {
        selected_ = address;
        hasSelection_ = true;
        scrollToSelection_ = true;
        pendingNibble_ = -1;
    }
    std::snprintf(address_, sizeof(address_), "%s", Hex(address).c_str());
}

void MemoryBrowserWorkspace::Refresh(UiContext& context, bool force) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) return;
    static constexpr int rates[] = {100, 250, 500, 1000};
    const auto now = std::chrono::steady_clock::now();
    if (!force) {
        if (!liveRefresh_ && !bytes_.empty()) return;
        if (lastRefresh_.time_since_epoch().count() != 0 &&
            now - lastRefresh_ < std::chrono::milliseconds(rates[std::clamp(liveRateIndex_, 0, 3)]))
            return;
    }
    lastRefresh_ = now;
    std::vector<uint8_t> fresh(kWindow, 0);
    std::vector<uint8_t> valid(kWindow / kPage, 0);
    for (uint64_t page = 0; page < kWindow / kPage; ++page) {
        size_t read = 0;
        valid[page] = session->ReadMemory(base_ + page * kPage, fresh.data() + page * kPage, kPage, &read) ? 1 : 0;
    }
    if (changedAt_.size() != kWindow) changedAt_.assign(kWindow, {});
    if (bytes_.size() == kWindow && pageValid_.size() == valid.size()) {
        for (size_t i = 0; i < kWindow; ++i)
            if (valid[i / kPage] && pageValid_[i / kPage] && fresh[i] != bytes_[i]) changedAt_[i] = now;
    }
    bytes_ = std::move(fresh);
    pageValid_ = std::move(valid);
}

bool MemoryBrowserWorkspace::Readable(uint64_t address, size_t size) const {
    if (address < base_ || address + size > base_ + bytes_.size() || pageValid_.empty()) return false;
    for (uint64_t at = address; at < address + size; ++at)
        if (!pageValid_[(at - base_) / kPage]) return false;
    return true;
}

bool MemoryBrowserWorkspace::ReadCached(uint64_t address, void* buffer, size_t size) const {
    if (!Readable(address, size)) return false;
    std::memcpy(buffer, bytes_.data() + (address - base_), size);
    return true;
}

bool MemoryBrowserWorkspace::WriteBytes(UiContext& context, uint64_t address, const std::vector<uint8_t>& bytes) {
    if (!context.memory || !context.mutationAllowed) {
        context.status = "Allow writes to edit memory";
        return false;
    }
    std::string error;
    if (!context.memory->Write(address, bytes, true, &error)) {
        context.status = "Memory write failed: " + error;
        return false;
    }
    context.status = "Wrote " + std::to_string(bytes.size()) + " byte(s) at " + Hex(address);
    Refresh(context, true);
    return true;
}

std::string MemoryBrowserWorkspace::CellText(uint64_t address) const {
    const size_t size = CellSize();
    uint8_t data[8] = {};
    if (!ReadCached(address, data, size)) return std::string(size * 2, '?');
    char buffer[40] = {};
    uint64_t value = 0;
    std::memcpy(&value, data, size);
    switch (static_cast<Display>(display_)) {
        case Display::Byte: std::snprintf(buffer, sizeof(buffer), "%02X", data[0]); break;
        case Display::Word: std::snprintf(buffer, sizeof(buffer), "%04llX", static_cast<unsigned long long>(value)); break;
        case Display::Dword: std::snprintf(buffer, sizeof(buffer), "%08llX", static_cast<unsigned long long>(value)); break;
        case Display::Qword: std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value)); break;
        case Display::Int32: return ValueScanner::Format(data, 4, ScanDataType::Int32);
        case Display::Float: return ValueScanner::Format(data, 4, ScanDataType::Float);
        case Display::Double: return ValueScanner::Format(data, 8, ScanDataType::Double);
    }
    return buffer;
}

const target::MemoryRegion* MemoryBrowserWorkspace::RegionAt(uint64_t address) const {
    auto it = std::upper_bound(regions_.begin(), regions_.end(), address,
                               [](uint64_t value, const auto& region) { return value < region.base; });
    if (it == regions_.begin()) return nullptr;
    --it;
    return address < it->base + it->size ? &*it : nullptr;
}

std::string MemoryBrowserWorkspace::AddressLabel(uint64_t address, bool& isStatic) const {
    isStatic = false;
    for (const auto& module : modules_) {
        if (address < module.base || address >= module.base + module.size) continue;
        isStatic = true;
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "+%llX", static_cast<unsigned long long>(address - module.base));
        return module.name + buffer;
    }
    return Hex(address);
}

bool MemoryBrowserWorkspace::ResolveAddress(UiContext& context, const std::string& raw, uint64_t& address) {
    const std::string text = Trim(raw);
    if (ParseHex(text, address)) return true;
    const auto plus = text.find_last_of('+');
    if (plus == std::string::npos || plus == 0 || !context.modules) return false;
    uint64_t offset = 0;
    if (!ParseHex(text.substr(plus + 1), offset)) return false;
    std::string error;
    const std::string name = Lower(Trim(text.substr(0, plus)));
    for (const auto& module : context.modules->List(&error)) {
        if (Lower(module.name) != name) continue;
        address = module.base + offset;
        return true;
    }
    return false;
}

void MemoryBrowserWorkspace::Find(UiContext& context, bool fromStart) {
    const std::string text = Trim(find_);
    if (text.empty()) return;
    const auto session = context.sessions->Active();
    services::ScanQuery query;
    query.type = LooksLikeBytes(text) ? ScanDataType::ByteArray : ScanDataType::String;
    query.value = text;
    query.caseSensitive = false;
    services::ScanOptions options;
    options.start = fromStart || !hasSelection_ ? 0 : selected_ + 1;
    options.writable = services::ScanTristate::Any;
    options.copyOnWrite = services::ScanTristate::Any;
    options.includeMapped = true;
    options.maxResults = 1;
    options.threads = 1;
    const auto label = std::string("Searching memory for ") + (query.type == ScanDataType::String ? "text" : "bytes");
    context.RunInBackground(label, [this, session, query, options]() {
        std::string error;
        const auto state = ValueScanner::FirstScan(session, query, options, &error);
        if (!state) {
            findInfo_ = error;
        } else if (state->Size() == 0) {
            findInfo_ = "Not found after " + Hex(options.start) + ". Find from start searches everything.";
        } else {
            findInfo_ = "Found at " + Hex(state->addresses.front());
            GoTo(state->addresses.front());
        }
    });
}

void MemoryBrowserWorkspace::HandleKeyboard(UiContext& context) {
    if (!hasSelection_ || ImGui::GetIO().WantTextInput) return;
    const size_t rowWidth = context.settings ? static_cast<size_t>(context.settings->Values().memoryBytesPerRow) : 16;
    const size_t cell = CellSize();
    uint64_t next = selected_;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && next >= cell) next -= cell;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) next += cell;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && next >= rowWidth) next -= rowWidth;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next += rowWidth;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && next >= rowWidth * 16) next -= rowWidth * 16;
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) next += rowWidth * 16;
    if (next != selected_) {
        selected_ = next;
        pendingNibble_ = -1;
        scrollToSelection_ = true;
        if (selected_ < base_ + kPage || selected_ + cell > base_ + kWindow - kPage) GoTo(selected_);
        return;
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        ImGui::SetClipboardText(Hex(selected_).c_str());
        context.status = "Address copied";
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        editAddress_ = selected_;
        editType_ = 0;
        openEdit_ = true;
        return;
    }
    // Typing hex digits edits the selected byte in place.
    if (static_cast<Display>(display_) != Display::Byte || ImGui::GetIO().KeyCtrl) return;
    int digit = -1;
    for (int i = 0; i < 10 && digit < 0; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + i), false) ||
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i), false))
            digit = i;
    for (int i = 0; i < 6 && digit < 0; ++i)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i), false)) digit = 10 + i;
    if (digit < 0) return;
    if (!context.mutationAllowed) {
        context.status = "Allow writes to edit memory";
        return;
    }
    if (pendingNibble_ < 0) {
        pendingNibble_ = digit;
        return;
    }
    const uint8_t value = static_cast<uint8_t>((pendingNibble_ << 4) | digit);
    pendingNibble_ = -1;
    if (WriteBytes(context, selected_, {value})) selected_ += 1;
}

void MemoryBrowserWorkspace::DrawToolbar(UiContext& context) {
    ImGui::SetNextItemWidth(std::min(Px(230), ImGui::GetContentRegionAvail().x));
    const bool enter = ImGui::InputTextWithHint("##MemoryAddress", "address or module+offset", address_, sizeof(address_),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    FlowSameLine(ButtonWidth("Go"));
    if (ImGui::Button("Go") || enter) {
        uint64_t address = 0;
        if (ResolveAddress(context, address_, address)) {
            context.NavigateTo("memory-browser", address);
        } else {
            context.status = "Enter a hexadecimal address or module+offset";
        }
    }
    FlowSameLine(ButtonWidth("Disassemble"));
    if (ImGui::Button("Disassemble") && hasSelection_) context.NavigateTo("disassembly", selected_);
    FlowSameLine(Px(150));
    ImGui::SetNextItemWidth(Px(150));
    if (ImGui::Combo("##Display", &display_, kDisplayNames, IM_ARRAYSIZE(kDisplayNames))) {
        pendingNibble_ = -1;
        if (hasSelection_) selected_ -= selected_ % CellSize();
    }
    FlowSameLine(CheckboxWidth("Live") + Px(90));
    ImGui::Checkbox("Live", &liveRefresh_);
    ImGui::SameLine();
    const char* rates[] = {"100 ms", "250 ms", "500 ms", "1 s"};
    ImGui::SetNextItemWidth(Px(80));
    ImGui::Combo("##LiveRate", &liveRateIndex_, rates, IM_ARRAYSIZE(rates));

    ImGui::SetNextItemWidth(std::min(Px(260), ImGui::GetContentRegionAvail().x));
    const bool findEnter = ImGui::InputTextWithHint("##Find", "find text or bytes (DE AD ?? EF)", find_, sizeof(find_),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    FlowSameLine(ButtonWidth("Find next"));
    if (ImGui::Button("Find next") || findEnter) Find(context, false);
    FlowSameLine(ButtonWidth("Find from start"));
    if (ImGui::Button("Find from start")) Find(context, true);
    if (!findInfo_.empty()) {
        FlowSameLine(TextWidth(findInfo_.c_str()));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", findInfo_.c_str());
    }

    if (hasSelection_) {
        bool isStatic = false;
        const std::string where = AddressLabel(selected_, isStatic);
        std::string line = "Selected " + Hex(selected_);
        if (isStatic) line += "  =  " + where;
        if (const auto* region = RegionAt(selected_)) {
            line += "  |  region " + Hex(region->base) + " ";
            line += region->readable ? 'R' : '-';
            line += region->writable ? 'W' : '-';
            line += region->executable ? 'X' : '-';
            if (region->copyOnWrite) line += " CoW";
            line += region->type == target::MemoryRegionType::Image ? " image"
                  : region->type == target::MemoryRegionType::Mapped ? " mapped" : " private";
        }
        if (pendingNibble_ >= 0) line += "  |  typing...";
        ImGui::TextDisabled("%s", line.c_str());
    }
}

MemoryBrowserWorkspace::RowLayout MemoryBrowserWorkspace::MeasureRow(UiContext& context) const {
    MonoFont mono;
    const size_t rowWidth = context.settings ? static_cast<size_t>(context.settings->Values().memoryBytesPerRow) : 16;
    const size_t cell = CellSize();
    const float charWidth = ImGui::CalcTextSize("0").x;
    size_t labelChars = Hex(base_ + kWindow).size();
    for (const auto& module : modules_) {
        if (base_ + kWindow <= module.base || base_ >= module.base + module.size) continue;
        labelChars = std::max(labelChars, module.name.size() + 1 + Hex(module.size).size() - 2);
    }
    const size_t textChars = std::max<size_t>(cell * 2, static_cast<Display>(display_) >= Display::Int32 ? 12 : 0);
    RowLayout layout;
    layout.addressWidth = charWidth * static_cast<float>(labelChars + 2);
    layout.cellWidth = charWidth * static_cast<float>(textChars) + Px(6);
    const bool ascii = static_cast<Display>(display_) == Display::Byte;
    layout.width = layout.addressWidth + layout.cellWidth * static_cast<float>(rowWidth / cell) +
                   (ascii ? charWidth * static_cast<float>(rowWidth) + Px(16) : 0.0f) + Px(24);
    return layout;
}

void MemoryBrowserWorkspace::DrawHex(UiContext& context, float width, float height) {
    ImGui::BeginChild("HexView", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (bytes_.empty()) {
        ImGui::TextDisabled("Enter an address and press Go.");
        ImGui::EndChild();
        return;
    }
    const size_t rowWidth = context.settings ? static_cast<size_t>(context.settings->Values().memoryBytesPerRow) : 16;
    const size_t cell = CellSize();
    const size_t cells = rowWidth / cell;
    const bool ascii = static_cast<Display>(display_) == Display::Byte;
    const auto now = std::chrono::steady_clock::now();
    bool openMenu = false;
    {
    MonoFont mono;
    const RowLayout layout = MeasureRow(context);
    const float cellWidth = layout.cellWidth;
    const float addressWidth = layout.addressWidth;
    const int rows = static_cast<int>(kWindow / rowWidth);
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();

    if (scrollToSelection_ && hasSelection_ && selected_ >= base_ && selected_ < base_ + kWindow) {
        const float y = static_cast<float>((selected_ - base_) / rowWidth) * rowHeight;
        const float visible = ImGui::GetWindowHeight();
        if (y < ImGui::GetScrollY() || y > ImGui::GetScrollY() + visible - rowHeight * 2)
            ImGui::SetScrollY(std::max(0.0f, y - visible * 0.4f));
        scrollToSelection_ = false;
    }

    ImGuiListClipper clipper;
    clipper.Begin(rows, rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const uint64_t rowAddress = base_ + static_cast<uint64_t>(row) * rowWidth;
            ImGui::PushID(row);
            bool isStatic = false;
            const std::string label = AddressLabel(rowAddress, isStatic);
            if (isStatic) ImGui::TextColored(StaticAddressColor(), "%s", label.c_str());
            else ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine(addressWidth);
            for (size_t c = 0; c < cells; ++c) {
                const uint64_t address = rowAddress + c * cell;
                const std::string text = CellText(address);
                bool changed = false;
                if (changedAt_.size() == kWindow)
                    for (size_t i = 0; i < cell; ++i)
                        changed |= now - changedAt_[address - base_ + i] < std::chrono::milliseconds(1200);
                const bool readable = Readable(address, cell);
                const bool selected = hasSelection_ && selected_ >= address && selected_ < address + cell;
                ImGui::PushID(static_cast<int>(c));
                if (!readable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                else if (changed) ImGui::PushStyleColor(ImGuiCol_Text, ChangedValueColor());
                const std::string shown = selected && pendingNibble_ >= 0
                    ? std::string(1, "0123456789ABCDEF"[pendingNibble_]) + "_" : text;
                if (ImGui::Selectable(shown.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(cellWidth - Px(4), 0))) {
                    selected_ = address;
                    hasSelection_ = true;
                    pendingNibble_ = -1;
                    std::snprintf(address_, sizeof(address_), "%s", Hex(address).c_str());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        editAddress_ = address;
                        editType_ = static_cast<Display>(display_) == Display::Float ? 8
                                  : static_cast<Display>(display_) == Display::Double ? 9
                                  : static_cast<Display>(display_) == Display::Int32 ? 4
                                  : cell == 8 ? 7 : cell == 4 ? 5 : cell == 2 ? 3 : 1;
                        openEdit_ = true;
                    }
                }
                if (!readable || changed) ImGui::PopStyleColor();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    selected_ = address;
                    hasSelection_ = true;
                    menuAddress_ = address;
                    openMenu = true;
                }
                ImGui::PopID();
                ImGui::SameLine(0, Px(4));
            }
            if (ascii) {
                std::string text;
                for (size_t i = 0; i < rowWidth; ++i) {
                    uint8_t byte = 0;
                    text += ReadCached(rowAddress + i, &byte, 1) ? (std::isprint(byte) ? static_cast<char>(byte) : '.') : '?';
                }
                ImGui::SameLine(0, Px(12));
                ImGui::TextUnformatted(text.c_str());
            } else {
                ImGui::NewLine();
            }
            ImGui::PopID();
        }
    }
    }

    if (openMenu) ImGui::OpenPopup("CellMenu");
    if (ImGui::BeginPopup("CellMenu")) {
        const auto session = context.sessions->Active();
        const unsigned pointerSize = session && session->Target().architecture == target::Architecture::X86 ? 4 : 8;
        uint64_t pointer = 0;
        const bool canFollow = ReadCached(menuAddress_, &pointer, pointerSize) ||
                               (session && session->ReadMemory(menuAddress_, &pointer, pointerSize, nullptr));
        if (ImGui::MenuItem("Follow pointer", nullptr, false, canFollow && pointer != 0))
            context.NavigateTo("memory-browser", pointer);
        if (ImGui::MenuItem("Edit value...", "Enter")) {
            editAddress_ = menuAddress_;
            openEdit_ = true;
        }
        if (ImGui::MenuItem("Add to the address list")) {
            PendingAddressEntry entry;
            entry.address = menuAddress_;
            entry.type = static_cast<Display>(display_) == Display::Float ? ScanDataType::Float
                       : static_cast<Display>(display_) == Display::Double ? ScanDataType::Double
                       : cell == 8 ? ScanDataType::Int64 : cell == 2 ? ScanDataType::Int16
                       : cell == 1 ? ScanDataType::Byte : ScanDataType::Int32;
            context.pendingAddresses.push_back(entry);
            context.status = "Added to the Memory address list";
        }
        if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(Hex(menuAddress_).c_str());
        if (ImGui::MenuItem("Copy 16 bytes")) {
            std::string text;
            for (uint64_t i = 0; i < 16; ++i) {
                uint8_t byte = 0;
                char buffer[4] = {};
                std::snprintf(buffer, sizeof(buffer), i ? " %02X" : "%02X", ReadCached(menuAddress_ + i, &byte, 1) ? byte : 0);
                text += buffer;
            }
            ImGui::SetClipboardText(text.c_str());
        }
        ImGui::Separator();
        AddressContextOptions options;
        options.valueSize = static_cast<int>(cell);
        DrawAddressContextActions(context, menuAddress_, options);
        ImGui::EndPopup();
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) HandleKeyboard(context);
    ImGui::EndChild();
}

void MemoryBrowserWorkspace::DrawInspector(UiContext& context) {
    ImGui::BeginChild("Inspector", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("Data inspector");
    ImGui::Separator();
    if (!hasSelection_) {
        HintText("Select a byte to decode it as every type.");
        ImGui::EndChild();
        return;
    }
    uint8_t data[64] = {};
    const auto session = context.sessions->Active();
    const bool ok = ReadCached(selected_, data, sizeof(data)) ||
                    (session && session->ReadMemory(selected_, data, 16, nullptr));
    if (!ok) {
        ImGui::TextDisabled("%s is not readable", Hex(selected_).c_str());
        ImGui::EndChild();
        return;
    }
    if (ImGui::BeginTable("InspectTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(64));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kInspect)); ++i) {
            const auto& type = kInspect[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", type.label);
            ImGui::TableSetColumnIndex(1);
            const std::string value = ValueScanner::Format(data, 8, type.type, false, type.isUnsigned);
            ImGui::PushID(i);
            MonoFont mono;
            if (ImGui::Selectable(value.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                editAddress_ = selected_;
                editType_ = i;
                openEdit_ = true;
            }
            ImGui::PopID();
        }
        const unsigned pointerSize = session && session->Target().architecture == target::Architecture::X86 ? 4 : 8;
        uint64_t pointer = 0;
        std::memcpy(&pointer, data, pointerSize);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Pointer");
        ImGui::TableSetColumnIndex(1);
        bool isStatic = false;
        const std::string target = AddressLabel(pointer, isStatic);
        {
            MonoFont mono;
            if (ImGui::Selectable(target.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && pointer)
                context.NavigateTo("memory-browser", pointer);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to follow");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Binary");
        ImGui::TableSetColumnIndex(1);
        char bits[10] = {};
        for (int bit = 0; bit < 8; ++bit) bits[bit] = (data[0] >> (7 - bit)) & 1 ? '1' : '0';
        MonoTextUnformatted(bits);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Text");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(ValueScanner::Format(data, 32, ScanDataType::String).c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("UTF-16");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(ValueScanner::Format(data, 32, ScanDataType::String, false, false, true).c_str());
        ImGui::EndTable();
    }
    HintText("Double-click a value to edit it. In Byte hex, type hex digits over the selection; arrows move it.");
    ImGui::EndChild();
}

void MemoryBrowserWorkspace::DrawEditPopup(UiContext& context) {
    if (openEdit_) {
        openEdit_ = false;
        uint8_t data[8] = {};
        const auto session = context.sessions->Active();
        const auto& type = kInspect[std::clamp(editType_, 0, static_cast<int>(IM_ARRAYSIZE(kInspect)) - 1)];
        if (ReadCached(editAddress_, data, 8) || (session && session->ReadMemory(editAddress_, data, 8, nullptr)))
            std::snprintf(editValue_, sizeof(editValue_), "%s",
                          ValueScanner::Format(data, 8, type.type, false, type.isUnsigned).c_str());
        ImGui::OpenPopup("Edit memory value");
    }
    ImGui::SetNextWindowSize(ImVec2(Px(420), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Edit memory value", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    MonoText("%s", Hex(editAddress_).c_str());
    ImGui::SetNextItemWidth(Px(140));
    const char* names[IM_ARRAYSIZE(kInspect)] = {};
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kInspect)); ++i) names[i] = kInspect[i].label;
    ImGui::Combo("Type", &editType_, names, IM_ARRAYSIZE(names));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##EditMemoryValue", editValue_, sizeof(editValue_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(!context.mutationAllowed);
    if (ImGui::Button("Write", ImVec2(Px(110), 0)) || (enter && context.mutationAllowed)) {
        const auto& type = kInspect[std::clamp(editType_, 0, static_cast<int>(IM_ARRAYSIZE(kInspect)) - 1)];
        std::vector<uint8_t> bytes;
        std::string error;
        if (!ValueScanner::Encode(editValue_, type.type, false, false, bytes, &error)) context.status = error;
        else if (WriteBytes(context, editAddress_, bytes)) ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!context.mutationAllowed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Allow writes first");
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(Px(110), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MemoryBrowserWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        HintText("Select a process to browse memory.");
        return;
    }
    if (session->Target().id != targetId_) {
        targetId_ = session->Target().id;
        hasSelection_ = false;
        regions_.clear();
        modules_.clear();
        lastLayout_ = {};
        if (context.settings) {
            const int configured = context.settings->Values().autoRefreshMs;
            liveRateIndex_ = configured <= 150 ? 0 : configured <= 375 ? 1 : configured <= 750 ? 2 : 3;
        }
        const auto regions = session->MemoryRegions();
        const auto first = std::find_if(regions.begin(), regions.end(),
                                        [](const auto& region) { return region.readable; });
        GoTo(first != regions.end() ? first->base : 0, false);
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastLayout_.time_since_epoch().count() == 0 || now - lastLayout_ > std::chrono::seconds(3)) {
        lastLayout_ = now;
        regions_ = session->MemoryRegions();
        std::sort(regions_.begin(), regions_.end(), [](const auto& a, const auto& b) { return a.base < b.base; });
        std::string error;
        if (context.modules) modules_ = context.modules->List(&error);
    }

    uint64_t navigation = 0;
    if (context.ConsumeNavigation("memory-browser", navigation)) GoTo(navigation);
    Refresh(context, bytes_.empty());

    DrawToolbar(context);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const bool side = available.x >= MeasureRow(context).width + Px(250);
    if (side) {
        const float inspectorWidth = std::clamp(available.x * 0.28f, Px(240), Px(340));
        DrawHex(context, available.x - inspectorWidth - ImGui::GetStyle().ItemSpacing.x, 0.0f);
        ImGui::SameLine();
        DrawInspector(context);
    } else {
        DrawHex(context, 0.0f, std::max(Px(160), available.y - Px(250)));
        DrawInspector(context);
    }
    DrawEditPopup(context);
}

} // namespace cortex::ui

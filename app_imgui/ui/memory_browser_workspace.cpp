#include "memory_browser_workspace.h"
#include "address_context_menu.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace cortex::ui {
namespace {

std::string HexBytes(const uint8_t* bytes, size_t count) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < count; ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

std::string Ascii(const uint8_t* bytes, size_t count) {
    std::string out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char c = bytes[i];
        out.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
    }
    return out;
}

} // namespace

bool MemoryBrowserWorkspace::ParseAddress(uint64_t& address) const {
    try {
        size_t used = 0;
        const std::string text(address_);
        address = std::stoull(text, &used, 0);
        return used == text.size() && address != 0;
    } catch (...) {
        return false;
    }
}

void MemoryBrowserWorkspace::Navigate(uint64_t address) {
    currentAddress_ = address;
    std::snprintf(address_, sizeof(address_), "0x%llX",
                  static_cast<unsigned long long>(address));
}

void MemoryBrowserWorkspace::Refresh(UiContext& context) {
    if (!context.memory) return;
    uint64_t address = 0;
    if (!ParseAddress(address)) {
        context.status = "Invalid memory address";
        return;
    }
    byteCount_ = std::clamp(byteCount_, 16, 4096);
    std::string error;
    std::vector<uint8_t> value;
    if (!context.memory->Read(address, static_cast<size_t>(byteCount_), value, &error)) {
        context.status = "Memory read failed: " + error;
        bytes_.clear();
        return;
    }
    currentAddress_ = address;
    bytes_ = std::move(value);
    context.status = "Read " + std::to_string(bytes_.size()) + " byte(s)";
}

bool MemoryBrowserWorkspace::WriteBytes(UiContext& context) {
    if (!context.memory || !context.mutationAllowed) {
        context.status = "Enable writes before editing memory";
        return false;
    }
    uint64_t address = 0;
    if (!ParseAddress(address)) {
        context.status = "Invalid memory address";
        return false;
    }

    std::vector<uint8_t> bytes;
    std::istringstream stream(writeBytes_);
    std::string token;
    try {
        while (stream >> token) {
            size_t used = 0;
            const unsigned long value = std::stoul(token, &used, 16);
            if (used != token.size() || value > 0xff) throw std::runtime_error("bad byte");
            bytes.push_back(static_cast<uint8_t>(value));
        }
    } catch (...) {
        context.status = "Bytes must look like: DE AD BE EF";
        return false;
    }
    if (bytes.empty()) {
        context.status = "Enter hexadecimal bytes to write";
        return false;
    }

    std::string error;
    if (!context.memory->Write(address, bytes, true, &error)) {
        context.status = "Memory write failed: " + error;
        return false;
    }
    context.status = "Wrote " + std::to_string(bytes.size()) + " byte(s)";
    Refresh(context);
    return true;
}

void MemoryBrowserWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        ImGui::TextDisabled("Select a process to browse memory.");
        return;
    }

    const std::string id = session->Target().id;
    if (id != targetId_) {
        targetId_ = id;
        bytes_.clear();
        currentAddress_ = 0;
        if (context.settings)
            byteCount_ = context.settings->Values().memoryReadSize;
        std::snprintf(address_, sizeof(address_), "0x%llX",
                      static_cast<unsigned long long>(session->MemoryRegions().empty()
                        ? 0ull : session->MemoryRegions().front().base));
    }

    uint64_t navigationAddress = 0;
    if (context.ConsumeNavigation("memory-browser", navigationAddress)) {
        Navigate(navigationAddress);
        Refresh(context);
    }

    ImGui::TextUnformatted("Memory viewer");
    ImGui::SameLine();
    ImGui::TextDisabled("Direct external-process view");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##MemoryAddress", "0x7FF...", address_, sizeof(address_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Bytes", &byteCount_, 16, 256);
    ImGui::SameLine();
    if (ImGui::Button("Read")) Refresh(context);
    ImGui::SameLine();
    if (ImGui::Button("Disassemble") && ParseAddress(currentAddress_)) {
        context.NavigateTo("disassembly", currentAddress_);
    }

    ImGui::Spacing();
    const size_t rowWidth = context.settings
        ? static_cast<size_t>(context.settings->Values().memoryBytesPerRow)
        : size_t{16};
    if (ImGui::BeginChild("HexView", ImVec2(0, -118), ImGuiChildFlags_Borders)) {
        if (bytes_.empty()) {
            ImGui::TextDisabled("Enter an address and press Read.");
        } else if (ImGui::BeginTable("HexTable", 3,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                     ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 145);
            ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthStretch, 0.72f);
            ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthStretch, 0.28f);
            ImGui::TableHeadersRow();

            for (size_t offset = 0; offset < bytes_.size(); offset += rowWidth) {
                const size_t count = std::min(rowWidth, bytes_.size() - offset);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("0x%llX",
                            static_cast<unsigned long long>(currentAddress_ + offset));
                if (ImGui::BeginPopupContextItem()) {
                    AddressContextOptions options;
                    options.valueType = "u8";
                    options.valueSize = 1;
                    DrawAddressContextActions(context, currentAddress_ + offset, options);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(HexBytes(bytes_.data() + offset, count).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(Ascii(bytes_.data() + offset, count).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::TextDisabled("Write bytes at the current address");
    ImGui::SetNextItemWidth(-110);
    ImGui::InputTextWithHint("##WriteBytes", "90 90 90", writeBytes_, sizeof(writeBytes_));
    ImGui::SameLine();
    ImGui::BeginDisabled(!context.mutationAllowed);
    if (ImGui::Button("Write", ImVec2(100, 0))) WriteBytes(context);
    ImGui::EndDisabled();
}

} // namespace cortex::ui

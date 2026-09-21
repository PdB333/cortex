#include "disassembly_workspace.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace cortex::ui {
namespace {

std::string FormatBytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

} // namespace

bool DisassemblyWorkspace::ParseAddress(uint64_t& address) const {
    try {
        size_t used = 0;
        const std::string text(address_);
        address = std::stoull(text, &used, 0);
        return used == text.size() && address != 0;
    } catch (...) {
        return false;
    }
}

void DisassemblyWorkspace::Decode(UiContext& context, uint64_t address) {
    if (!context.disassembly) return;
    count_ = std::clamp(count_, 16, 1000);
    std::string error;
    std::vector<services::DisassemblyInstruction> rows;
    if (!context.disassembly->Decode(address, static_cast<size_t>(count_), rows, &error)) {
        context.status = "Disassembly failed: " + error;
        instructions_.clear();
        return;
    }
    currentAddress_ = address;
    instructions_ = std::move(rows);
    std::snprintf(address_, sizeof(address_), "0x%llX",
                  static_cast<unsigned long long>(address));
    context.status = std::to_string(instructions_.size()) + " instruction(s)";
}

void DisassemblyWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        ImGui::TextDisabled("Select a process to disassemble memory.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        instructions_.clear();
        currentAddress_ = 0;
    }

    if (context.navigationAddressPending) {
        const uint64_t address = context.navigationAddress;
        context.navigationAddressPending = false;
        Decode(context, address);
    }

    ImGui::TextUnformatted("Disassembler");
    ImGui::SameLine();
    ImGui::TextDisabled(cortex::target::ArchitectureName(session->Target().architecture));
    ImGui::Spacing();

    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##DisasmAddress", "0x7FF...", address_, sizeof(address_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Count", &count_, 16, 64);
    ImGui::SameLine();
    if (ImGui::Button("Go")) {
        uint64_t address = 0;
        if (ParseAddress(address)) Decode(context, address);
        else context.status = "Invalid disassembly address";
    }
    ImGui::SameLine();
    if (ImGui::Button("Memory") && currentAddress_) {
        context.navigationAddress = currentAddress_;
        context.navigationAddressPending = true;
        context.requestWorkspace = "memory-browser";
    }

    ImGui::Spacing();
    if (instructions_.empty()) {
        ImGui::TextDisabled("Enter an address, or open one from Modules / Memory.");
        return;
    }

    if (ImGui::BeginTable("DisassemblyTable", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 145);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < instructions_.size(); ++i) {
            const auto& row = instructions_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char label[32] = {};
            std::snprintf(label, sizeof(label), "0x%llX",
                          static_cast<unsigned long long>(row.address));
            if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns)) {
                currentAddress_ = row.address;
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Browse memory here")) {
                    context.navigationAddress = row.address;
                    context.navigationAddressPending = true;
                    context.requestWorkspace = "memory-browser";
                }
                if (ImGui::MenuItem("Find what writes this")) {
                    context.runtimeToolPreset = "re_find_last_writer";
                    context.runtimeArgumentsPreset =
                        std::string("{\"address\":") + std::to_string(row.address) +
                        ",\"size\":1,\"timeout_ms\":5000,\"mutation_permission\":true}";
                    context.requestWorkspace = "runtime";
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(FormatBytes(row.bytes).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.text.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} // namespace cortex::ui

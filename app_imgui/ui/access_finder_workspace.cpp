#include "access_finder_workspace.h"
#include "widgets.h"
#include "address_context_menu.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace cortex::ui {
namespace {

std::string Hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

} // namespace

void AccessFinderWorkspace::Start(UiContext& context, const AccessFinderRequest& request) {
    Watch watch;
    watch.address = request.address;
    watch.size = request.size == 8 || request.size == 4 || request.size == 2 ? request.size : 1;
    watch.writesOnly = request.writesOnly;
    std::string error;
    if (!context.debuggerModel) {
        watch.error = "The debugger is not available";
    } else if (!context.mutationAllowed) {
        watch.error = "Allow writes: a hardware breakpoint changes the target's debug registers";
    } else if (!context.debuggerModel->AddBreakpoint(Hex(request.address),
                                                      request.writesOnly ? "hw_write" : "hw_readwrite",
                                                      watch.size, false, true, 0, &error)) {
        watch.error = "Breakpoint failed: " + error;
    } else {
        // The new breakpoint is the highest id on this address and kind.
        const std::string kind = request.writesOnly ? "hw_write" : "hw_readwrite";
        for (const auto& breakpoint : context.debuggerModel->Breakpoints())
            if (breakpoint.address == request.address && breakpoint.kind == kind)
                watch.breakpointId = std::max(watch.breakpointId, breakpoint.id);
        watch.active = watch.breakpointId >= 0;
        if (!watch.active) watch.error = "The breakpoint was not reported back by the debugger";
    }
    context.status = watch.error.empty()
        ? std::string("Watching what ") + (watch.writesOnly ? "writes to " : "accesses ") + Hex(watch.address)
        : watch.error;
    watches_.push_back(std::move(watch));
    selected_ = static_cast<int>(watches_.size()) - 1;
}

void AccessFinderWorkspace::Stop(UiContext& context, Watch& watch) {
    if (!watch.active) return;
    std::string error;
    if (context.debuggerModel && !context.debuggerModel->RemoveBreakpoint(watch.breakpointId, &error))
        context.status = "Remove breakpoint failed: " + error;
    watch.active = false;
}

// A data breakpoint traps after the access, so the reported address is the
// next instruction. Decode backwards for the instruction that ends there,
// preferring one with a memory operand.
uint64_t AccessFinderWorkspace::AccessingInstruction(UiContext& context, uint64_t trap, std::string& text) {
    const auto cached = previousInstruction_.find(trap);
    if (cached != previousInstruction_.end()) {
        text = cached->second.second;
        return cached->second.first;
    }
    uint64_t best = trap;
    std::string bestText;
    bool bestMemory = false;
    if (context.disassembly) {
        for (uint64_t length = 1; length <= 15 && length <= trap; ++length) {
            std::vector<services::DisassemblyInstruction> decoded;
            std::string error;
            if (!context.disassembly->Decode(trap - length, 1, decoded, &error) || decoded.empty()) continue;
            if (decoded.front().bytes.size() != length) continue;
            const bool memory = decoded.front().text.find('[') != std::string::npos;
            if (bestText.empty() || (memory && !bestMemory) || (memory == bestMemory && length > trap - best)) {
                best = trap - length;
                bestText = decoded.front().text;
                bestMemory = memory;
            }
        }
    }
    if (bestText.empty()) bestText = "(next instruction " + Hex(trap) + ")";
    previousInstruction_[trap] = {best, bestText};
    text = bestText;
    return best;
}

void AccessFinderWorkspace::Poll(UiContext& context) {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ < std::chrono::milliseconds(250)) return;
    lastPoll_ = now;
    if (!context.debuggerModel) return;
    for (auto& watch : watches_) {
        if (!watch.active) continue;
        std::vector<DebugBreakpointLogEntry> entries;
        std::string error;
        if (!context.debuggerModel->LoadBreakpointLog(watch.breakpointId, watch.lastSeq, 2000, entries, &error)) {
            watch.error = error;
            continue;
        }
        for (const auto& entry : entries) {
            watch.lastSeq = std::max(watch.lastSeq, entry.seq);
            std::string text;
            const uint64_t instruction = AccessingInstruction(context, entry.instruction, text);
            auto& hit = watch.hits[instruction];
            hit.instruction = instruction;
            hit.text = text;
            ++hit.count;
            hit.registers.clear();
            for (const auto& value : entry.registers.registers) hit.registers.emplace_back(value.name, value.value);
            ++watch.total;
        }
    }
}

void AccessFinderWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    const std::string targetId = session ? session->Target().id : std::string();
    if (targetId != targetId_) {
        for (auto& watch : watches_) watch.active = false;  // the old debugger session is gone
        watches_.clear();
        previousInstruction_.clear();
        selected_ = -1;
        targetId_ = targetId;
    }
    if (!session) {
        context.accessFinderRequests.clear();
        HintText("Select a process, then right-click an address > Debugger > Find out what writes to this address.");
        return;
    }
    for (const auto& request : context.accessFinderRequests) Start(context, request);
    context.accessFinderRequests.clear();
    Poll(context);

    if (watches_.empty()) {
        HintText("Right-click an address anywhere (scan results, address list, memory viewer) > Debugger > "
                 "Find out what writes to / accesses this address. Each hit is counted per instruction while "
                 "the game keeps running; x86 has four hardware breakpoint slots.");
        return;
    }

    const float listWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.30f, Px(220), Px(320));
    ImGui::BeginChild("Watches", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(watches_.size()); ++i) {
        auto& watch = watches_[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const std::string label = std::string(watch.writesOnly ? "Writes to " : "Accesses ") + Hex(watch.address);
        if (ImGui::Selectable(label.c_str(), selected_ == i)) selected_ = i;
        ImGui::TextDisabled("  %s, %llu hit(s), %zu instruction(s)", watch.active ? "running" : "stopped",
                            static_cast<unsigned long long>(watch.total), watch.hits.size());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("Hits", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (selected_ < 0 || selected_ >= static_cast<int>(watches_.size())) selected_ = 0;
    auto& watch = watches_[static_cast<size_t>(selected_)];
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s %s (%d byte%s)", watch.writesOnly ? "Instructions that write to" : "Instructions that access",
                Hex(watch.address).c_str(), watch.size, watch.size == 1 ? "" : "s");
    if (watch.active) {
        FlowSameLine(ButtonWidth("Stop"));
        if (ImGui::Button("Stop")) Stop(context, watch);
    } else {
        FlowSameLine(ButtonWidth("Remove"));
        if (ImGui::Button("Remove")) {
            watches_.erase(watches_.begin() + selected_);
            selected_ = -1;
            ImGui::EndChild();
            return;
        }
    }
    if (!watch.error.empty()) ImGui::TextColored(WarningTextColor(), "%s", watch.error.c_str());
    ImGui::Separator();

    std::vector<const Hit*> rows;
    for (const auto& item : watch.hits) rows.push_back(&item.second);
    std::sort(rows.begin(), rows.end(), [](const Hit* a, const Hit* b) { return a->count > b->count; });
    if (rows.empty()) {
        HintText(watch.active ? "Waiting for the game to touch the address..." : "No hit was recorded.");
        ImGui::EndChild();
        return;
    }
    if (BeginDataTable("HitsTable", 3,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_Resizable,
                       ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, Px(70));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); ++i) {
            const Hit& hit = *rows[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(std::to_string(hit.count).c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                context.NavigateTo("disassembly", hit.instruction);
            if (ImGui::IsItemHovered() && !hit.registers.empty()) {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("Registers at the last hit");
                for (const auto& reg : hit.registers) MonoText("%-6s %016llX", reg.first.c_str(), static_cast<unsigned long long>(reg.second));
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("HitMenu")) {
                if (ImGui::MenuItem("Disassemble")) context.NavigateTo("disassembly", hit.instruction);
                if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(Hex(hit.instruction).c_str());
                if (ImGui::MenuItem("Copy instruction")) ImGui::SetClipboardText(hit.text.c_str());
                ImGui::Separator();
                AddressContextOptions options;
                options.label = hit.text;
                DrawAddressContextActions(context, hit.instruction, options);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            MonoTextUnformatted(Hex(hit.instruction).c_str());
            ImGui::TableSetColumnIndex(2);
            MonoTextUnformatted(hit.text.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace cortex::ui

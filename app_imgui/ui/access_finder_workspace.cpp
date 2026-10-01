#include "access_finder_workspace.h"
#include "widgets.h"
#include "address_context_menu.h"

#include "services/instruction_operands.h"

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
    watch.instruction = request.instruction;
    std::string error;
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    watch.x64 = !session || session->Target().architecture != target::Architecture::X86;
    if (watch.instruction) {
        watch.size = 1;
        watch.code.assign(16, 0);
        size_t size = watch.code.size();
        while (size > 1 && !(session && session->ReadMemory(request.address, watch.code.data(), size, nullptr))) --size;
        watch.code.resize(size);
        services::MemoryAccess access;
        const auto anyRegister = [](const std::string&, uint64_t& value) {
            value = 0;
            return true;
        };
        if (!services::ResolveMemoryAccess(watch.code.data(), watch.code.size(), request.address, watch.x64,
                                           anyRegister, access, &error)) {
            watch.error = error;
            context.status = "What addresses: " + error;
            watches_.push_back(std::move(watch));
            selected_ = static_cast<int>(watches_.size()) - 1;
            return;
        }
        watch.instructionText = access.text;
    }
    const std::string kind = watch.instruction ? "hw_execute" : (request.writesOnly ? "hw_write" : "hw_readwrite");
    if (!context.debuggerModel) {
        watch.error = "The debugger is not available";
    } else if (!context.mutationAllowed) {
        watch.error = "Allow writes: a hardware breakpoint changes the target's debug registers";
    } else if (!context.debuggerModel->AddBreakpoint(Hex(request.address), kind, watch.size, false, true, 0,
                                                      &error)) {
        watch.error = "Breakpoint failed: " + error;
    } else {
        // The new breakpoint is the highest id on this address and kind.
        for (const auto& breakpoint : context.debuggerModel->Breakpoints())
            if (breakpoint.address == request.address && breakpoint.kind == kind)
                watch.breakpointId = std::max(watch.breakpointId, breakpoint.id);
        watch.active = watch.breakpointId >= 0;
        if (!watch.active) watch.error = "The breakpoint was not reported back by the debugger";
    }
    context.status = !watch.error.empty() ? watch.error
        : watch.instruction ? "Watching the addresses " + watch.instructionText + " accesses"
        : std::string("Watching what ") + (watch.writesOnly ? "writes to " : "accesses ") + Hex(watch.address);
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
    if (now - lastPoll_ < pollInterval_) return;
    lastPoll_ = now;
    if (!context.debuggerModel) return;
    for (auto& watch : watches_) {
        if (!watch.active) continue;
        std::vector<DebugBreakpointLogEntry> entries;
        std::string error;
        const auto started = std::chrono::steady_clock::now();
        // Entries newer than the last one seen: the last one itself used to
        // come back every time and was counted again.
        const uint64_t since = watch.lastSeq ? watch.lastSeq + 1 : 0;
        if (!context.debuggerModel->LoadBreakpointLog(watch.breakpointId, since, 500, entries, &error)) {
            watch.error = error;
            continue;
        }
        bool changed = false;
        for (const auto& entry : entries) {
            watch.lastSeq = std::max(watch.lastSeq, entry.seq);
            changed = true;
            ++watch.listed;
            if (watch.instruction) {
                const auto lookup = [&entry](const std::string& name, uint64_t& value) {
                    for (const auto& reg : entry.registers.registers) {
                        if (reg.name != name) continue;
                        value = reg.value;
                        return true;
                    }
                    return false;
                };
                services::MemoryAccess access;
                if (!services::ResolveMemoryAccess(watch.code.data(), watch.code.size(), watch.address, watch.x64,
                                                   lookup, access, nullptr))
                    continue;
                auto& hit = watch.hits[access.address];
                hit.instruction = access.address;
                hit.size = access.size;
                hit.write = access.write;
                ++hit.count;
                hit.registers.clear();
                for (const auto& value : entry.registers.registers) hit.registers.emplace_back(value.name, value.value);
                continue;
            }
            std::string text;
            const uint64_t instruction = AccessingInstruction(context, entry.instruction, text);
            auto& hit = watch.hits[instruction];
            hit.instruction = instruction;
            hit.text = text;
            ++hit.count;
            hit.registers.clear();
            for (const auto& value : entry.registers.registers) hit.registers.emplace_back(value.name, value.value);
        }
        // The sequence number is the hit count, so it also covers the hits
        // the debugger dropped because they came faster than they are read.
        watch.total = std::max(watch.total, watch.lastSeq);
        if (changed) ++watch.version;

        if (watch.rateAt.time_since_epoch().count() == 0) {
            watch.rateAt = now;
            watch.rateSeq = watch.total;
        } else if (now - watch.rateAt >= std::chrono::seconds(1)) {
            const double seconds = std::chrono::duration<double>(now - watch.rateAt).count();
            watch.rate = static_cast<double>(watch.total - watch.rateSeq) / seconds;
            watch.rateAt = now;
            watch.rateSeq = watch.total;
        }

        // Instruction watches show what each address holds now. Read here,
        // a few times a second, rather than once per row per frame.
        if (watch.instruction && now - watch.valuesAt >= std::chrono::milliseconds(500)) {
            watch.valuesAt = now;
            const auto session = context.sessions ? context.sessions->Active() : nullptr;
            size_t reads = 0;
            for (auto& item : watch.hits) {
                if (!session || ++reads > 256) break;
                Hit& hit = item.second;
                hit.value = 0;
                hit.valueRead = session->ReadMemory(hit.instruction, &hit.value, std::clamp(hit.size, 1u, 8u), nullptr);
            }
            ++watch.version;
        }

        // A hot address stops the whole game on every hit: stop before the
        // game is unplayable, not after.
        if (stopAfterHits_ > 0 && watch.total >= static_cast<uint64_t>(stopAfterHits_)) {
            Stop(context, watch);
            watch.note = "Stopped after " + std::to_string(watch.total) +
                         " hits so the game is not slowed further. Raise or clear \"Stop after\" to keep watching.";
        }

        // A slow poll means the list is busy: poll less often.
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        pollInterval_ = std::clamp(spent * 8, std::chrono::milliseconds(250), std::chrono::milliseconds(2000));
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
                 "Find out what writes to / accesses this address, or right-click an instruction in the "
                 "Disassembler > Debugger > Find out what addresses this instruction accesses. Hits are counted "
                 "while the game keeps running; x86 has four hardware breakpoint slots.");
        return;
    }

    const float listWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.30f, Px(220), Px(320));
    ImGui::BeginChild("Watches", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(watches_.size()); ++i) {
        auto& watch = watches_[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const std::string label = watch.instruction ? "Addresses of " + (watch.instructionText.empty() ? Hex(watch.address)
                                                                                                    : watch.instructionText)
                                : std::string(watch.writesOnly ? "Writes to " : "Accesses ") + Hex(watch.address);
        if (ImGui::Selectable(label.c_str(), selected_ == i)) selected_ = i;
        ImGui::TextDisabled("  %s, %llu hit(s), %zu %s", watch.active ? "running" : "stopped",
                            static_cast<unsigned long long>(watch.total), watch.hits.size(),
                            watch.instruction ? "address(es)" : "instruction(s)");
        if (watch.active && watch.rate >= 500.0)
            ImGui::TextColored(WarningTextColor(), "  %.0f hits/s: slowing the game", watch.rate);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("Hits", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (selected_ < 0 || selected_ >= static_cast<int>(watches_.size())) selected_ = 0;
    auto& watch = watches_[static_cast<size_t>(selected_)];
    ImGui::AlignTextToFramePadding();
    if (watch.instruction)
        ImGui::Text("Addresses accessed by %s  (%s)", watch.instructionText.c_str(), Hex(watch.address).c_str());
    else
        ImGui::Text("%s %s (%d byte%s)", watch.writesOnly ? "Instructions that write to" : "Instructions that access",
                    Hex(watch.address).c_str(), watch.size, watch.size == 1 ? "" : "s");
    if (watch.active) {
        FlowSameLine(ButtonWidth("Stop"));
        if (ImGui::Button("Stop")) Stop(context, watch);
        FlowSameLine(Px(210));
        ImGui::SetNextItemWidth(Px(90));
        if (ImGui::InputInt("Stop after", &stopAfterHits_, 0, 0)) stopAfterHits_ = std::max(stopAfterHits_, 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every hit stops the whole game while Cortex records it. The watch stops itself\n"
                              "after this many hits so a busy address does not freeze the game. 0 = never.");
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
    if (!watch.note.empty()) ImGui::TextColored(WarningTextColor(), "%s", watch.note.c_str());
    if (watch.total > watch.listed)
        ImGui::TextDisabled("%llu hit(s) came faster than they can be listed and were counted only.",
                            static_cast<unsigned long long>(watch.total - watch.listed));
    ImGui::Separator();

    if (watch.orderVersion != watch.version) {
        watch.order.clear();
        for (const auto& item : watch.hits) watch.order.push_back(item.first);
        std::sort(watch.order.begin(), watch.order.end(), [&watch](uint64_t a, uint64_t b) {
            return watch.hits[a].count > watch.hits[b].count;
        });
        watch.orderVersion = watch.version;
    }
    const auto& rows = watch.order;
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
        ImGui::TableSetupColumn(watch.instruction ? "Value" : "Instruction", ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const size_t i = static_cast<size_t>(row);
            const Hit& hit = watch.hits[rows[i]];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(std::to_string(hit.count).c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                context.NavigateTo(watch.instruction ? "memory-browser" : "disassembly", hit.instruction);
            if (ImGui::IsItemHovered() && !hit.registers.empty()) {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("Registers at the last hit");
                for (const auto& reg : hit.registers) MonoText("%-6s %016llX", reg.first.c_str(), static_cast<unsigned long long>(reg.second));
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("HitMenu")) {
                if (watch.instruction && ImGui::MenuItem("Add to the address list")) {
                    PendingAddressEntry entry;
                    entry.address = hit.instruction;
                    entry.description = "Accessed by " + watch.instructionText;
                    entry.type = hit.size == 8 ? services::ScanDataType::Int64 : hit.size == 2 ? services::ScanDataType::Int16
                               : hit.size == 1 ? services::ScanDataType::Byte : services::ScanDataType::Int32;
                    context.pendingAddresses.push_back(entry);
                    context.status = "Added to the Memory address list";
                }
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
            if (watch.instruction) {
                const unsigned size = std::clamp(hit.size, 1u, 8u);
                if (hit.valueRead)
                    MonoText("%llu  (%0*llX)%s", static_cast<unsigned long long>(hit.value), static_cast<int>(size * 2),
                             static_cast<unsigned long long>(hit.value), hit.write ? "  written" : "");
                else
                    ImGui::TextDisabled("??");
            } else {
                MonoTextUnformatted(hit.text.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace cortex::ui

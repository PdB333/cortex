#pragma once

#include "ui_context.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace cortex::ui {

struct AddressContextOptions {
    std::string label;
    std::string valueType = "i32";
    int valueSize = 1;
    bool allowSave = true;
};

inline std::string FormatContextAddress(uint64_t address) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX",
                  static_cast<unsigned long long>(address));
    return buffer;
}

inline std::string AddressContextLabel(
        uint64_t address, const AddressContextOptions& options) {
    return options.label.empty()
        ? "Address " + FormatContextAddress(address)
        : options.label;
}

inline std::string RuntimeSupportText(const std::string& reason) {
    if (reason == "payload_cross_bitness_helper_missing")
        return "Unavailable: x86 helper missing. Use the portable bundle or restore runtime/x86/cortex_runtime_helper.exe.";
    if (reason == "payload_binary_missing")
        return "Unavailable: target runtime DLL is missing from the portable bundle.";
    if (reason == "payload_runtime_directory_missing")
        return "Unavailable: Cortex runtime directory is not configured.";
    if (reason == "payload_cross_bitness_helper_required")
        return "Unavailable: this target architecture needs a cross-bitness bootstrap helper.";
    if (reason == "payload_injection_not_supported_on_target")
        return "Unavailable: runtime injection is not supported for this target.";
    if (reason == "no_active_session")
        return "Unavailable: select a process first.";
    return reason.empty() ? "Runtime unavailable." : "Runtime unavailable: " + reason;
}

inline bool AddressRuntimeAvailable(
        UiContext& context, std::string& reason) {
    reason.clear();
    if (!context.payload) {
        reason = "runtime_client_unavailable";
        return false;
    }
    if (context.payload->Ready()) return true;
    return context.payload->RuntimeSupportAvailable(&reason);
}

inline void RuntimeDisabledHint(bool disabled, const std::string& reason) {
    if (disabled &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", RuntimeSupportText(reason).c_str());
    }
}

inline void DrawAddressContextActions(
        UiContext& context, uint64_t address,
        const AddressContextOptions& options = {}) {
    const std::string addressText = FormatContextAddress(address);
    const std::string label = AddressContextLabel(address, options);
    const int valueSize = std::max(1, options.valueSize);

    std::string runtimeReason;
    const bool runtimeAvailable = AddressRuntimeAvailable(context, runtimeReason);
    const bool runtimeConnected =
        context.payload && context.payload->Ready();
    const bool runtimeMutation =
        runtimeAvailable && context.mutationAllowed;

    if (ImGui::BeginMenu("Open / follow")) {
        if (ImGui::MenuItem("Browse memory"))
            context.NavigateTo("memory-browser", address);
        if (ImGui::MenuItem("Disassemble"))
            context.NavigateTo("disassembly", address);
        if (ImGui::MenuItem("Open in RE"))
            context.NavigateTo("re", address);
        if (ImGui::MenuItem("Pointer Maps"))
            context.NavigateTo("pointermaps", address);
        if (ImGui::MenuItem("Structures"))
            context.NavigateTo("structures", address);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Monitor")) {
        const bool watchDisabled =
            !context.watchesModel || !runtimeMutation;
        ImGui::BeginDisabled(watchDisabled);
        if (ImGui::MenuItem("Add live watch")) {
            std::string error;
            if (!context.watchesModel->AddWatch(
                    addressText, options.valueType, label,
                    context.mutationAllowed, &error)) {
                context.status = "Add watch failed: " + error;
            } else {
                context.status = "Live watch added";
                context.requestWorkspace = "watches";
            }
        }
        ImGui::EndDisabled();
        RuntimeDisabledHint(watchDisabled && !runtimeAvailable, runtimeReason);

        const bool pageDisabled =
            !context.instrumentationModel || !runtimeMutation;
        ImGui::BeginDisabled(pageDisabled);
        if (ImGui::MenuItem("Find what accesses (page watch)")) {
            std::string error;
            if (!context.instrumentationModel->AddPageAccessWatch(
                    addressText, valueSize, label,
                    context.mutationAllowed, &error)) {
                context.status = "Page watch failed: " + error;
            } else {
                context.status = "Page-access watch added";
                context.requestWorkspace = "instrumentation";
            }
        }
        ImGui::EndDisabled();
        RuntimeDisabledHint(pageDisabled && !runtimeAvailable, runtimeReason);

        const bool snapshotDisabled =
            !context.snapshotsModel || !runtimeConnected;
        ImGui::BeginDisabled(snapshotDisabled);
        if (ImGui::MenuItem("Snapshot 64 bytes")) {
            const std::string ranges =
                "[{\"address\":\"" + addressText + "\",\"size\":64}]";
            std::string error;
            if (!context.snapshotsModel->Create(ranges, label, &error)) {
                context.status = "Snapshot failed: " + error;
            } else {
                context.status = "Snapshot created";
                context.requestWorkspace = "snapshots";
            }
        }
        ImGui::EndDisabled();
        if (snapshotDisabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", runtimeConnected
                ? "Snapshot unavailable."
                : "Connect or enable the target runtime before taking snapshots.");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debugger")) {
        const bool debuggerDisabled =
            !context.debuggerModel || !context.mutationAllowed;

        auto addBreakpoint = [&](const char* title,
                                 const char* kind,
                                 int size) {
            ImGui::BeginDisabled(debuggerDisabled);
            if (ImGui::MenuItem(title)) {
                std::string error;
                const bool pauseOnHit =
                    context.settings &&
                    context.settings->Values().breakpointDefaultAction == "pause";
                if (!context.debuggerModel->AddBreakpoint(
                        addressText, kind, size, pauseOnHit, true, 0, &error)) {
                    context.status = "Add breakpoint failed: " + error;
                } else {
                    context.status = "Breakpoint added";
                    context.requestWorkspace = "debugger";
                }
            }
            ImGui::EndDisabled();
        };

        addBreakpoint("Software breakpoint", "software", 1);
        addBreakpoint("HW execute breakpoint", "hw_execute", 1);
        addBreakpoint("HW write breakpoint", "hw_write",
                      std::clamp(valueSize, 1, 8));
        addBreakpoint("HW read/write breakpoint", "hw_readwrite",
                      std::clamp(valueSize, 1, 8));
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Reverse engineering")) {
        const bool reMutationDisabled =
            !context.reModel || !runtimeMutation;

        ImGui::BeginDisabled(reMutationDisabled);
        if (ImGui::MenuItem("Find what writes")) {
            std::string error;
            if (!context.reModel->FindLastWriter(
                    addressText, valueSize, 10000,
                    context.mutationAllowed, &error)) {
                context.status = "Find last writer failed: " + error;
            } else {
                context.status = "Last-writer analysis complete";
                context.NavigateTo("re", address);
            }
        }
        ImGui::EndDisabled();
        RuntimeDisabledHint(reMutationDisabled && !runtimeAvailable, runtimeReason);

        ImGui::BeginDisabled(reMutationDisabled);
        if (ImGui::MenuItem("Track object")) {
            std::string error;
            if (!context.reModel->TrackObject(
                    label, addressText, "", 256, true, "",
                    context.mutationAllowed, &error)) {
                context.status = "Track object failed: " + error;
            } else {
                context.status = "Object tracked";
                context.NavigateTo("re", address);
            }
        }
        ImGui::EndDisabled();
        RuntimeDisabledHint(reMutationDisabled && !runtimeAvailable, runtimeReason);

        const bool subobjectDisabled =
            !context.reModel || !runtimeConnected;
        ImGui::BeginDisabled(subobjectDisabled);
        if (ImGui::MenuItem("Detect C++ subobjects")) {
            std::string error;
            if (!context.reModel->DetectSubobjects(addressText, 256, &error)) {
                context.status = "Subobject analysis failed: " + error;
            } else {
                context.status = "Subobject analysis complete";
                context.NavigateTo("re", address);
            }
        }
        ImGui::EndDisabled();
        if (subobjectDisabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", runtimeConnected
                ? "Subobject analysis unavailable."
                : "Connect or enable the target runtime before subobject analysis.");
        ImGui::EndMenu();
    }

    if (options.allowSave) {
        const bool saveDisabled =
            !context.projectModel || !runtimeMutation;
        ImGui::BeginDisabled(saveDisabled);
        if (ImGui::MenuItem("Add to Addresses")) {
            std::string error;
            if (!context.projectModel->SetAddress(
                    label, addressText, options.valueType,
                    "Added from address context menu",
                    context.mutationAllowed, &error)) {
                context.status = "Save address failed: " + error;
            } else {
                context.status = "Address saved";
                context.requestWorkspace = "addresses";
            }
        }
        ImGui::EndDisabled();
        RuntimeDisabledHint(saveDisabled && !runtimeAvailable, runtimeReason);
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Copy")) {
        if (ImGui::MenuItem("Address"))
            ImGui::SetClipboardText(addressText.c_str());

        const bool symbolDisabled =
            !context.symbolsModel || !runtimeConnected;
        ImGui::BeginDisabled(symbolDisabled);
        if (ImGui::MenuItem("Module + offset")) {
            std::string error;
            if (context.symbolsModel->Resolve(addressText, &error)) {
                const auto& result = context.symbolsModel->Result();
                if (!result.module.empty() && !result.rva.empty()) {
                    const std::string text = result.module + "+" + result.rva;
                    ImGui::SetClipboardText(text.c_str());
                } else {
                    ImGui::SetClipboardText(addressText.c_str());
                }
            } else {
                ImGui::SetClipboardText(addressText.c_str());
                context.status = "Symbol resolve failed: " + error;
            }
        }
        ImGui::EndDisabled();
        if (symbolDisabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", runtimeConnected
                ? "Symbol resolution unavailable."
                : "Connect or enable the target runtime before symbol resolution.");
        ImGui::EndMenu();
    }

    if (!runtimeAvailable) {
        ImGui::Separator();
        ImGui::TextDisabled("Runtime: %s", RuntimeSupportText(runtimeReason).c_str());
    }
}

} // namespace cortex::ui

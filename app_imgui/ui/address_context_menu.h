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

inline void DrawAddressContextActions(
        UiContext& context, uint64_t address,
        const AddressContextOptions& options = {}) {
    const std::string addressText = FormatContextAddress(address);
    const std::string label = AddressContextLabel(address, options);
    const int valueSize = std::max(1, options.valueSize);

    if (ImGui::MenuItem("Browse memory"))
        context.NavigateTo("memory-browser", address);
    if (ImGui::MenuItem("Disassemble"))
        context.NavigateTo("disassembly", address);
    if (ImGui::MenuItem("Open in RE"))
        context.NavigateTo("re", address);

    ImGui::Separator();

    if (options.allowSave) {
        ImGui::BeginDisabled(!context.projectModel || !context.mutationAllowed);
        if (ImGui::MenuItem("Add to Addresses")) {
            std::string error;
            if (!context.projectModel->SetAddress(
                    label, addressText, options.valueType,
                    "Added from address context menu",
                    context.mutationAllowed, &error)) {
                context.status = "Save address failed: " + error;
            } else {
                context.status = "Address saved";
                context.requestWorkspace = "memory";
            }
        }
        ImGui::EndDisabled();
    }

    ImGui::BeginDisabled(!context.debuggerModel || !context.mutationAllowed);
    if (ImGui::MenuItem("Add software breakpoint")) {
        std::string error;
        const bool pauseOnHit =
            context.settings &&
            context.settings->Values().breakpointDefaultAction == "pause";
        if (!context.debuggerModel->AddBreakpoint(
                addressText, "software", 1, pauseOnHit, true, 0, &error))
            context.status = "Add breakpoint failed: " + error;
        else
            context.status = "Breakpoint added";
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!context.reModel || !context.mutationAllowed);
    if (ImGui::MenuItem("Find what writes")) {
        std::string error;
        if (!context.reModel->FindLastWriter(
                addressText, valueSize, 10000,
                context.mutationAllowed, &error))
            context.status = "Find last writer failed: " + error;
        else {
            context.status = "Last-writer analysis complete";
            context.NavigateTo("re", address);
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!context.instrumentationModel || !context.mutationAllowed);
    if (ImGui::MenuItem("Find what accesses (page watch)")) {
        std::string error;
        if (!context.instrumentationModel->AddPageAccessWatch(
                addressText, valueSize, label,
                context.mutationAllowed, &error))
            context.status = "Page watch failed: " + error;
        else {
            context.status = "Page-access watch added";
            context.requestWorkspace = "instrumentation";
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (ImGui::MenuItem("Pointer scan"))
        context.NavigateTo("pointermaps", address);
    if (ImGui::MenuItem("Open in Structures"))
        context.NavigateTo("structures", address);

    ImGui::BeginDisabled(!context.reModel || !context.mutationAllowed);
    if (ImGui::MenuItem("Track object in RE")) {
        std::string error;
        if (!context.reModel->TrackObject(
                label, addressText, "", 256, true, "",
                context.mutationAllowed, &error))
            context.status = "Track object failed: " + error;
        else {
            context.status = "Object tracked";
            context.NavigateTo("re", address);
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!context.reModel);
    if (ImGui::MenuItem("Detect C++ subobjects")) {
        std::string error;
        if (!context.reModel->DetectSubobjects(addressText, 256, &error))
            context.status = "Subobject analysis failed: " + error;
        else {
            context.status = "Subobject analysis complete";
            context.NavigateTo("re", address);
        }
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!context.snapshotsModel);
    if (ImGui::MenuItem("Snapshot 64 bytes")) {
        const std::string ranges =
            "[{\"address\":\"" + addressText + "\",\"size\":64}]";
        std::string error;
        if (!context.snapshotsModel->Create(ranges, label, &error))
            context.status = "Snapshot failed: " + error;
        else {
            context.status = "Snapshot created";
            context.requestWorkspace = "snapshots";
        }
    }
    ImGui::EndDisabled();

    if (ImGui::MenuItem("Copy address"))
        ImGui::SetClipboardText(addressText.c_str());

    ImGui::BeginDisabled(!context.symbolsModel);
    if (ImGui::MenuItem("Copy module+offset")) {
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
}

} // namespace cortex::ui

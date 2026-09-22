#include "symbols_workspace.h"

#include <imgui.h>

namespace cortex::ui {

bool SymbolsWorkspace::Navigate(UiContext& context, const std::string& address,
                                const char* workspace) {
    try {
        size_t used = 0;
        const uint64_t value = std::stoull(address, &used, 0);
        if (used != address.size()) {
            context.status = "Symbol address is not numeric: " + address;
            return false;
        }
        context.navigationAddress = value;
        context.navigationAddressPending = true;
        context.requestWorkspace = workspace;
        return true;
    } catch (...) {
        context.status = "Symbol address is not numeric: " + address;
        return false;
    }
}

void SymbolsWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.symbolsModel) {
        ImGui::TextDisabled("Select a process to resolve symbols.");
        return;
    }

    ImGui::TextUnformatted("Symbols");
    ImGui::SameLine();
    ImGui::TextDisabled(context.payload && context.payload->Ready()
                            ? "runtime connected" : "runtime disconnected");
    ImGui::SameLine();
    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing runtime")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.status = "Runtime connected";
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.status = "Runtime enabled";
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    const char* modes[] = {"Resolve address", "Lookup name"};
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##SymbolMode", &mode_, modes, IM_ARRAYSIZE(modes));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-190);
    ImGui::InputTextWithHint("##SymbolQuery",
                             mode_ == 0 ? "0x... / address" : "symbol name",
                             query_.data(), query_.size());
    ImGui::SameLine();
    if (ImGui::Button(mode_ == 0 ? "Resolve" : "Lookup", ImVec2(100, 0))) {
        std::string error;
        const bool ok = mode_ == 0
            ? context.symbolsModel->Resolve(query_.data(), &error)
            : context.symbolsModel->Lookup(query_.data(), &error);
        context.status = ok ? "Symbol query complete" : "Symbol query failed: " + error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) context.symbolsModel->Reset();

    ImGui::Spacing();
    const auto& result = context.symbolsModel->Result();
    if (!result.valid) {
        ImGui::TextDisabled("Resolve an address or look up a symbol name.");
        return;
    }

    ImGui::Text("Query: %s", result.query.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(result.found ? "FOUND" : "not found");
    if (!result.error.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("error: %s", result.error.c_str());
    }

    if (!result.address.empty()) {
        if (ImGui::Button("Memory")) Navigate(context, result.address, "memory-browser");
        ImGui::SameLine();
        if (ImGui::Button("Disassembly")) Navigate(context, result.address, "disassembly");
    }

    ImGui::Separator();
    if (ImGui::BeginTable("SymbolDetails", 2,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto field = [](const char* name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
        };
        auto number = [](const char* name, uint64_t value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(value));
        };

        field("Address", result.address);
        field("Module", result.module);
        field("Module path", result.modulePath);
        field("Module base", result.moduleBase);
        field("RVA", result.rva);
        field("Symbol", result.symbol);
        field("Symbol address", result.symbolAddress);
        number("Displacement", result.displacement);
        field("Source file", result.file);
        number("Source line", static_cast<uint64_t>(result.line));
        field("Build ID", result.buildId);
        field("Loaded PDB", result.loadedPdb);
        field("Symbol type", result.symbolType);
        field("Verification", result.verification);
        field("Exact symbols", result.exactSymbols);
        ImGui::EndTable();
    }
}

} // namespace cortex::ui

#include "ai_investigation_workspace.h"
#include "widgets.h"
#include "../../core/remote/client.h"
#include <imgui.h>
#include <cstdio>

namespace cortex::ui {
void AiInvestigationWorkspace::Ps5Run(UiContext& context, const std::string& requested) {
    using json = nlohmann::json;
    const std::string action = requested == "maps_next" ? "maps" : requested;
    json args = json::object();
    if (action == "maps" || action == "read" || action == "scan" || action == "write") {
        args["pid"] = ps5Pid_; args["target_id"] = ps5Target_.data();
    }
    if (action == "read" || action == "scan" || action == "write") args["address"] = ps5Address_.data();
    if (action == "read" || action == "scan") args["length"] = ps5Length_;
    if (action == "scan") { args["pattern"] = ps5Pattern_.data(); args["max_matches"] = ps5MaxMatches_; }
    if (action == "maps") {
        args["limit"] = 64;
        if (requested == "maps_next") { args["offset"] = ps5NextMapOffset_; args["expected_revision"] = ps5MapRevision_; }
    }
    if (action == "write") {
        args["hex"] = ps5Hex_.data(); args["expected_hex"] = ps5Expected_.data();
        args["expected_name"] = ps5Name_.data(); args["mutation_permission"] = true;
    }
    const std::string config = ps5Config_.data(); const bool writes = ps5AllowWrites_;
    context.RunInBackground("Cortex Remote: " + action, [this, &context, config, action, args, writes] {
        ps5Result_ = cortex::remote::RemoteRequest(config, action, args, writes);
        const bool ok = ps5Result_.value("ok", false);
        if (action == "processes" && ok) {
            ps5Processes_ = ps5Result_["processes"];
            bool stillPresent = false;
            for (const auto& row : ps5Processes_) stillPresent |= row["target_id"] == ps5Target_.data();
            if (!stillPresent) {
                ps5Target_.fill(0); ps5Name_.fill(0); ps5Pid_ = 0;
                ps5Expected_.fill(0); ps5Hex_.fill(0); ps5AllowWrites_ = false;
                ps5MapRevision_.clear(); ps5NextMapOffset_ = -1;
            }
        }
        if (action == "maps") {
            ps5NextMapOffset_ = -1; ps5MapRevision_.clear();
            if (ok && ps5Result_.contains("map_revision")) {
                ps5MapRevision_ = ps5Result_["map_revision"].get<std::string>();
                if (!ps5Result_["next_offset"].is_null()) ps5NextMapOffset_ = ps5Result_["next_offset"].get<int>();
            }
        }
        if (!ok && ps5Result_.value("error", std::string()) == "stale_target") {
            ps5Target_.fill(0); ps5Name_.fill(0); ps5AllowWrites_ = false;
            ps5MapRevision_.clear(); ps5NextMapOffset_ = -1;
        }
        context.status = ok ? "Cortex Remote: " + action + " completed" : "Cortex Remote: " + ps5Result_.value("error", std::string("failed"));
    });
}
void AiInvestigationWorkspace::DrawPs5(UiContext& context) {
    HintText("Our Cortex agent, mutual TLS and optional certificate pins. Native PS5 execution needs an authorized environment. "
             "Reads and searches may briefly stop the target if its operator enabled ptrace; no hardware compatibility is implied.");
    if (ImGui::InputTextWithHint("Client TLS profile", "Path to client.json", ps5Config_.data(), ps5Config_.size())) {
        ps5Target_.fill(0); ps5Name_.fill(0); ps5Processes_ = nullptr; ps5Result_ = nullptr; ps5AllowWrites_ = false;
        ps5MapRevision_.clear(); ps5NextMapOffset_ = -1; ps5Pid_ = 0; ps5Expected_.fill(0); ps5Hex_.fill(0);
    }
    if (ImGui::Button("Probe Cortex agent")) { Ps5Run(context, "info"); return; }
    ImGui::SameLine(); if (ImGui::Button("List remote processes")) { Ps5Run(context, "processes"); return; }
    if (ps5Processes_.is_array()) {
        if (ImGui::BeginChild("Remote processes", ImVec2(0, Px(135)), ImGuiChildFlags_Borders)) {
            for (const auto& row : ps5Processes_) {
                // RemoteRequest has already checked types, identities and bounds.
                const int pid = row["pid"].get<int>();
                const auto name = row["name"].get<std::string>(), token = row["target_id"].get<std::string>();
                ImGui::PushID(pid);
                if (ImGui::Selectable("Select##process", ps5Pid_ == pid && token == ps5Target_.data())) {
                    ps5Pid_ = pid;
                    std::snprintf(ps5Name_.data(), ps5Name_.size(), "%s", name.c_str());
                    std::snprintf(ps5Target_.data(), ps5Target_.size(), "%s", token.c_str());
                    ps5Expected_.fill(0); ps5Hex_.fill(0); ps5Result_ = nullptr; ps5AllowWrites_ = false;
                    ps5MapRevision_.clear(); ps5NextMapOffset_ = -1;
                }
                ImGui::SameLine(); ImGui::Text("%d", pid); ImGui::SameLine(); ImGui::TextUnformatted(name.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::Text("Selected remote PID: %d", ps5Pid_);
    ImGui::TextWrapped("Target generation: %s", ps5Target_[0] ? ps5Target_.data() : "select a process from the list");
    if (ImGui::Button("Read first map page")) { Ps5Run(context, "maps"); return; }
    if (ps5NextMapOffset_ >= 0) {
        ImGui::SameLine();
        if (ImGui::Button("Next map page")) { Ps5Run(context, "maps_next"); return; }
        ImGui::TextDisabled("Next offset: %d. A layout change invalidates pagination.", ps5NextMapOffset_);
    }
    ImGui::InputText("Remote address", ps5Address_.data(), ps5Address_.size());
    ImGui::InputInt("Bytes (1-65536)", &ps5Length_);
    if (ImGui::Button("Read remote memory")) { Ps5Run(context, "read"); return; }
    ImGui::InputTextWithHint("Byte pattern", "48 8b ?? 00", ps5Pattern_.data(), ps5Pattern_.size());
    ImGui::InputInt("Maximum matches (1-256)", &ps5MaxMatches_);
    if (ImGui::Button("Search this memory range")) { Ps5Run(context, "scan"); return; }
    HintText("Search reads the selected range once (at most 64 KiB). ?? matches one byte. It does not scan the whole process.");
    ImGui::Separator(); ImGui::Checkbox("Allow writes for this panel session", &ps5AllowWrites_);
    bool confirm = false;
    if (ps5AllowWrites_) {
        HintText("The client profile AND agent must also allow writes. No automatic retry or rollback. "
                 "An uncertain result may mean memory changed. This panel accepts up to 256 replacement bytes.");
        ImGui::InputText("Expected process name", ps5Name_.data(), ps5Name_.size());
        ImGui::InputText("Expected bytes (hex)", ps5Expected_.data(), ps5Expected_.size());
        ImGui::InputText("New bytes (hex)", ps5Hex_.data(), ps5Hex_.size());
        if (ImGui::Button("Review remote write...")) ImGui::OpenPopup("Confirm Cortex Remote write");
        if (ImGui::BeginPopupModal("Confirm Cortex Remote write", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Profile: %s", ps5Config_.data()); ImGui::Text("PID: %d  Name: %s", ps5Pid_, ps5Name_.data());
            ImGui::Text("Address: %s", ps5Address_.data()); ImGui::TextWrapped("Target: %s", ps5Target_.data());
            ImGui::TextWrapped("The agent will verify identity and expected bytes, write and read back. This is not a global transaction.");
            if (ImGui::Button("Confirm write")) { ImGui::CloseCurrentPopup(); confirm = true; }
            ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
    }
    if (confirm) { Ps5Run(context, "write"); return; }
    if (!ps5Result_.is_null()) {
        ImGui::Separator();
        if (ps5Result_.value("ok", false) && ps5Result_.contains("matches")) {
            ImGui::Text("Matches: %llu (showing %zu)", ps5Result_["total_matches"].get<unsigned long long>(), ps5Result_["matches"].size());
            if (ImGui::BeginChild("Remote matches", ImVec2(0, Px(95)), ImGuiChildFlags_Borders)) {
                for (const auto& row : ps5Result_["matches"]) {
                    const auto address = row["address"].get<std::string>();
                    if (ImGui::Selectable(address.c_str())) std::snprintf(ps5Address_.data(), ps5Address_.size(), "%s", address.c_str());
                }
            }
            ImGui::EndChild();
        }
        const auto text = ps5Result_.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        if (ImGui::Button("Copy remote result")) ImGui::SetClipboardText(text.c_str());
        ImGui::BeginChild("Cortex Remote result", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(text.c_str()); ImGui::EndChild();
    }
}
} // namespace cortex::ui

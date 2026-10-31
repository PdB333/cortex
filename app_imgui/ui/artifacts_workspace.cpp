#include "artifacts_workspace.h"

#include "file_dialog.h"
#include "widgets.h"

#include "../../mcp_bridge/artifact_tools.h"
#include "../../core/artifacts/js_provider.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cortex::ui {
namespace {

using json = nlohmann::json;

void Copy(char* destination, size_t size, const std::string& text) {
    std::snprintf(destination, size, "%s", text.c_str());
}

std::string Scalar(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

// A bounded tree view of provider output: large lists show their first rows.
void JsonTree(const json& value, const char* label, int depth) {
    constexpr size_t kRows = 200;
    if (!value.is_object() && !value.is_array()) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Scalar(value).c_str());
        return;
    }
    const std::string title = std::string(label) + (value.is_array() ? "  [" + std::to_string(value.size()) + "]" : "");
    const ImGuiTreeNodeFlags flags = depth == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    if (!ImGui::TreeNodeEx(title.c_str(), flags)) return;
    size_t shown = 0;
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end() && shown < kRows; ++it, ++shown) {
            ImGui::PushID(it.key().c_str());
            JsonTree(it.value(), it.key().c_str(), depth + 1);
            ImGui::PopID();
        }
    } else {
        for (size_t i = 0; i < value.size() && shown < kRows; ++i, ++shown) {
            ImGui::PushID(static_cast<int>(i));
            const json& row = value[i];
            std::string rowLabel = "#" + std::to_string(i);
            if (row.is_object())
                for (const char* key : {"name", "type", "rva"})
                    if (row.contains(key) && row[key].is_string()) { rowLabel += "  " + row[key].get<std::string>(); break; }
            JsonTree(row, rowLabel.c_str(), depth + 1);
            ImGui::PopID();
        }
    }
    if (value.size() > kRows) ImGui::TextDisabled("... %zu more (use cortex.exe inspect for the full JSON)", value.size() - kRows);
    ImGui::TreePop();
}

} // namespace

void ArtifactsWorkspace::Inspect(UiContext& context, const std::string& path, const std::string& child) {
    if (path.empty()) {
        context.status = "Enter a file path";
        return;
    }
    context.RunInBackground("Inspecting " + path, [this, &context, path, child]() {
        std::string error;
        json result, children;
        auto source = mcp_bridge::artifacts::OpenChain(path, child, error);
        if (!source) {
            result = {{"ok", false}, {"error", error}};
        } else {
            const auto& registry = cortex::artifacts::DefaultRegistry();
            result = registry.Inspect(*source);
            const auto provider = registry.Detect(*source);
            if (provider && !provider->Children(*source, {}, error).empty()) children = registry.ListChildren(*source);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
        position_ = nullptr;
        children_ = std::move(children);
        inspectedPath_ = path;
        inspectedChild_ = child;
        context.status = result_.value("ok", false)
            ? "Inspected " + source->Name() + " (" + result_.value("format", std::string("?")) + ")"
            : "Inspect failed: " + result_.value("error", std::string("unknown"));
    });
}

void ArtifactsWorkspace::HandleCommand(UiContext& context, const std::string& command) {
    static const std::string prefix = "inspect_artifact ";
    static const std::string tab = "artifact_tab ";
    static const std::string position = "artifact_position ";
    if (command.rfind(tab, 0) == 0) {
        selectTab_ = command.substr(tab.size());
        return;
    }
    if (command.rfind(position, 0) == 0) {
        int line = 0, column = 0;
        if (std::sscanf(command.c_str() + position.size(), "%d:%d", &line, &column) == 2 && line > 0 && column > 0) {
            positionLine_ = line;
            positionColumn_ = column;
            LookupPosition(context);
        }
        return;
    }
    if (command.rfind(prefix, 0) != 0) return;
    const std::string path = command.substr(prefix.size());
    Copy(path_.data(), path_.size(), path);
    child_.fill(0);
    Inspect(context, path, {});
}

int ArtifactsWorkspace::TabFlags(const char* name) const {
    return selectTab_ == name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
}

void ArtifactsWorkspace::Draw(UiContext& context) {
    ImGui::SetNextItemWidth(std::max(Px(240), ImGui::GetContentRegionAvail().x - ButtonWidth("Browse...") -
                                                  ButtonWidth("Inspect") - Px(16)));
    const bool enter = ImGui::InputTextWithHint("##ArtifactPath", "file: .exe, .dll, .asar ...", path_.data(), path_.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string chosen;
        if (ShowOpenFileDialog(L"All files\0*.*\0Executables and libraries\0*.exe;*.dll\0Electron archives\0*.asar\0",
                               chosen)) {
            Copy(path_.data(), path_.size(), chosen);
            child_.fill(0);
            Inspect(context, chosen, {});
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Inspect") || enter) Inspect(context, path_.data(), child_.data());
    ImGui::SetNextItemWidth(std::min(Px(420), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##ArtifactChild", "entry inside a container (nested: a.asar!b.js)", child_.data(), child_.size());
    HintText("Static analysis of files: nothing is executed or loaded. The same providers answer cortex.exe inspect "
             "and the cortex_artifact_* MCP tools.");
    ImGui::Separator();
    DrawResult(context);
}

void ArtifactsWorkspace::DrawResult(UiContext& context) {
    json result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = result_;
    }
    if (result.is_null()) {
        ImGui::TextDisabled("No artifact inspected yet.");
        return;
    }
    if (!result.value("ok", false)) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", result.value("error", std::string("failed")).c_str());
        if (result.contains("identity")) JsonTree(result["identity"], "identity", 0);
        return;
    }
    const json identity = result.value("identity", json::object());
    ImGui::Text("%s", identity.value("name", std::string()).c_str());
    ImGui::TextDisabled("%s  -  %llu bytes  -  SHA-256 %s", result.value("format", std::string()).c_str(),
                        static_cast<unsigned long long>(identity.value("size", uint64_t{0})),
                        identity.value("sha256", std::string()).c_str());
    if (ImGui::SmallButton("Copy SHA-256")) ImGui::SetClipboardText(identity.value("sha256", std::string()).c_str());
    ImGui::SameLine();
    // Hands the artifact to agents through the shared investigation context.
    const bool canRecord = context.contextModel && context.payload && context.payload->Ready();
    ImGui::BeginDisabled(!canRecord);
    if (ImGui::SmallButton("Record in investigation context")) {
        std::string error;
        const auto target = context.sessions ? context.sessions->ActiveTarget() : std::nullopt;
        const json data = {{"artifact", {{"name", identity.value("name", std::string())},
                                         {"sha256", identity.value("sha256", std::string())},
                                         {"format", result.value("format", std::string())},
                                         {"provider", result.value("provider", json::object()).value("name", std::string())}}}};
        if (context.contextModel->Observe("Inspected artifact " + identity.value("name", std::string()), data,
                                          {{{"source", "artifact"}, {"reference", "sha256:" + identity.value("sha256", std::string())}}},
                                          target ? target->generation : 0, &error))
            context.status = "Artifact recorded in the investigation context";
        else
            context.status = "Record failed: " + error;
    }
    ImGui::EndDisabled();
    if (!canRecord && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Needs a connected Cortex runtime in a target (the context lives there).");

    // Static, decompile and live are separate capabilities, each validated
    // on its own; the provider says which ones it has.
    if (BeginDataTable("ArtifactCapabilities", 3, 0, ImVec2(0, 0), 90.0f)) {
        ImGui::TableSetupColumn("Capability", ImGuiTableColumnFlags_WidthFixed, Px(100));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Px(110));
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const json capabilities = result.value("capabilities", json::object());
        for (const char* name : {"static", "decompile", "live"}) {
            const json row = capabilities.value(name, json::object());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            const std::string status = row.value("status", std::string("unavailable"));
            if (status == "validated") ImGui::TextColored(ImVec4(0.35f, 0.78f, 0.45f, 1.0f), "%s", status.c_str());
            else ImGui::TextDisabled("%s", status.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", row.value("note", std::string()).c_str());
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginTabBar("ArtifactTabs")) {
        if (ImGui::BeginTabItem("Structure", nullptr, TabFlags("Structure"))) {
            ImGui::BeginChild("ArtifactTree", ImVec2(0, 0), ImGuiChildFlags_None);
            JsonTree(result.value("data", json::object()), result.value("format", std::string("data")).c_str(), 0);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        bool hasChildren = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasChildren = children_.is_object() && children_.contains("children");
        }
        if (hasChildren && ImGui::BeginTabItem("Entries")) {
            DrawChildren(context);
            ImGui::EndTabItem();
        }
        const json data = result.value("data", json::object());
        const json electron = data.value("electron", json());
        bool electronUsed = false;
        if (electron.is_object()) {
            const json ipc = electron.value("ipc", json());
            const bool channels = ipc.is_object() ? !ipc.value("channels", json::array()).empty() : ipc.is_array() && !ipc.empty();
            electronUsed = channels || !electron.value("windows", json::array()).empty() ||
                           !electron.value("bridge", json::array()).empty();
        }
        if (electronUsed && ImGui::BeginTabItem("Electron", nullptr, TabFlags("Electron"))) {
            DrawElectron(data);
            ImGui::EndTabItem();
        }
        if (data.contains("modules") && ImGui::BeginTabItem("Modules", nullptr, TabFlags("Modules"))) {
            DrawModules(data["modules"]);
            ImGui::EndTabItem();
        }
        const std::string format = result.value("format", std::string());
        if ((format == "sourcemap" || (format == "javascript" && data.contains("source_map"))) &&
            ImGui::BeginTabItem("Source position", nullptr, TabFlags("Source position"))) {
            DrawSourcePosition(context);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        selectTab_.clear();
    }
}

namespace {

std::string Where(const json& row) {
    if (row.contains("at")) return std::to_string(row["at"].value("line", 0)) + ":" + std::to_string(row["at"].value("column", 0));
    return row.value("file", std::string()) + ":" + std::to_string(row.value("line", 0));
}

std::string Preference(const json& preferences, const char* key) {
    if (!preferences.is_object() || !preferences.contains(key)) return "(default)";
    const json& value = preferences[key];
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_string()) return value.get<std::string>();
    return value.value("expression", value.dump());
}

} // namespace

// Windows, bridge and IPC channels: what the source names, never observed
// traffic. One script lists its own uses; an archive aggregates them.
void ArtifactsWorkspace::DrawElectron(const json& data) {
    const json electron = data.value("electron", json::object());
    HintText("Static view: names found in the source, not observed traffic. Channels built at run time are only counted.");
    const json windows = electron.value("windows", json::array());
    if (!windows.empty() && BeginDataTable("ElectronWindows", 6, 0, ImVec2(0, 0), 140.0f)) {
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Constructor", ImGuiTableColumnFlags_WidthFixed, Px(120));
        ImGui::TableSetupColumn("contextIsolation", ImGuiTableColumnFlags_WidthFixed, Px(120));
        ImGui::TableSetupColumn("nodeIntegration", ImGuiTableColumnFlags_WidthFixed, Px(120));
        ImGui::TableSetupColumn("sandbox", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::TableSetupColumn("Preload", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& window : windows) {
            const json preferences = window.value("web_preferences", json());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(Where(window).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(window.value("constructor", std::string()).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(Preference(preferences, "contextIsolation").c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(Preference(preferences, "nodeIntegration").c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(Preference(preferences, "sandbox").c_str());
            ImGui::TableSetColumnIndex(5);
            const json preload = window.value("preload", json());
            if (preload.is_object()) {
                const json resolved = preload.value("resolved", json());
                ImGui::TextUnformatted(resolved.is_string() ? resolved.get<std::string>().c_str()
                                                            : preload.value("expression", std::string()).c_str());
            }
        }
        ImGui::EndTable();
    }
    for (const auto& exposure : electron.value("bridge", json::array()))
        ImGui::BulletText("contextBridge exposes window.%s  (%s)", exposure.value("key", std::string()).c_str(),
                          Where(exposure).c_str());

    // An archive groups uses by channel; a single script lists them.
    const json ipc = electron.value("ipc", json());
    if (ipc.is_object()) {
        const json channels = ipc.value("channels", json::array());
        ImGui::Text("%zu IPC channel(s), %d computed", channels.size(), ipc.value("computed_channels", 0));
        for (const char* list : {"called_without_handler", "handled_without_caller"})
            for (const auto& channel : ipc.value(list, json::array()))
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s: %s", list, channel.get<std::string>().c_str());
        if (BeginDataTable("ElectronChannels", 5, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Main handles", ImGuiTableColumnFlags_WidthFixed, Px(100));
            ImGui::TableSetupColumn("Renderer calls", ImGuiTableColumnFlags_WidthFixed, Px(110));
            ImGui::TableSetupColumn("To renderer", ImGuiTableColumnFlags_WidthFixed, Px(100));
            ImGui::TableSetupColumn("Uses", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& channel : channels) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(channel.value("channel", std::string()).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(channel.value("handled_in_main", false) ? "yes" : "-");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(channel.value("called_from_renderer", false) ? "yes" : "-");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(channel.value("sent_to_renderer", false) ? "yes" : "-");
                ImGui::TableSetColumnIndex(4);
                std::string uses;
                for (const auto& use : channel.value("uses", json::array()))
                    uses += (uses.empty() ? "" : ", ") + use.value("object", std::string()) + "." +
                            use.value("method", std::string()) + " @" + Where(use);
                ImGui::TextWrapped("%s", uses.c_str());
            }
            ImGui::EndTable();
        }
    } else if (ipc.is_array() && BeginDataTable("ElectronUses", 4, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthFixed, Px(220));
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, Px(150));
        ImGui::TableSetupColumn("Line:col", ImGuiTableColumnFlags_WidthFixed, Px(90));
        ImGui::TableHeadersRow();
        for (const auto& use : ipc) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(use.value("channel", std::string()).c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s.%s", use.value("object", std::string()).c_str(), use.value("method", std::string()).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(use.value("role", std::string()).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(Where(use).c_str());
        }
        ImGui::EndTable();
    }
}

void ArtifactsWorkspace::DrawModules(const json& modules) {
    const json edges = modules.value("edges", json::array());
    const json unresolved = modules.value("unresolved", json::array());
    ImGui::Text("%zu file(s), %zu edge(s), %zu unresolved, %zu package(s)", modules.value("nodes", json::array()).size(),
                edges.size(), unresolved.size(), modules.value("packages", json::object()).size());
    HintText("Every import/require with a constant specifier, whether or not it runs.");
    for (const auto& row : unresolved)
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s:%d  %s  (%s)", row.value("from", std::string()).c_str(),
                           row.value("line", 0), row.value("specifier", std::string()).c_str(),
                           row.value("reason", std::string()).c_str());
    if (!BeginDataTable("ModuleEdges", 4, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Px(120));
    ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, Px(60));
    ImGui::TableHeadersRow();
    for (const auto& edge : edges) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(edge.value("from", std::string()).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(edge.value("to", std::string()).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(edge.value("kind", std::string()).c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d", edge.value("line", 0));
    }
    ImGui::EndTable();
}

void ArtifactsWorkspace::DrawSourcePosition(UiContext& context) {
    HintText("A 1-based line and column of this generated script, mapped through its source map.");
    ImGui::SetNextItemWidth(Px(100));
    ImGui::InputInt("Line", &positionLine_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(100));
    ImGui::InputInt("Column", &positionColumn_);
    positionLine_ = std::max(1, positionLine_);
    positionColumn_ = std::max(1, positionColumn_);
    ImGui::SameLine();
    if (ImGui::Button("Map to original")) LookupPosition(context);
    json answer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        answer = position_;
    }
    if (answer.is_null()) return;
    if (!answer.value("ok", false)) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", answer.value("error", std::string("failed")).c_str());
        return;
    }
    const json original = answer.value("original", json());
    if (original.is_null()) {
        ImGui::TextDisabled("No mapping at or before this position.");
        return;
    }
    ImGui::Text("%s:%d:%d", original.value("source", std::string()).c_str(), original.value("line", 0),
                original.value("column", 0));
    if (original.contains("name") && original["name"].is_string())
        ImGui::Text("name: %s", original["name"].get<std::string>().c_str());
    if (original.contains("line_text")) ImGui::TextWrapped("%s", original["line_text"].get<std::string>().c_str());
    ImGui::TextDisabled("map: %s", answer.value("map", json::object()).value("name", std::string()).c_str());
}

void ArtifactsWorkspace::LookupPosition(UiContext& context) {
    std::string path, chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = inspectedPath_;
        chain = inspectedChild_;
    }
    const uint32_t line = static_cast<uint32_t>(positionLine_), column = static_cast<uint32_t>(positionColumn_);
    context.RunInBackground("Mapping source position", [this, &context, path, chain, line, column]() {
        std::string error;
        json answer;
        auto source = mcp_bridge::artifacts::OpenChain(path, chain, error);
        if (source) answer = cortex::artifacts::SourcePosition(*source, line, column,
                                                               mcp_bridge::artifacts::MapOpenerFor(path, chain, nullptr), error);
        if (answer.is_null()) answer = {{"ok", false}, {"error", error}};
        const json original = answer.value("original", json());
        context.status = original.is_object()
            ? "Mapped to " + original.value("source", std::string()) + ":" + std::to_string(original.value("line", 0)) +
                  ":" + std::to_string(original.value("column", 0))
            : "No original position: " + answer.value("error", std::string("no mapping"));
        std::lock_guard<std::mutex> lock(mutex_);
        position_ = std::move(answer);
    });
}

void ArtifactsWorkspace::DrawChildren(UiContext& context) {
    json children;
    std::string path, chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        children = children_.value("children", json::array());
        path = inspectedPath_;
        chain = inspectedChild_;
    }
    ImGui::SetNextItemWidth(Px(260));
    ImGui::InputTextWithHint("##ArtifactChildFilter", "filter entries", childFilter_.data(), childFilter_.size());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu entr%s - double-click to inspect one", children.size(), children.size() == 1 ? "y" : "ies");
    if (!BeginDataTable("ArtifactEntries", 3, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Px(100));
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, Px(240));
    ImGui::TableHeadersRow();
    const std::string filter = childFilter_.data();
    int row = 0;
    for (const auto& entry : children) {
        const std::string name = entry.value("name", std::string());
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;
        ImGui::PushID(row++);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool readable = entry.value("readable", false);
        if (!readable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
            readable && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const std::string next = chain.empty() ? name : chain + "!" + name;
            Copy(child_.data(), child_.size(), next);
            Inspect(context, path, next);
        }
        if (!readable) ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%llu", static_cast<unsigned long long>(entry.value("size", uint64_t{0})));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", entry.value("note", std::string()).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace cortex::ui

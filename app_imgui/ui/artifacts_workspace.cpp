#include "artifacts_workspace.h"

#include "file_dialog.h"
#include "widgets.h"

#include "../../mcp_bridge/artifact_tools.h"
#include "../../core/artifacts/annotations.h"
#include "../../core/artifacts/dotnet_provider.h"
#include "../../core/services/user_data.h"
#include "../../core/artifacts/js_provider.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cortex::desktop {
std::string ExecutableDirectory();
}

namespace cortex::ui {
namespace {

// Annotations made in the desktop: the same per-user store the MCP host
// uses by default, so a name given here is what an agent sees.
cortex::artifacts::AnnotationStore& DesktopAnnotations() {
    static cortex::artifacts::AnnotationStore store(
        cortex::services::UserDataDirectory(std::filesystem::u8path(cortex::desktop::ExecutableDirectory())) / "annotations");
    return store;
}

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
        cil_ = nullptr;
        functions_ = nullptr;
        codeDetail_ = nullptr;
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
    static const std::string cil = "artifact_cil ";
    if (command == "artifact_functions") {
        LoadFunctions(context);
        return;
    }
    if (command.rfind("artifact_function ", 0) == 0) {
        OpenFunction(context, command.substr(18));
        return;
    }
    if (command.rfind("artifact_rename ", 0) == 0) {
        const std::string rest = command.substr(16);
        const size_t space = rest.find(' ');
        Rename(context, rest.substr(0, space), space == std::string::npos ? std::string() : rest.substr(space + 1));
        return;
    }
    if (command.rfind(cil, 0) == 0) {
        Disassemble(context, command.substr(cil.size()));
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
        const std::string machine = data.value("machine", std::string());
        if (data.contains("sections") && (machine.find("x64") != std::string::npos || machine.find("x86") != std::string::npos ||
                                          machine.find("amd64") != std::string::npos || machine.find("i386") != std::string::npos) &&
            ImGui::BeginTabItem("Code", nullptr, TabFlags("Code"))) {
            DrawCode(context);
            ImGui::EndTabItem();
        }
        if (data.contains("dotnet") && ImGui::BeginTabItem(".NET", nullptr, TabFlags(".NET"))) {
            DrawDotNet(context, data["dotnet"]);
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

// Assembly identity, types and members; a method's CIL on demand.
void ArtifactsWorkspace::DrawDotNet(UiContext& context, const json& dotnet) {
    if (dotnet.contains("error")) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "metadata: %s", dotnet.value("error", std::string()).c_str());
        return;
    }
    const json assembly = dotnet.value("assembly", json::object());
    ImGui::Text("%s %s  -  metadata %s", assembly.value("name", std::string("(no assembly)")).c_str(),
                assembly.value("version", std::string()).c_str(), dotnet.value("metadata_version", std::string()).c_str());
    std::string references;
    for (const auto& reference : dotnet.value("assembly_refs", json::array()))
        references += (references.empty() ? "" : ", ") + reference.value("name", std::string()) + " " +
                      reference.value("version", std::string());
    if (!references.empty()) ImGui::TextWrapped("References: %s", references.c_str());
    for (const auto& import : dotnet.value("pinvoke", json::array()))
        ImGui::BulletText("P/Invoke %s -> %s!%s", import.value("method", std::string()).c_str(),
                          import.value("module", std::string()).c_str(), import.value("entry", std::string()).c_str());
    HintText("Double-click a method to disassemble its CIL (static; not decompiled).");

    const float listWidth = std::max(Px(320), ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::BeginChild("DotNetTypes", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##TypeFilter", "filter types and members", typeFilter_.data(), typeFilter_.size());
    const std::string filter = typeFilter_.data();
    for (const auto& type : dotnet.value("types", json::array())) {
        const std::string typeName = type.value("name", std::string());
        bool typeMatches = filter.empty() || typeName.find(filter) != std::string::npos;
        if (!typeMatches)
            for (const auto& method : type.value("methods", json::array()))
                typeMatches = typeMatches || method.value("name", std::string()).find(filter) != std::string::npos;
        if (!typeMatches) continue;
        ImGui::PushID(typeName.c_str());
        if (ImGui::TreeNode(typeName.c_str())) {
            for (const auto& field : type.value("fields", json::array()))
                ImGui::TextDisabled("field %s %s", field.value("signature", std::string()).c_str(),
                                    field.value("name", std::string()).c_str());
            for (const auto& method : type.value("methods", json::array())) {
                const std::string label = method.value("name", std::string()) + "  " + method.value("signature", std::string());
                const std::string token = method.value("token", std::string());
                ImGui::PushID(token.c_str());
                if (ImGui::Selectable(label.c_str(), cil_.is_object() && cil_.value("query", std::string()) == token,
                                      ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    Disassemble(context, token);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("DotNetCil", ImVec2(0, 0), ImGuiChildFlags_Borders);
    json cil;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cil = cil_;
    }
    if (!cil.is_object()) {
        ImGui::TextDisabled("No method disassembled.");
    } else if (!cil.value("ok", false)) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", cil.value("error", std::string()).c_str());
    } else {
        for (const auto& method : cil.value("methods", json::array())) {
            ImGui::Text("%s  %s", method.value("method", std::string()).c_str(), method.value("signature", std::string()).c_str());
            if (!method.contains("instructions")) {
                ImGui::TextDisabled("no body: %s", method.value("reason", std::string()).c_str());
                continue;
            }
            ImGui::TextDisabled("%s header, max stack %d, %d bytes", method.value("header", std::string()).c_str(),
                                method.value("max_stack", 0), method.value("code_size", 0));
            for (const auto& clause : method.value("exception_clauses", json::array()))
                ImGui::TextDisabled(".try IL_%04x..IL_%04x %s IL_%04x %s", clause.value("try_offset", 0),
                                    clause.value("try_offset", 0) + clause.value("try_length", 0),
                                    clause.value("kind", std::string()).c_str(), clause.value("handler_offset", 0),
                                    clause.value("catch_type", json()).is_string() ? clause["catch_type"].get<std::string>().c_str() : "");
            for (const auto& instruction : method.value("instructions", json::array()))
                ImGui::Text("IL_%04x  %-12s %s", instruction.value("offset", 0), instruction.value("opcode", std::string()).c_str(),
                            instruction.value("operand", std::string()).c_str());
            if (method.contains("warning")) ImGui::TextDisabled("%s", method["warning"].get<std::string>().c_str());
            ImGui::Separator();
        }
    }
    ImGui::EndChild();
}

void ArtifactsWorkspace::Disassemble(UiContext& context, const std::string& method) {
    std::string path, chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = inspectedPath_;
        chain = inspectedChild_;
    }
    context.RunInBackground("Disassembling " + method, [this, &context, path, chain, method]() {
        std::string error;
        json answer;
        auto source = mcp_bridge::artifacts::OpenChain(path, chain, error);
        answer = source ? cortex::artifacts::DisassembleMethods(*source, method, 20000) : json{{"ok", false}, {"error", error}};
        answer["query"] = method;
        const size_t count = answer.value("methods", json::array()).size();
        context.status = answer.value("ok", false) ? "Disassembled " + std::to_string(count) + " method(s): " + method
                                                   : "CIL failed: " + answer.value("error", std::string());
        std::lock_guard<std::mutex> lock(mutex_);
        cil_ = std::move(answer);
    });
}

// Native code of a PE: the function catalog (with provenance), a function's
// CFG and the references to it, and names kept as annotations.
void ArtifactsWorkspace::DrawCode(UiContext& context) {
    json functions, detail;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functions = functions_;
        detail = codeDetail_;
    }
    if (!functions.is_object()) {
        if (ImGui::Button("Analyse code")) LoadFunctions(context);
        ImGui::SameLine();
        HintText("Builds the function catalog and cross-references from the file (offline, nothing runs).");
        return;
    }
    if (!functions.value("ok", false)) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", functions.value("error", std::string()).c_str());
        return;
    }
    std::string sources;
    const json bySource = functions.value("by_source", json::object());
    for (const auto& [source, count] : bySource.items())
        sources += (sources.empty() ? "" : ", ") + source + " " + std::to_string(count.get<size_t>());
    ImGui::Text("%s: %zu functions (%s), %zu xrefs, %zu strings", functions.value("architecture", std::string()).c_str(),
                functions.value("total", size_t{0}), sources.c_str(), functions.value("xrefs", size_t{0}),
                functions.value("strings", size_t{0}));
    const float listWidth = std::max(Px(300), ImGui::GetContentRegionAvail().x * 0.38f);
    ImGui::BeginChild("CodeFunctions", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##CodeFilter", "filter functions", codeFilter_.data(), codeFilter_.size());
    const std::string filter = codeFilter_.data();
    if (BeginDataTable("CodeFunctionTable", 3, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("RVA", ImGuiTableColumnFlags_WidthFixed, Px(80));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Provenance", ImGuiTableColumnFlags_WidthFixed, Px(150));
        ImGui::TableHeadersRow();
        int shown = 0;
        for (const auto& function : functions.value("functions", json::array())) {
            const std::string name = function.value("name", std::string());
            if (!filter.empty() && name.find(filter) == std::string::npos) continue;
            if (++shown > 5000) break;
            const std::string rva = function.value("rva", std::string());
            ImGui::PushID(rva.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool open = detail.is_object() && detail.value("rva", std::string()) == rva;
            if (ImGui::Selectable(rva.c_str(), open, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                OpenFunction(context, rva);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableSetColumnIndex(2);
            std::string provenance;
            for (const auto& source : function.value("sources", json::array()))
                provenance += (provenance.empty() ? "" : ",") + source.get<std::string>();
            ImGui::TextDisabled("%s", provenance.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("CodeDetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (!detail.is_object()) {
        ImGui::TextDisabled("Double-click a function for its CFG and references.");
    } else {
        const json cfg = detail.value("cfg", json::object());
        const std::string rva = detail.value("rva", std::string());
        ImGui::Text("%s  %s", rva.c_str(), cfg.value("name", std::string()).c_str());
        ImGui::SetNextItemWidth(Px(220));
        ImGui::InputTextWithHint("##Rename", "new name", rename_.data(), rename_.size());
        ImGui::SameLine();
        if (ImGui::Button("Rename")) Rename(context, rva, rename_.data());
        HintText("A name is an annotation keyed by the file's SHA-256; agents see it too.");
        const json xrefs = detail.value("xrefs", json::object());
        ImGui::Text("%zu reference(s)", xrefs.value("total", size_t{0}));
        for (const auto& ref : xrefs.value("references", json::array())) {
            const json function = ref.value("function", json::object());
            ImGui::BulletText("%s %s in %s", ref.value("type", std::string()).c_str(), ref.value("from", std::string()).c_str(),
                              function.value("name", std::string("?")).c_str());
        }
        ImGui::Separator();
        for (const auto& block : cfg.value("blocks", json::array())) {
            ImGui::TextDisabled("block %s", block.value("start", std::string()).c_str());
            for (const auto& instruction : block.value("instructions", json::array())) {
                std::string text = instruction.value("text", std::string());
                if (instruction.contains("call")) text += "    ; " + instruction["call"].get<std::string>();
                ImGui::Text("  %s  %s", instruction.value("rva", std::string()).c_str(), text.c_str());
            }
        }
        for (const auto& edge : cfg.value("edges", json::array()))
            ImGui::TextDisabled("%s -> %s (%s)", edge.value("from", std::string()).c_str(),
                                edge["to"].is_string() ? edge["to"].get<std::string>().c_str() : "?",
                                edge.value("type", std::string()).c_str());
    }
    ImGui::EndChild();
}

void ArtifactsWorkspace::LoadFunctions(UiContext& context, bool announce) {
    std::string path, chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = inspectedPath_;
        chain = inspectedChild_;
    }
    context.RunInBackground("Analysing code", [this, &context, path, chain, announce]() {
        std::string error;
        auto source = mcp_bridge::artifacts::OpenChain(path, chain, error);
        json answer = source ? mcp_bridge::artifacts::HandleCode("cortex_artifact_functions", {{"max_entries", 65536}}, *source,
                                                                &DesktopAnnotations(), "desktop")
                             : json{{"ok", false}, {"error", error}};
        if (announce || !answer.value("ok", false))
            context.status = answer.value("ok", false)
                ? "Code analysed: " + std::to_string(answer.value("total", size_t{0})) + " functions"
                : "Code analysis failed: " + answer.value("error", std::string());
        std::lock_guard<std::mutex> lock(mutex_);
        functions_ = std::move(answer);
    });
}

void ArtifactsWorkspace::OpenFunction(UiContext& context, const std::string& location, bool announce) {
    std::string path, chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = inspectedPath_;
        chain = inspectedChild_;
    }
    context.RunInBackground("Opening " + location, [this, &context, path, chain, location, announce]() {
        std::string error;
        auto source = mcp_bridge::artifacts::OpenChain(path, chain, error);
        json detail;
        if (source) {
            auto* store = &DesktopAnnotations();
            detail["cfg"] = mcp_bridge::artifacts::HandleCode("cortex_artifact_cfg", {{"function", location}}, *source, store, "desktop");
            detail["xrefs"] = mcp_bridge::artifacts::HandleCode("cortex_artifact_xrefs", {{"target", location}, {"max_entries", 200}},
                                                                *source, store, "desktop");
            detail["rva"] = detail["cfg"].value("function", location);
        }
        const bool ok = source && detail["cfg"].value("ok", false);
        if (announce || !ok) context.status = ok ? "Function " + detail["cfg"].value("name", location) + ": " +
                                  std::to_string(detail["cfg"].value("blocks", json::array()).size()) + " blocks"
                            : "Function failed: " + (source ? detail["cfg"].value("error", std::string()) : error);
        std::lock_guard<std::mutex> lock(mutex_);
        codeDetail_ = ok ? std::move(detail) : json(nullptr);
    });
}

void ArtifactsWorkspace::Rename(UiContext& context, const std::string& rva, const std::string& name) {
    std::string path, chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = inspectedPath_;
        chain = inspectedChild_;
    }
    std::string error;
    auto source = mcp_bridge::artifacts::OpenChain(path, chain, error);
    if (!source) {
        context.status = "Rename failed: " + error;
        return;
    }
    // The desktop writes the person's own annotation: no mutation_permission,
    // which is the agent-side declaration.
    const json result = DesktopAnnotations().Put(cortex::artifacts::Sha256Of(*source), "rva:" + rva, "name", name, "desktop", -1);
    context.status = result.value("ok", false) ? (name.empty() ? "Name removed" : "Renamed " + rva + " to " + name)
                                               : "Rename failed: " + result.value("error", std::string());
    if (result.value("ok", false)) {
        LoadFunctions(context, false);
        OpenFunction(context, rva, false);
    }
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

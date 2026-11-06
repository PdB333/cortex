#include "ai_investigation_workspace.h"

#include "address_context_menu.h"
#include "code_navigation.h"
#include "desktop_stores.h"
#include "widgets.h"

#include "../../core/target/process_tree.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <cstdio>

namespace cortex::ui {
namespace {

using json = nlohmann::json;

ImVec4 StateColor(const std::string& state) {
    if (state == "completed") return ImVec4(0.35f, 0.78f, 0.45f, 1.0f);
    if (state == "awaiting_approval") return ImVec4(0.95f, 0.75f, 0.3f, 1.0f);
    if (state == "failed" || state == "cancelled") return ImVec4(0.9f, 0.4f, 0.35f, 1.0f);
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

uint64_t ParseRva(const std::string& location) {
    if (location.rfind("rva:0x", 0) != 0) return 0;
    try {
        return std::stoull(location.substr(6), nullptr, 16);
    } catch (...) {
        return 0;
    }
}

} // namespace

int AiInvestigationWorkspace::TabFlags(const char* name) const {
    return selectTab_ == name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
}

void AiInvestigationWorkspace::HandleCommand(UiContext& context, const std::string& command) {
    auto argument = [&](const char* prefix) { return command.substr(std::string(prefix).size()); };
    if (command == "ai_investigations_refresh") Refresh(context);
    else if (command.rfind("ai_investigation_open ", 0) == 0) Open(context, argument("ai_investigation_open "));
    else if (command.rfind("ai_investigation_approve ", 0) == 0 || command.rfind("ai_investigation_reject ", 0) == 0) {
        const bool approve = command.rfind("ai_investigation_approve ", 0) == 0;
        const std::string rest = argument(approve ? "ai_investigation_approve " : "ai_investigation_reject ");
        const size_t space = rest.find(' ');
        if (space != std::string::npos) Decide(context, rest.substr(0, space), std::strtoul(rest.c_str() + space + 1, nullptr, 10), approve);
    } else if (command == "ai_evidence_refresh") RefreshEvidence(context);
    else if (command.rfind("ai_evidence_live ", 0) == 0 || command.rfind("ai_evidence_static ", 0) == 0) {
        const bool live = command.rfind("ai_evidence_live ", 0) == 0;
        const json found = DesktopEvidence().Get(argument(live ? "ai_evidence_live " : "ai_evidence_static "));
        if (!found.value("ok", false)) context.status = "Evidence not found";
        else if (live) EvidenceLive(context, found["record"]);
        else EvidenceStatic(context, found["record"]);
    } else if (command.rfind("ai_process_tree ", 0) == 0) LoadProcessTree(context, std::strtoull(argument("ai_process_tree ").c_str(), nullptr, 10));
    else if (command.rfind("ai_tab ", 0) == 0) selectTab_ = argument("ai_tab ");
    else if (command.rfind("ai_live_attach ", 0) == 0) LiveAttach(context, std::atoi(argument("ai_live_attach ").c_str()));
    else if (command.rfind("ai_live_attach_dap ", 0) == 0) {
        std::istringstream in(argument("ai_live_attach_dap "));
        int port = 0, pid = 0;
        std::string adapter;
        in >> port >> adapter >> pid;
        LiveAttachDap(context, port, adapter, pid);
    } else if (command.rfind("ai_live_script ", 0) == 0) LiveOpenScript(context, argument("ai_live_script "));
    else if (command.rfind("ai_live_breakpoint ", 0) == 0) LiveBreakpoint(context, std::atoi(argument("ai_live_breakpoint ").c_str()));
    else if (command == "ai_live_hits") {
        auto session = DesktopLive().Get(liveSession_);
        if (session && liveBreakpoint_) {
            liveHits_ = session->Hits(liveBreakpoint_, 0, 200);
            context.status = "Live: " + std::to_string(liveHits_.value("hits", 0)) + " hit(s)";
        }
    } else if (command == "ai_live_detach") LiveDetach(context);
}

void AiInvestigationWorkspace::Draw(UiContext& context) {
    HintText("No AI runs inside Cortex: an agent connects through your MCP client (Home > Work with an AI agent). "
             "Here you follow its investigations, approve or reject the steps that touch a program, and check the "
             "evidence it recorded.");
    if (ImGui::BeginTabBar("AiTabs")) {
        if (ImGui::BeginTabItem("Investigations", nullptr, TabFlags("Investigations"))) {
            DrawInvestigations(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Evidence", nullptr, TabFlags("Evidence"))) {
            DrawEvidence(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Process tree", nullptr, TabFlags("Process tree"))) {
            DrawProcessTree(context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Live (CDP)", nullptr, TabFlags("Live (CDP)"))) {
            DrawLive(context);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        selectTab_.clear();
    }
}

void AiInvestigationWorkspace::Refresh(UiContext& context) {
    investigations_ = DesktopInvestigations().List({}, 200);
    size_t waiting = 0;
    for (const auto& row : investigations_.value("investigations", json::array()))
        if (row.value("state", std::string()) == "awaiting_approval") ++waiting;
    context.status = "Investigations: " + std::to_string(investigations_.value("investigations", json::array()).size()) +
                     ", " + std::to_string(waiting) + " awaiting approval";
}

void AiInvestigationWorkspace::Open(UiContext& context, const std::string& id) {
    const json found = DesktopInvestigations().Get(id);
    selected_ = found.value("ok", false) ? found["investigation"] : json(nullptr);
    context.status = selected_.is_object() ? "Investigation " + selected_.value("title", std::string()) + ": " +
                                                 selected_.value("state", std::string())
                                           : "Investigation not found";
}

void AiInvestigationWorkspace::Decide(UiContext& context, const std::string& id, size_t step, bool approve) {
    // The person's decision: recorded with the actor "desktop".
    const json result = DesktopInvestigations().Approve(id, step, approve, note_.data(), "desktop");
    if (result.value("ok", false)) {
        selected_ = result["investigation"];
        note_.fill(0);
        context.status = std::string(approve ? "Approved" : "Rejected") + " step " + std::to_string(step) + " of " + id;
        Refresh(context);
        context.status = std::string(approve ? "Approved" : "Rejected") + " step " + std::to_string(step) + " of " + id;
    } else {
        context.status = "Decision refused: " + result.value("error", std::string()) + " " + result.value("reason", std::string());
    }
}

void AiInvestigationWorkspace::DrawInvestigations(UiContext& context) {
    if (ImGui::Button("Refresh")) Refresh(context);
    if (!investigations_.is_object()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Refresh to read the investigations agents created.");
        return;
    }
    const float listWidth = std::max(Px(320), ImGui::GetContentRegionAvail().x * 0.38f);
    ImGui::BeginChild("InvestigationList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    for (const auto& row : investigations_.value("investigations", json::array())) {
        const std::string id = row.value("id", std::string());
        ImGui::PushID(id.c_str());
        const bool open = selected_.is_object() && selected_.value("id", std::string()) == id;
        if (ImGui::Selectable(row.value("title", std::string()).c_str(), open)) Open(context, id);
        ImGui::TextColored(StateColor(row.value("state", std::string())), "  %s", row.value("state", std::string()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu/%zu steps  %s", row.value("steps_finished", size_t{0}), row.value("steps", size_t{0}),
                            row.value("workflow", std::string()).c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("InvestigationDetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (!selected_.is_object()) {
        ImGui::TextDisabled("Select an investigation.");
        ImGui::EndChild();
        return;
    }
    const std::string id = selected_.value("id", std::string());
    ImGui::TextWrapped("%s", selected_.value("question", std::string()).c_str());
    ImGui::TextColored(StateColor(selected_.value("state", std::string())), "%s", selected_.value("state", std::string()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy report"))
        ImGui::SetClipboardText(DesktopInvestigations().Report(id).value("document", std::string()).c_str());
    ImGui::SetNextItemWidth(Px(320));
    ImGui::InputTextWithHint("##DecisionNote", "note for your decision (optional)", note_.data(), note_.size());
    if (BeginDataTable("InvestigationPlan", 6, 0, ImVec2(0, 0), 200.0f)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, Px(24));
        ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthFixed, Px(170));
        ImGui::TableSetupColumn("Capability / effect", ImGuiTableColumnFlags_WidthFixed, Px(150));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Px(130));
        ImGui::TableSetupColumn("Decision", ImGuiTableColumnFlags_WidthFixed, Px(140));
        ImGui::TableHeadersRow();
        for (const auto& step : selected_.value("plan", json::array())) {
            const size_t index = step.value("index", size_t{0});
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", index);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", step.value("title", std::string()).c_str());
            if (step.contains("hop")) ImGui::TextDisabled("%s", step.value("hop", std::string()).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(step.value("tool", std::string()).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s / %s", step.value("capability", std::string()).c_str(), step.value("effect", std::string()).c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(step.value("status", std::string()).c_str());
            ImGui::TableSetColumnIndex(5);
            if (step.value("status", std::string()) == "awaiting_approval") {
                if (ImGui::SmallButton("Approve")) Decide(context, id, index, true);
                ImGui::SameLine();
                if (ImGui::SmallButton("Reject")) Decide(context, id, index, false);
            } else if (step.contains("approval")) {
                ImGui::TextDisabled("%s by %s", step["approval"].value("approved", false) ? "approved" : "rejected",
                                    step["approval"].value("by", std::string()).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    HintText("Approving lets the agent record that step's result. The tool's own gates still apply "
             "(--allow-writes, launch profiles).");
    if (ImGui::CollapsingHeader("Journal")) {
        for (const auto& entry : selected_.value("journal", json::array()))
            ImGui::BulletText("%s by %s", entry.value("event", std::string()).c_str(), entry.value("actor", std::string()).c_str());
    }
    if (ImGui::CollapsingHeader("Evidence cited")) {
        for (const auto& evidenceId : selected_.value("evidence", json::array())) {
            const json record = DesktopEvidence().Get(evidenceId.get<std::string>());
            ImGui::BulletText("%s  %s", evidenceId.get<std::string>().c_str(),
                              record.value("ok", false) ? record["record"].value("statement", std::string()).c_str() : "(missing)");
        }
    }
    ImGui::EndChild();
}

void AiInvestigationWorkspace::RefreshEvidence(UiContext& context) {
    evidence_ = DesktopEvidence().List(json::object(), 1000);
    context.status = "Evidence: " + std::to_string(evidence_.value("matched", size_t{0})) + " record(s)";
}

void AiInvestigationWorkspace::EvidenceLive(UiContext& context, const json& record) {
    const json subject = record.value("subject", json::object());
    const uint64_t rva = ParseRva(subject.value("location", std::string()));
    const auto module = ModuleForSha256(context, subject.value("sha256", std::string()));
    if (!rva || !module) {
        context.status = "Live: no loaded module has this evidence's file (SHA-256), or no RVA";
        return;
    }
    context.NavigateTo("disassembly", module->base + rva);
    char text[96];
    std::snprintf(text, sizeof(text), "Live %s+0x%llx = 0x%llx", module->name.c_str(), static_cast<unsigned long long>(rva),
                  static_cast<unsigned long long>(module->base + rva));
    context.status = text;
}

void AiInvestigationWorkspace::EvidenceStatic(UiContext& context, const json& record) {
    const json subject = record.value("subject", json::object());
    const uint64_t rva = ParseRva(subject.value("location", std::string()));
    // The file on disk comes from a loaded module with the same SHA-256.
    const auto module = ModuleForSha256(context, subject.value("sha256", std::string()));
    if (!rva || !module) {
        context.status = "Static: the file is not known on this machine (no loaded module with its SHA-256), or no RVA";
        return;
    }
    OpenStaticCode(context, *module, module->base + rva);
    context.status = "Opening " + module->path + " at " + subject.value("location", std::string());
}

void AiInvestigationWorkspace::DrawEvidence(UiContext& context) {
    if (ImGui::Button("Refresh")) RefreshEvidence(context);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(260));
    ImGui::InputTextWithHint("##EvidenceFilter", "filter statements", evidenceFilter_.data(), evidenceFilter_.size());
    if (!evidence_.is_object()) return;
    HintText("Live and Static are enabled only through a loaded module whose file has the record's SHA-256.");
    const std::string filter = evidenceFilter_.data();
    if (!BeginDataTable("AiEvidence", 5, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, Px(150));
    ImGui::TableSetupColumn("Statement", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, Px(160));
    ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthFixed, Px(120));
    ImGui::TableSetupColumn("Go", ImGuiTableColumnFlags_WidthFixed, Px(110));
    ImGui::TableHeadersRow();
    const bool live = context.sessions && context.sessions->ActiveTarget();
    for (const auto& record : evidence_.value("records", json::array())) {
        const std::string statement = record.value("statement", std::string());
        if (!filter.empty() && statement.find(filter) == std::string::npos) continue;
        ImGui::PushID(record.value("id", std::string()).c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(record.value("class", std::string()).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", statement.c_str());
        ImGui::TableSetColumnIndex(2);
        const std::string location = record["subject"].value("location", std::string());
        ImGui::TextUnformatted(location.empty() ? "(artifact)" : location.c_str());
        ImGui::TableSetColumnIndex(3);
        const std::string status = record.value("verification", json::object()).value("status", std::string("not_checked"));
        if (record.value("stale", false)) ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "STALE");
        else ImGui::TextDisabled("%s", status.c_str());
        ImGui::TableSetColumnIndex(4);
        ImGui::BeginDisabled(!live || !ParseRva(location));
        if (ImGui::SmallButton("Live")) EvidenceLive(context, record);
        ImGui::SameLine();
        if (ImGui::SmallButton("Static")) EvidenceStatic(context, record);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void AiInvestigationWorkspace::LoadProcessTree(UiContext& context, uint64_t pid) {
    tree_ = cortex::target::ProcessTreeJson(cortex::target::SnapshotProcesses(), pid);
    const size_t count = tree_.value("processes", json::array()).size();
    context.status = tree_.value("ok", false)
        ? "Process tree: " + std::to_string(count) + " process(es)" + (tree_.value("electron", false) ? ", Electron/Chromium" : "")
        : "Process tree: " + tree_.value("error", std::string());
}

void AiInvestigationWorkspace::DrawProcessTree(UiContext& context) {
    if (treePid_ == 0 && context.sessions)
        if (const auto target = context.sessions->ActiveTarget()) treePid_ = static_cast<int>(target->processId);
    ImGui::SetNextItemWidth(Px(120));
    ImGui::InputInt("PID", &treePid_);
    ImGui::SameLine();
    if (ImGui::Button("Show tree") && treePid_ > 0) LoadProcessTree(context, static_cast<uint64_t>(treePid_));
    HintText("Roles come from each process's --type= switch (Electron, Chromium). Attach to the process that runs "
             "the code you want: main for ipcMain handlers, a renderer for page code, the main or a utility for "
             "native addons.");
    if (!tree_.is_object() || !tree_.value("ok", false)) return;
    if (!BeginDataTable("ProcessTree", 4, ImGuiTableFlags_ScrollY, ImGui::GetContentRegionAvail())) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, Px(240));
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, Px(70));
    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, Px(220));
    ImGui::TableSetupColumn("Command line", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& process : tree_["processes"]) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%*s%s", static_cast<int>(process.value("depth", size_t{0}) * 2), "", process.value("name", std::string()).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%llu", static_cast<unsigned long long>(process.value("pid", uint64_t{0})));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(process.value("role", std::string()).c_str());
        ImGui::TableSetColumnIndex(3);
        const json line = process.value("command_line", json());
        ImGui::TextDisabled("%s", line.is_string() ? line.get<std::string>().c_str() : "(not readable)");
    }
    ImGui::EndTable();
}

} // namespace cortex::ui

namespace cortex::ui {

void AiInvestigationWorkspace::LiveAttach(UiContext& context, int port) {
    if (port <= 0 || port > 65535) {
        context.status = "Live: invalid port";
        return;
    }
    if (!liveSession_.empty()) DesktopLive().Detach(liveSession_);
    liveSession_.clear();
    liveScripts_ = liveSource_ = liveHits_ = nullptr;
    liveBreakpoint_ = 0;
    const json attached = DesktopLive().Attach(static_cast<uint16_t>(port), "");
    if (!attached.value("ok", false)) {
        context.status = "Live: attach failed: " + attached.value("error", std::string());
        return;
    }
    livePort_ = port;
    liveSession_ = attached.value("session_id", std::string());
    liveStatus_ = attached;
    const json target = attached.contains("target") && attached["target"].is_object() ? attached["target"] : json::object();
    context.status = "Live: attached " + liveSession_ + " (" + target.value("type", std::string()) + " " +
                     target.value("title", std::string()) + ")";
}

void AiInvestigationWorkspace::LiveAttachDap(UiContext& context, int port, const std::string& adapter, int pid) {
    // netcoredbg with port 0: Cortex starts it in stdio mode (CORTEX_NETCOREDBG).
    const bool stdio = adapter == "netcoredbg" && port == 0;
    if (!stdio && (port <= 0 || port > 65535)) {
        context.status = "Live: invalid port";
        return;
    }
    // Attaching a managed debugger to a process follows the write permission.
    if (adapter == "netcoredbg" && !context.mutationAllowed) {
        context.status = "Live: attaching netcoredbg to a process needs write permission";
        return;
    }
    if (!liveSession_.empty()) DesktopLive().Detach(liveSession_);
    liveSession_.clear();
    liveScripts_ = liveSource_ = liveHits_ = nullptr;
    liveBreakpoint_ = 0;
    if (stdio && DesktopLive().Netcoredbg().empty())
        if (const char* path = std::getenv("CORTEX_NETCOREDBG")) DesktopLive().SetNetcoredbg(path);
    const json attached = stdio             ? DesktopLive().AttachNetcoredbgStdio(static_cast<uint64_t>(std::max(0, pid)))
                          : adapter == "jdwp" ? DesktopLive().AttachJdwp(static_cast<uint16_t>(port))
                                            : DesktopLive().AttachDap(static_cast<uint16_t>(port), adapter, static_cast<uint64_t>(std::max(0, pid)));
    if (!attached.value("ok", false)) {
        context.status = "Live: attach failed: " + attached.value("error", std::string());
        return;
    }
    livePort_ = port;
    liveSession_ = attached.value("session_id", std::string());
    liveStatus_ = attached;
    context.status = "Live: attached " + liveSession_ + " (" + adapter + ")";
}

void AiInvestigationWorkspace::LiveOpenScript(UiContext& context, const std::string& url) {
    auto session = DesktopLive().Get(liveSession_);
    if (!session) {
        context.status = "Live: not attached";
        return;
    }
    liveSource_ = session->Source(url, 1, 20000);
    // DAP: a file is readable once the debugger reported it in a stack;
    // before that the path is still usable for a breakpoint.
    if ((session->Protocol() == "dap" || session->Protocol() == "jdwp") && !liveSource_.value("ok", false))
        liveSource_ = {{"ok", true}, {"url", url}, {"text", "(the source is shown once the debugger reports this file in a stack)"}};
    context.status = liveSource_.value("ok", false) ? "Live: source of " + url : "Live: no script " + url;
}

void AiInvestigationWorkspace::LiveBreakpoint(UiContext& context, int line) {
    auto session = DesktopLive().Get(liveSession_);
    if (!session || !liveSource_.is_object() || !liveSource_.value("ok", false)) {
        context.status = "Live: open a script first";
        return;
    }
    // A breakpoint pauses the program briefly at each hit: it follows the
    // desktop's write permission like the native debugger.
    if (!context.mutationAllowed) {
        context.status = "Live: breakpoints need write permission (enable writes for this target)";
        return;
    }
    const json bp = session->SetLogBreakpoint(liveSource_.value("url", std::string()), static_cast<uint32_t>(std::max(1, line)), 0, 50, 8);
    if (!bp.value("ok", false)) {
        context.status = "Live: breakpoint failed: " + bp.value("error", std::string());
        return;
    }
    liveBreakpoint_ = bp.value("breakpoint", 0);
    liveHits_ = nullptr;
    context.status = "Live: breakpoint " + std::to_string(liveBreakpoint_) + " at " + liveSource_.value("url", std::string()) + ":" +
                     std::to_string(line);
}

void AiInvestigationWorkspace::LiveDetach(UiContext& context) {
    if (liveSession_.empty()) return;
    DesktopLive().Detach(liveSession_);
    liveSession_.clear();
    liveBreakpoint_ = 0;
    context.status = "Live: detached";
}

void AiInvestigationWorkspace::DrawLive(UiContext& context) {
    HintText("A Node.js or Electron inspector on 127.0.0.1 (node --inspect=127.0.0.1:PORT, Electron --inspect or "
             "--remote-debugging-port). Cortex lists scripts and console output and sets LOG breakpoints: each hit records "
             "the stack and locals, then the program resumes at once. Nothing is evaluated in the program.");
    auto session = DesktopLive().Get(liveSession_);
    static const char* protocols[] = {"CDP (Node.js, Electron)", "DAP: debugpy (Python)", "DAP: netcoredbg (.NET)", "JDWP (Java)"};
    ImGui::SetNextItemWidth(Px(220));
    ImGui::Combo("##LiveProtocol", &liveProtocol_, protocols, IM_ARRAYSIZE(protocols));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(120));
    ImGui::InputInt("Port", &livePort_);
    if (liveProtocol_ == 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(120));
        ImGui::InputInt("Process id", &livePid_);
    }
    ImGui::SameLine();
    if (ImGui::Button(session ? "Reattach" : "Attach")) {
        if (liveProtocol_ == 0) LiveAttach(context, livePort_);
        else if (liveProtocol_ == 3) LiveAttachDap(context, livePort_, "jdwp", 0);
        else LiveAttachDap(context, livePort_, liveProtocol_ == 1 ? "debugpy" : "netcoredbg", livePid_);
    }
    if (session) {
        ImGui::SameLine();
        if (ImGui::Button("Detach")) {
            LiveDetach(context);
            return;
        }
    }
    if (!session) return;
    const double now = ImGui::GetTime();
    if (now - livePolled_ > 0.5) {
        livePolled_ = now;
        liveStatus_ = session->Status();
        liveScripts_ = session->Scripts(liveFilter_.data(), 0, 500);
        if (liveBreakpoint_) liveHits_ = session->Hits(liveBreakpoint_, 0, 200);
    }
    if (!liveStatus_.is_object()) liveStatus_ = json::object();
    const json target = liveStatus_.contains("target") && liveStatus_["target"].is_object() ? liveStatus_["target"] : json::object();
    ImGui::Text("%s  %s: %zu scripts, %llu console messages, %llu foreign pauses resumed%s",
                target.value("type", std::string()).c_str(), target.value("title", std::string()).c_str(),
                liveStatus_.value("scripts", size_t{0}), static_cast<unsigned long long>(liveStatus_.value("console_messages", uint64_t{0})),
                static_cast<unsigned long long>(liveStatus_.value("foreign_pauses", uint64_t{0})),
                liveStatus_.value("connected", false) ? "" : "  (disconnected)");
    const float listWidth = std::max(Px(260), ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::BeginChild("LiveScripts", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    if (session->Protocol() == "dap" || session->Protocol() == "jdwp") {
        ImGui::TextWrapped(session->Protocol() == "dap" ? "DAP: give the source file path for a breakpoint."
                                                        : "JDWP: give a loaded class name (pkg.Class) for a breakpoint.");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##LivePath", "source file path", livePath_.data(), livePath_.size());
        if (ImGui::Button("Use this file")) LiveOpenScript(context, livePath_.data());
    }
    ImGui::SetNextItemWidth(-1);
    if (session->Protocol() == "cdp")
        ImGui::InputTextWithHint("##LiveFilter", "filter scripts (URL)", liveFilter_.data(), liveFilter_.size());
    const json scripts = liveScripts_.is_object() ? liveScripts_.value("scripts", json::array()) : json::array();
    for (const auto& script : scripts) {
        const std::string url = script.value("url", std::string());
        if (url.empty()) continue;
        const bool selected = liveSource_.is_object() && liveSource_.value("url", std::string()) == url;
        if (ImGui::Selectable(url.c_str(), selected)) LiveOpenScript(context, url);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("LiveDetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (liveSource_.is_object() && liveSource_.value("ok", false)) {
        ImGui::TextUnformatted(liveSource_.value("url", std::string()).c_str());
        ImGui::SetNextItemWidth(Px(100));
        ImGui::InputInt("Line", &liveLine_);
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::Button("Log breakpoint")) LiveBreakpoint(context, liveLine_);
        ImGui::EndDisabled();
        if (!context.mutationAllowed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Needs write permission: a hit pauses the program briefly.");
        const float sourceHeight = ImGui::GetContentRegionAvail().y * (liveBreakpoint_ ? 0.5f : 1.0f);
        std::string text = liveSource_.value("text", std::string());
        ImGui::InputTextMultiline("##LiveSource", text.data(), text.size() + 1, ImVec2(-1, sourceHeight), ImGuiInputTextFlags_ReadOnly);
        if (liveBreakpoint_ && liveHits_.is_object()) {
            ImGui::Text("Breakpoint %d: %d hit(s)%s", liveBreakpoint_, liveHits_.value("hits", 0),
                        liveHits_.value("active", false) ? "" : " (removed after its hit limit)");
            ImGui::BeginChild("LiveHits", ImVec2(0, 0), ImGuiChildFlags_None);
            const json records = liveHits_.value("records", json::array());
            for (const auto& record : records) {
                if (!record.is_object()) continue;
                const json frames = record.value("frames", json::array());
                if (frames.empty() || !frames[0].is_object()) continue;
                const json& top = frames[0];
                std::string locals;
                const json frameLocals = top.value("locals", json::object());
                for (const auto& [name, value] : frameLocals.items()) {
                    if (!value.is_object()) continue;
                    locals += (locals.empty() ? "" : ", ") + name + "=" +
                              (value.contains("value") ? value["value"].dump() : value.value("description", value.value("type", std::string())));
                }
                ImGui::BulletText("#%d %s %s:%d  %s", record.value("hit", 0), top.value("function", std::string()).c_str(),
                                  top.value("url", top.value("path", std::string())).c_str(), top.value("line", 0), locals.c_str());
            }
            ImGui::EndChild();
        }
    } else {
        ImGui::TextDisabled("Select a script.");
    }
    ImGui::EndChild();
}

} // namespace cortex::ui

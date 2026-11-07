// Setup (AI clients, analysis engines) and updates in the desktop.
//
// Both reuse core/setup, the code behind `cortex setup` and `cortex update`:
// the desktop shows what was detected, the person ticks what to change, and
// nothing is written or installed before they click.

#include "desktop_app.h"
#include "ui/desktop_stores.h"
#include "ui/widgets.h"
#include "../core/version.h"

#include <ctime>
#include <thread>

namespace cortex::desktop {
namespace {

namespace fs = std::filesystem;
using cortex::ui::Px;

fs::path SelfExe() { return fs::u8path(ExecutableDirectory()) / "cortex.exe"; }

void OpenUrl(const std::string& url) {
    const std::wstring wide = fs::u8path(url).wstring();
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const char* ClientStateText(const std::string& state) {
    if (state == "configured") return "configured";
    if (state == "outdated") return "registered with other settings";
    if (state == "not_configured") return "not configured";
    if (state == "unreadable") return "not plain JSON: add by hand";
    return "not installed";
}

void Detect(const std::shared_ptr<SetupPanelState>& panel) {
    {
        std::lock_guard<std::mutex> lock(panel->mutex);
        if (panel->loading) return;
        panel->loading = true;
    }
    std::thread([panel]() {
        auto environment = setup::CurrentEnvironment(SelfExe());
        auto engines = setup::DetectEngines(environment);
        setup::ClientOptions options;
        {
            std::lock_guard<std::mutex> lock(panel->mutex);
            options.allowWrites = panel->allowWrites;
        }
        for (const auto& report : engines)
            if (!report.found.empty()) options.engines[report.id] = report.found.front().path.u8string();
        auto clients = setup::DetectClients(environment, options);
        std::lock_guard<std::mutex> lock(panel->mutex);
        panel->environment = std::move(environment);
        panel->engines = std::move(engines);
        panel->clients = std::move(clients);
        for (const auto& client : panel->clients) {
            if (!panel->selected.count(client.id))
                panel->selected[client.id] = client.installed && client.state != "configured" && client.state != "unreadable";
        }
        for (const auto& report : panel->engines)
            if (!panel->engineChoice.count(report.id)) panel->engineChoice[report.id] = report.found.empty() ? -1 : 0;
        panel->loading = false;
        panel->loaded = true;
    }).detach();
}

setup::ClientOptions OptionsFrom(SetupPanelState& panel) {
    setup::ClientOptions options;
    options.allowWrites = panel.allowWrites;
    if (!panel.passEngines) return options;
    for (const auto& report : panel.engines) {
        const auto it = panel.engineChoice.find(report.id);
        if (it == panel.engineChoice.end() || it->second < 0 || it->second >= static_cast<int>(report.found.size())) continue;
        options.engines[report.id] = report.found[static_cast<size_t>(it->second)].path.u8string();
    }
    return options;
}

void Apply(AppState& app, bool remove) {
    auto panel = app.setupPanel;
    std::vector<std::string> log;
    setup::ClientOptions options;
    setup::Environment environment;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(panel->mutex);
        options = OptionsFrom(*panel);
        environment = panel->environment;
        for (const auto& client : panel->clients)
            if (client.installed && panel->selected[client.id]) ids.push_back(client.id);
    }
    for (const auto& id : ids) {
        const auto result = remove ? setup::RemoveClient(environment, id, false) : setup::ConfigureClient(environment, id, options, false);
        log.push_back(std::string(result.ok ? "ok    " : "FAIL  ") + result.message +
                      (result.backup.empty() ? "" : "  (backup: " + result.backup.filename().u8string() + ")"));
    }
    if (!remove && !options.engines.empty()) {
        auto value = [&options](const char* id) {
            const auto it = options.engines.find(id);
            return it == options.engines.end() ? std::string() : it->second;
        };
        const auto current = cortex::ui::DesktopEngineSettings();
        auto keep = [](const std::string& set, const std::string& found) { return set.empty() ? found : set; };
        std::string error;
        const bool saved = cortex::ui::ConfigureDesktopAnalysis(keep(current.ghidraDir, value("ghidra")),
                                                                keep(current.ilspycmd, value("ilspycmd")),
                                                                keep(current.dotnet, value("dotnet")),
                                                                keep(current.vineflowerJar, value("vineflower")), error,
                                                                value("java"));
        log.push_back(saved ? "ok    engines saved for the desktop (Artifacts > Engines)" : "FAIL  engines not saved: " + error);
    }
    if (ids.empty() && (remove || options.engines.empty())) log.push_back("Nothing selected.");
    else if (!remove && !ids.empty()) log.push_back("Restart the AI clients to load Cortex.");
    {
        std::lock_guard<std::mutex> lock(panel->mutex);
        panel->log = log;
        panel->scrollToLog = true;
        panel->selected.clear();
    }
    app.ui.status = remove ? "Cortex removed from the selected AI clients" : "Setup applied";
    Detect(panel);
}

}  // namespace

void OpenSetupPanel(AppState& app) {
    app.requestSetupPanel = true;
    Detect(app.setupPanel);
}

void DrawSetupPanel(AppState& app) {
    if (app.requestSetupPanel) {
        ImGui::OpenPopup("Set up Cortex");
        app.requestSetupPanel = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Px(820), Px(560)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Set up Cortex", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    auto panel = app.setupPanel;
    std::unique_lock<std::mutex> lock(panel->mutex);

    cortex::ui::HintText("Registers Cortex as an MCP server in the AI clients installed for you, and finds the analysis "
                         "engines. Nothing is written until you click Apply; every changed file is backed up beside itself.");
    if (panel->loading || !panel->loaded) {
        ImGui::Spacing();
        ImGui::TextDisabled("Looking for AI clients and engines...");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        lock.unlock();
        ImGui::EndPopup();
        return;
    }

    // The scrolling body; the buttons stay at the bottom.
    ImGui::BeginChild("SetupBody", ImVec2(0, -Px(46)), ImGuiChildFlags_None);
    ImGui::SeparatorText("AI clients");
    if (cortex::ui::BeginDataTable("SetupClients", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthFixed, Px(220));
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, Px(230));
        ImGui::TableSetupColumn("Configuration file", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& client : panel->clients) {
            if (!client.installed) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool ticked = panel->selected[client.id];
            ImGui::BeginDisabled(!client.installed || client.state == "unreadable");
            if (ImGui::Checkbox((client.name + "##client").c_str(), &ticked)) panel->selected[client.id] = ticked;
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(1);
            if (client.state == "configured") ImGui::TextColored(ImVec4(0.35f, 0.78f, 0.45f, 1.0f), "configured");
            else ImGui::TextDisabled("%s", ClientStateText(client.state));
            if (!client.detail.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", client.detail.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", client.installed ? client.configPath.u8string().c_str() : "-");
        }
        ImGui::EndTable();
    }
    {
        std::string missing;
        for (const auto& client : panel->clients)
            if (!client.installed) missing += (missing.empty() ? "" : ", ") + client.name;
        if (!missing.empty()) cortex::ui::HintText(("Not found on this computer: " + missing + ".").c_str());
    }
    if (ImGui::Checkbox("Let agents change targets (--allow-writes)", &panel->allowWrites)) {
        lock.unlock();
        Detect(panel);
        lock.lock();
    }
    if (panel->allowWrites)
        ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f),
                           "Agents will be able to write memory and set breakpoints when they ask with mutation_permission.");
    else
        cortex::ui::HintText("Read-only by default: agents observe; every change is refused.");

    ImGui::SeparatorText("Analysis engines");
    if (cortex::ui::BeginDataTable("SetupEngines", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthFixed, Px(170));
        ImGui::TableSetupColumn("Found", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(110));
        ImGui::TableHeadersRow();
        for (const auto& report : panel->engines) {
            ImGui::PushID(report.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(report.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", report.purpose.c_str());
            ImGui::TableSetColumnIndex(1);
            int& choice = panel->engineChoice[report.id];
            if (report.found.empty()) {
                ImGui::TextDisabled("not found");
            } else {
                auto label = [&report](int index) {
                    if (index < 0) return std::string("(do not use)");
                    const auto& item = report.found[static_cast<size_t>(index)];
                    return item.path.u8string() + (item.version.empty() ? "" : "  (" + item.version + ")");
                };
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##found", label(choice).c_str())) {
                    for (int i = -1; i < static_cast<int>(report.found.size()); ++i)
                        if (ImGui::Selectable(label(i).c_str(), i == choice)) choice = i;
                    ImGui::EndCombo();
                }
            }
            ImGui::TableSetColumnIndex(2);
            if (report.found.empty()) {
                if (report.download.rfind("http", 0) == 0) {
                    if (ImGui::SmallButton("Download")) OpenUrl(report.download);
                } else {
                    if (ImGui::SmallButton("Copy command")) ImGui::SetClipboardText(report.download.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", report.download.c_str());
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Checkbox("Give the engines to the MCP server and save them for the desktop", &panel->passEngines);
    for (const auto& line : panel->log) ImGui::TextUnformatted(line.c_str());
    if (panel->scrollToLog) {
        ImGui::SetScrollHereY(1.0f);
        panel->scrollToLog = false;
    }
    ImGui::EndChild();

    const bool any = std::any_of(panel->selected.begin(), panel->selected.end(), [](const auto& item) { return item.second; });
    lock.unlock();
    if (ImGui::Button("Apply", ImVec2(Px(140), Px(32)))) Apply(app, false);
    ImGui::SameLine();
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Remove Cortex from the ticked clients", ImVec2(0, Px(32)))) Apply(app, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Search again", ImVec2(0, Px(32)))) Detect(panel);
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(Px(100), Px(32)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ------------------------------------------------------------------ updates

namespace {

std::string Feed() {
    const wchar_t* feed = _wgetenv(L"CORTEX_UPDATE_FEED");
    return feed && *feed ? fs::path(feed).u8string() : std::string();
}

void RunUpdate(AppState& app) {
    auto update = app.update;
    setup::Release release;
    {
        std::lock_guard<std::mutex> lock(update->mutex);
        if (update->working || !update->result.available) return;
        update->working = true;
        update->outcome.clear();
        update->progress = "Downloading...";
        release = update->result.latest;
    }
    const fs::path folder = fs::u8path(ExecutableDirectory());
    const fs::path work = cortex::ui::DesktopDataDirectory() / "updates";
    std::thread([update, release, folder, work]() {
        const auto kind = setup::DetectInstallKind(folder);
        const auto prepared = setup::PrepareUpdate(release, kind, work, [update](uint64_t done, uint64_t total) {
            std::lock_guard<std::mutex> lock(update->mutex);
            update->progress = "Downloading " + std::to_string(done >> 20) + " / " + std::to_string(total >> 20) + " MB";
        });
        std::string outcome;
        std::string newFolder;
        bool quit = false;
        if (!prepared.ok) {
            outcome = "The update was not installed: " + prepared.error;
        } else {
            // This desktop is closed by the installer; anything else that
            // holds a file of this folder (an MCP server, a target with the
            // runtime) must be closed first.
            std::vector<setup::BlockingProcess> blocking;
            if (kind == setup::InstallKind::Installed) blocking = setup::FindBlockingProcesses(folder);
            if (!blocking.empty()) {
                outcome = "Verified, but these programs use files of Cortex; close them (or detach) and try again:";
                for (const auto& item : blocking)
                    outcome += "\n  " + item.name + " (" + std::to_string(item.pid) + "): " + item.reason;
            } else {
                const auto applied = setup::ApplyUpdate(prepared, folder, release.version);
                if (!applied.ok) outcome = "The update was not installed: " + applied.error;
                else if (applied.exitToFinish) {
                    outcome = "Verified (SHA-256 " + prepared.sha256.substr(0, 16) + "...). The installer is starting; Cortex closes.";
                    quit = true;
                } else {
                    newFolder = applied.newFolder.u8string();
                    outcome = "Cortex " + release.version + " is ready in " + applied.newFolder.u8string() +
                              " (verified, SHA-256 " + prepared.sha256.substr(0, 16) + "...). This folder is unchanged.";
                }
            }
        }
        std::lock_guard<std::mutex> lock(update->mutex);
        update->working = false;
        update->progress.clear();
        update->outcome = outcome;
        update->newFolder = newFolder;
        update->quitRequested = quit;
    }).detach();
}

}  // namespace

void StartUpdateCheck(AppState& app, bool manual) {
    auto update = app.update;
    {
        std::lock_guard<std::mutex> lock(update->mutex);
        if (update->checking) return;
        update->checking = true;
        update->manual = manual;
    }
    if (app.allowUpdateCheck) {  // test modes leave the person's settings alone
        app.settings.Values().lastUpdateCheck = static_cast<long long>(std::time(nullptr));
        app.settings.Save();
    }
    if (manual) app.requestUpdateDialog = true;
    const std::string feed = Feed();
    std::thread([update, feed]() {
        auto result = setup::CheckForUpdate(feed, cortex::kVersion);
        std::lock_guard<std::mutex> lock(update->mutex);
        update->result = std::move(result);
        update->checking = false;
        update->checked = true;
    }).detach();
}

void MaybeCheckForUpdates(AppState& app) {
    static bool started = false;
    if (started || !app.allowUpdateCheck) return;
    started = true;
    const auto& values = app.settings.Values();
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!values.updateCheckAtStartup || now - values.lastUpdateCheck < 24 * 3600) return;
    StartUpdateCheck(app, false);
}

void DrawUpdateButton(AppState& app) {
    std::string version;
    {
        std::lock_guard<std::mutex> lock(app.update->mutex);
        if (!app.update->checked || !app.update->result.ok || !app.update->result.available) return;
        version = app.update->result.latest.version;
    }
    if (version == app.settings.Values().skippedUpdate) return;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.45f, 0.30f, 1.0f));
    if (ImGui::Button(("Update to " + version).c_str())) app.requestUpdateDialog = true;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cortex %s is available", version.c_str());
}

void DrawUpdateDialog(AppState& app) {
    if (app.requestUpdateDialog) {
        ImGui::OpenPopup("Updates");
        app.requestUpdateDialog = false;
    }
    {
        // Close Cortex once the installer has started (it waits for us).
        std::lock_guard<std::mutex> lock(app.update->mutex);
        if (app.update->quitRequested && app.window) {
            app.update->quitRequested = false;
            PostMessageW(static_cast<HWND>(app.window), WM_CLOSE, 0, 0);
        }
    }
    ImGui::SetNextWindowSize(ImVec2(Px(680), Px(520)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Updates", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    std::unique_lock<std::mutex> lock(app.update->mutex);
    const auto& state = *app.update;
    const auto kind = setup::DetectInstallKind(fs::u8path(ExecutableDirectory()));
    ImGui::Text("This copy: Cortex %s (%s)", cortex::kVersion, setup::InstallKindName(kind));
    if (state.checking) {
        ImGui::TextDisabled("Checking the latest release...");
    } else if (!state.checked) {
        ImGui::TextDisabled("Not checked yet.");
    } else if (!state.result.ok) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "The check failed: %s", state.result.error.c_str());
    } else if (!state.result.available) {
        ImGui::TextColored(ImVec4(0.35f, 0.78f, 0.45f, 1.0f), "Cortex is up to date (latest release %s).",
                           state.result.latest.version.c_str());
    } else {
        const auto& latest = state.result.latest;
        {
            cortex::ui::HeadingFont heading;
            ImGui::Text("Cortex %s is available", latest.version.c_str());
        }
        cortex::ui::HintText(kind == setup::InstallKind::Installed
                                 ? "The installer is downloaded, checked against the release's SHA-256, then run; Cortex "
                                   "closes and reopens. Your settings and records are kept."
                                 : "The portable archive is downloaded, checked against the release's SHA-256 and unpacked "
                                   "into a new folder beside this one, which is kept. Settings are shared.");
        ImGui::BeginChild("ReleaseNotes", ImVec2(0, Px(260)), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", latest.notes.empty() ? "(no release notes)" : latest.notes.c_str());
        ImGui::EndChild();
        if (!state.progress.empty()) ImGui::TextDisabled("%s", state.progress.c_str());
        ImGui::BeginDisabled(state.working);
        const bool install = ImGui::Button(kind == setup::InstallKind::Installed ? "Download and install" : "Download and unpack",
                                           ImVec2(0, Px(32)));
        ImGui::SameLine();
        const bool skip = ImGui::Button("Skip this version", ImVec2(0, Px(32)));
        ImGui::SameLine();
        const bool page = ImGui::Button("Release page", ImVec2(0, Px(32)));
        ImGui::EndDisabled();
        const std::string version = latest.version;
        const std::string url = latest.page;
        if (page) OpenUrl(url);
        if (skip) {
            lock.unlock();
            app.settings.Values().skippedUpdate = version;
            app.settings.Save();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        if (install) {
            lock.unlock();
            RunUpdate(app);
            lock.lock();
        }
    }
    if (!app.update->outcome.empty()) ImGui::TextWrapped("%s", app.update->outcome.c_str());
    if (!app.update->newFolder.empty()) {
        const fs::path folder = fs::u8path(app.update->newFolder);
        if (ImGui::Button("Start the new version and close this one")) {
            const std::wstring exe = (folder / "cortex.exe").wstring();
            const std::wstring dir = folder.wstring();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL)) > 32 &&
                app.window)
                PostMessageW(static_cast<HWND>(app.window), WM_CLOSE, 0, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open its folder")) OpenUrl(folder.u8string());
    }
    const bool busy = app.update->checking || app.update->working;
    lock.unlock();
    ImGui::Spacing();
    bool atStartup = app.settings.Values().updateCheckAtStartup;
    if (ImGui::Checkbox("Check for updates when Cortex starts (once a day)", &atStartup)) {
        app.settings.Values().updateCheckAtStartup = atStartup;
        app.settings.Save();
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Check again")) StartUpdateCheck(app, true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace cortex::desktop

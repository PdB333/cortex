#include "lua_workspace.h"
#include "address_resolver.h"
#include "widgets.h"
#include "file_dialog.h"

#include "application/lua_engine.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cortex::ui {
namespace {

struct Example {
    const char* title;
    const char* source;
};

const Example kExamples[] = {
    {"Read values",
     "-- Addresses: numbers, \"module+offset\" or [pointer]+offset expressions.\n"
     "local base = getAddress(\"game.exe+1A2B0\")\n"
     "print(string.format(\"%X\", base), readInteger(base), readFloat(base + 4))\n"},
    {"Follow a pointer chain",
     "-- [x] reads the pointer stored at x, like Cheat Engine.\n"
     "local health = readFloat(\"[[game.exe+10F4F4]+30]+EC\")\n"
     "print(\"health\", health)\n"},
    {"Find a byte pattern",
     "local results = AOBScan(\"48 8B 05 ?? ?? ?? ?? 48 85 C0\")\n"
     "if results then\n"
     "  for i = 0, results.Count - 1 do print(results[i]) end\n"
     "  results.destroy()\n"
     "else\n"
     "  print(\"not found\")\n"
     "end\n"},
    {"List modules",
     "for _, module in ipairs(enumModules()) do\n"
     "  print(module.Name, string.format(\"%X\", module.Address), module.Size)\n"
     "end\n"},
    {"Keep a value (needs Writes allowed)",
     "-- Writes every 100 ms for 10 seconds; Stop ends it early.\n"
     "for i = 1, 100 do\n"
     "  writeInteger(\"game.exe+1A2B0\", 999)\n"
     "  sleep(100)\n"
     "end\n"},
    {"Patch bytes (needs Writes allowed)",
     "local address = AOBScanUnique(\"29 83 ?? ?? ?? ?? 8B\")\n"
     "if address then\n"
     "  writeBytes(address, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90)\n"
     "  print(string.format(\"patched at %X\", address))\n"
     "end\n"},
};

const char kDefaultSource[] =
    "-- Cortex Lua engine, Cheat Engine compatible functions.\n"
    "-- Press Execute (Ctrl+Enter). Writes need Writes allowed.\n"
    "print(getProcessName(), getProcessId(), targetIs64Bit() and \"x64\" or \"x86\")\n"
    "for i, module in ipairs(enumModules()) do\n"
    "  if i > 5 then break end\n"
    "  print(module.Name, string.format(\"%X\", module.Address))\n"
    "end\n";

} // namespace

void LuaWorkspace::Poll(UiContext& context) {
    if (!running_ || !run_.valid()) return;
    if (run_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    status_ = run_.get();
    running_ = false;
    cancel_.reset();
    context.status = "Lua: " + status_;
    scrollToEnd_ = true;
}

void LuaWorkspace::Execute(UiContext& context) {
    if (running_) return;
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session) {
        status_ = "Select a process first";
        return;
    }
    static const int limits[] = {10000, 60000, 0};
    application::LuaRunOptions options;
    options.session = session;
    options.allowWrites = context.mutationAllowed;
    options.timeoutMs = limits[std::clamp(limitIndex_, 0, 2)];
    cancel_ = std::make_shared<std::atomic_bool>(false);
    auto cancel = cancel_;
    auto output = output_;
    options.output = [output](const std::string& line) {
        std::lock_guard<std::mutex> lock(output->mutex);
        output->lines.push_back(line);
        if (output->lines.size() > 5000) output->lines.erase(output->lines.begin(), output->lines.begin() + 1000);
    };
    options.exports = MakeExportReader(session);
    options.userSymbols = context.userSymbols;
    auto* modules = context.modules;
    options.modules = [modules]() {
        std::string error;
        return modules ? modules->List(&error) : std::vector<target::ModuleInfo>();
    };
    const std::string source = source_.data();
    {
        std::lock_guard<std::mutex> lock(output->mutex);
        output->lines.push_back("> running on " + session->Target().name + (options.allowWrites ? "" : " (read-only)"));
    }
    running_ = true;
    status_ = "running...";
    run_ = std::async(std::launch::async, [source, options, cancel, output]() mutable {
        options.cancelled = cancel.get();
        const auto result = application::RunDesktopLua(source, options);
        char timing[48] = {};
        std::snprintf(timing, sizeof(timing), " in %.0f ms", result.milliseconds);
        std::lock_guard<std::mutex> lock(output->mutex);
        if (!result.ok) {
            output->lines.push_back("error: " + result.error);
            return std::string("error: ") + result.error;
        }
        if (!result.returned.empty()) output->lines.push_back("= " + result.returned);
        return std::string("finished") + timing;
    });
}

// A cheat table's script lands in the editor; the user reviews and runs it.
void LuaWorkspace::Tick(UiContext& context) {
    if (context.luaScriptRequest.empty() || running_) return;
    if (context.luaScriptRequest.size() >= source_.size()) {
        context.status = "The table's Lua script is too large for the editor";
    } else {
        std::snprintf(source_.data(), source_.size(), "%s", context.luaScriptRequest.c_str());
        initialized_ = true;
        file_.clear();
        status_ = "Loaded the cheat table's Lua script: review it, then Execute";
    }
    context.luaScriptRequest.clear();
}

void LuaWorkspace::Draw(UiContext& context) {
    Poll(context);
    if (!initialized_) {
        std::snprintf(source_.data(), source_.size(), "%s", kDefaultSource);
        initialized_ = true;
    }

    ImGui::BeginDisabled(running_);
    if (ImGui::Button("Execute")) Execute(context);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Enter in the editor");
    ImGui::SameLine();
    ImGui::BeginDisabled(!running_);
    if (ImGui::Button("Stop") && cancel_) cancel_->store(true);
    ImGui::EndDisabled();
    FlowSameLine(Px(120));
    ImGui::SetNextItemWidth(Px(120));
    const char* limits[] = {"10 s limit", "60 s limit", "No limit"};
    ImGui::Combo("##LuaLimit", &limitIndex_, limits, IM_ARRAYSIZE(limits));
    FlowSameLine(Px(200));
    ImGui::SetNextItemWidth(Px(200));
    if (ImGui::BeginCombo("##LuaExamples", "Insert an example")) {
        for (const auto& example : kExamples) {
            if (ImGui::Selectable(example.title))
                std::snprintf(source_.data(), source_.size(), "%s", example.source);
        }
        ImGui::EndCombo();
    }
    FlowSameLine(ButtonWidth("Open..."));
    if (ImGui::Button("Open...")) {
        std::string path = file_;
        if (ShowOpenFileDialog(L"Lua scripts (*.lua)\0*.lua\0All files\0*.*\0", path)) {
            std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
            std::ostringstream text;
            text << input.rdbuf();
            std::snprintf(source_.data(), source_.size(), "%s", text.str().c_str());
            file_ = path;
            status_ = "Opened " + path;
        }
    }
    FlowSameLine(ButtonWidth("Save..."));
    if (ImGui::Button("Save...")) {
        std::string path = file_;
        if (ShowSaveFileDialog(L"Lua scripts (*.lua)\0*.lua\0All files\0*.*\0", L"lua", path)) {
            std::ofstream output(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
            output << source_.data();
            file_ = path;
            status_ = output ? "Saved " + path : "Cannot write " + path;
        }
    }
    FlowSameLine(ButtonWidth("Clear output"));
    if (ImGui::Button("Clear output")) {
        std::lock_guard<std::mutex> lock(output_->mutex);
        output_->lines.clear();
    }
    if (!status_.empty()) {
        FlowSameLine(TextWidth(status_.c_str()));
        ImGui::AlignTextToFramePadding();
        if (status_.rfind("error", 0) == 0) ImGui::TextColored(WarningTextColor(), "%s", status_.c_str());
        else ImGui::TextDisabled("%s", status_.c_str());
    }

    const float available = ImGui::GetContentRegionAvail().y;
    const float editorHeight = std::max(Px(120), available * 0.58f);
    MonoInputTextMultiline("##LuaSource", source_.data(), source_.size(), ImVec2(-1, editorHeight),
                           ImGuiInputTextFlags_AllowTabInput);
    if (ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        Execute(context);

    ImGui::BeginChild("LuaOutput", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    {
        MonoFont mono;
        std::lock_guard<std::mutex> lock(output_->mutex);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(output_->lines.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& line = output_->lines[static_cast<size_t>(i)];
                if (line.rfind("error:", 0) == 0) ImGui::TextColored(ChangedValueColor(), "%s", line.c_str());
                else if (line.rfind("> ", 0) == 0) ImGui::TextDisabled("%s", line.c_str());
                else ImGui::TextUnformatted(line.c_str());
            }
        }
        if (running_ || scrollToEnd_) {
            ImGui::SetScrollHereY(1.0f);
            scrollToEnd_ = false;
        }
    }
    ImGui::EndChild();

    std::string functions;
    for (const char* name : application::DesktopLuaFunctions()) {
        if (!functions.empty()) functions += ", ";
        functions += name;
    }
    ImGui::TextDisabled("Cheat Engine functions: %zu", application::DesktopLuaFunctions().size());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(Px(520));
        ImGui::TextUnformatted(functions.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace cortex::ui

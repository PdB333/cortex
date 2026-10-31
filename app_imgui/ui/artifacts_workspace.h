#pragma once

#include "workspace.h"

#include <nlohmann/json.hpp>

#include <array>
#include <memory>
#include <mutex>
#include <string>

namespace cortex::ui {

// Static analysis of files through the artifact providers (core/artifacts):
// PE, ASAR, and the runtimes added in later steps. Nothing needs a target
// process; the same providers back `cortex.exe inspect` and the
// cortex_artifact_* MCP tools.
class ArtifactsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "artifacts"; }
    const char* Title() const override { return "Artifacts"; }
    void Draw(UiContext& context) override;
    // "inspect_artifact <path>": inspects a file without the dialog;
    // "artifact_tab <name>" selects a result tab; "artifact_position L:C"
    // maps a generated position through the script's source map;
    // "artifact_cil <method>" disassembles .NET methods;
    // "artifact_functions", "artifact_function <location>" and
    // "artifact_rename <rva> <name>" drive the Code tab.
    void HandleCommand(UiContext& context, const std::string& command) override;

private:
    void Inspect(UiContext& context, const std::string& path, const std::string& child);
    void DrawResult(UiContext& context);
    void DrawChildren(UiContext& context);
    void DrawElectron(const nlohmann::json& data);
    void DrawModules(const nlohmann::json& modules);
    void DrawSourcePosition(UiContext& context);
    void LookupPosition(UiContext& context);
    void DrawDotNet(UiContext& context, const nlohmann::json& dotnet);
    void Disassemble(UiContext& context, const std::string& method);
    void DrawCode(UiContext& context);
    // `announce` false keeps the status line (a reload after a rename).
    void LoadFunctions(UiContext& context, bool announce = true);
    void OpenFunction(UiContext& context, const std::string& location, bool announce = true);
    void Rename(UiContext& context, const std::string& rva, const std::string& name);
    int TabFlags(const char* name) const;  // ImGuiTabItemFlags

    std::array<char, 1024> path_ = {};
    std::array<char, 512> child_ = {};
    // Written by the background inspection, read by the frame loop.
    std::mutex mutex_;
    nlohmann::json result_;
    nlohmann::json children_;
    std::string inspectedPath_;
    std::string inspectedChild_;
    std::array<char, 128> childFilter_ = {};
    int positionLine_ = 1;
    int positionColumn_ = 1;
    nlohmann::json position_;  // last source position answer
    nlohmann::json cil_;       // last CIL disassembly
    std::array<char, 128> typeFilter_ = {};
    nlohmann::json functions_;     // native function catalog of the inspected PE
    nlohmann::json codeDetail_;    // {"cfg", "xrefs"} of the opened function
    std::array<char, 128> codeFilter_ = {};
    std::array<char, 128> rename_ = {};
    std::string selectTab_;    // "artifact_tab <name>": selected on the next frame
};

} // namespace cortex::ui

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
    // "artifact_rename <rva> <name>" drive the Code tab;
    // "artifact_engine_start <ghidra|ilspy>", "artifact_engine_cancel",
    // "artifact_engine_open <rva|path>" and "artifact_engines_refresh" drive
    // the Engines tab; "artifact_evidence" reloads the Evidence tab; "artifact_open
    // <path>|<rva>", "artifact_show_live" and "artifact_record_function"
    // navigate between live and static code and record evidence.
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
    void DrawEvidence(UiContext& context);
    void LoadEvidence(UiContext& context);
    // Inspect `path` and open the function at `location` (module+RVA from
    // the live address menu), in one background task.
    void OpenAt(UiContext& context, const std::string& path, const std::string& location);
    // Disassemble base+RVA in the target when a loaded module is this file.
    void ShowLive(UiContext& context, const std::string& sha256, const std::string& fileName, const std::string& rva);
    void RecordFunctionEvidence(UiContext& context, const std::string& rva);
    // External engines (core/providers): Ghidra for PE, ILSpy for .NET.
    void DrawEngines(UiContext& context);
    void StartEngine(UiContext& context, const std::string& provider);
    void PollEngine(UiContext& context, bool force = false);
    void OpenEngineItem(UiContext& context, const std::string& key);  // an RVA (Ghidra) or a file path (ILSpy)
    void RefreshEngines(UiContext& context);

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
    nlohmann::json evidence_;      // evidence records about the inspected artifact
    std::string selectTab_;    // "artifact_tab <name>": selected on the next frame
    // Engine job of the inspected file; guarded by mutex_.
    std::string engineJob_;
    std::string engineProvider_;
    nlohmann::json engineState_;      // last cortex::providers::Service::Get (summary)
    nlohmann::json engineItems_;      // Ghidra functions or ILSpy files, texts clipped
    nlohmann::json engineDetail_;     // the opened decompiled function or file
    nlohmann::json engineProviders_;  // detected engines
    double enginePolled_ = 0;
    std::array<char, 1024> ghidraDir_ = {};
    std::array<char, 1024> ilspycmd_ = {};
    std::array<char, 1024> dotnetPath_ = {};
    std::array<char, 1024> vineflowerJar_ = {};
    std::string javaHome_;  // a JDK found by Find installed engines
    std::array<char, 128> engineFilter_ = {};
};

} // namespace cortex::ui

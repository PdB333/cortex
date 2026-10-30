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
    // "inspect_artifact <path>": inspects a file without the dialog.
    void HandleCommand(UiContext& context, const std::string& command) override;

private:
    void Inspect(UiContext& context, const std::string& path, const std::string& child);
    void DrawResult(UiContext& context);
    void DrawChildren(UiContext& context);

    std::array<char, 1024> path_ = {};
    std::array<char, 512> child_ = {};
    // Written by the background inspection, read by the frame loop.
    std::mutex mutex_;
    nlohmann::json result_;
    nlohmann::json children_;
    std::string inspectedPath_;
    std::string inspectedChild_;
    std::array<char, 128> childFilter_ = {};
};

} // namespace cortex::ui

#pragma once

#include "workspace.h"

#include <nlohmann/json.hpp>

#include <array>
#include <string>

namespace cortex::ui {

// The person's view of what agents do (roadmap step 7): investigations with
// their plan, approvals and journal; evidence records with navigation to
// the live process or the static code; the process tree of a
// multi-process target (Electron main/renderer/GPU/utility). The AI side
// is any MCP client; this workspace holds no API key and runs no agent.
class AiInvestigationWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "ai-investigation"; }
    const char* Title() const override { return "AI Investigation"; }
    void Draw(UiContext& context) override;
    // "ai_investigations_refresh", "ai_investigation_open <id>",
    // "ai_investigation_approve <id> <step>", "ai_investigation_reject <id>
    // <step>", "ai_evidence_refresh", "ai_evidence_live <id>",
    // "ai_evidence_static <id>", "ai_process_tree <pid>", "ai_tab <name>".
    void HandleCommand(UiContext& context, const std::string& command) override;

private:
    void DrawInvestigations(UiContext& context);
    void DrawEvidence(UiContext& context);
    void DrawProcessTree(UiContext& context);
    void Refresh(UiContext& context);
    void Open(UiContext& context, const std::string& id);
    void Decide(UiContext& context, const std::string& id, size_t step, bool approve);
    void RefreshEvidence(UiContext& context);
    void EvidenceLive(UiContext& context, const nlohmann::json& record);
    void EvidenceStatic(UiContext& context, const nlohmann::json& record);
    void LoadProcessTree(UiContext& context, uint64_t pid);
    int TabFlags(const char* name) const;

    nlohmann::json investigations_;
    nlohmann::json selected_;
    nlohmann::json evidence_;
    nlohmann::json tree_;
    std::array<char, 256> note_ = {};
    std::array<char, 128> evidenceFilter_ = {};
    int treePid_ = 0;
    std::string selectTab_;
};

} // namespace cortex::ui

#pragma once

#include "workspace.h"
#include "services/disassembly_service.h"

#include <string>
#include <vector>

namespace cortex::ui {

class DisassemblyWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "disassembly"; }
    const char* Title() const override { return "Disassembler"; }
    void Draw(UiContext& context) override;

private:
    bool ParseAddress(uint64_t& address) const;
    void Decode(UiContext& context, uint64_t address);
    void Analyze(UiContext& context, const char* tool,
                 const char* argumentKey, const char* kind,
                 bool includeData = false);

    char address_[64] = "0x0";
    int count_ = 160;
    uint64_t currentAddress_ = 0;
    std::vector<services::DisassemblyInstruction> instructions_;
    std::string analysisKind_;
    std::string analysisResult_;
    std::string analysisError_;
    std::string targetId_;
};

} // namespace cortex::ui

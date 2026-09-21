#pragma once

#include "workspace.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::ui {

class MemoryBrowserWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "memory-browser"; }
    const char* Title() const override { return "Memory viewer"; }
    void Draw(UiContext& context) override;

private:
    bool ParseAddress(uint64_t& address) const;
    void Navigate(uint64_t address);
    void Refresh(UiContext& context);
    bool WriteBytes(UiContext& context);

    char address_[64] = "0x0";
    char writeBytes_[512] = {};
    int byteCount_ = 256;
    uint64_t currentAddress_ = 0;
    std::vector<uint8_t> bytes_;
    std::string targetId_;
};

} // namespace cortex::ui

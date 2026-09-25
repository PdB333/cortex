#pragma once

#include "workspace.h"

#include <cstdint>
#include <string>

namespace cortex::ui {

class ScreenshotWorkspace final : public IWorkspace {
public:
    ~ScreenshotWorkspace() override;

    const char* Id() const override { return "screenshots"; }
    const char* Title() const override { return "Screenshots"; }
    void Draw(UiContext& context) override;

private:
    void ReleaseTexture();
    bool UpdateTexture(UiContext& context, std::string& error);

    std::string targetId_;
    int modeIndex_ = 0;
    uint64_t loadedGeneration_ = 0;
    void* textureView_ = nullptr;
    unsigned textureWidth_ = 0;
    unsigned textureHeight_ = 0;
};

} // namespace cortex::ui

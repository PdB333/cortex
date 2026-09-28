#pragma once

#include "runtime_model_base.h"

#include <cstdint>
#include <string>

namespace cortex::application {

class ScreenshotModel : public RuntimeModelBase {
public:
    explicit ScreenshotModel(services::RuntimeTransport& payload) : RuntimeModelBase(payload) {}

    void Reset();
    bool Capture(const std::string& mode, std::string* error = nullptr);

    const std::string& ImageBase64() const { return imageBase64_; }
    const std::string& Meta() const { return meta_; }
    uint64_t Generation() const { return generation_; }

private:

    std::string imageBase64_;
    std::string meta_;
    uint64_t generation_ = 0;
};

} // namespace cortex::application

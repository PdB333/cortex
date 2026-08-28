#pragma once

#include "backend.h"

namespace cortex::target {

class LocalBackend final : public Backend {
public:
    LocalBackend();

    NodeDescriptor Node() const override;
    std::vector<TargetDescriptor> ListTargets() override;

private:
    NodeDescriptor node_;
};

} // namespace cortex::target

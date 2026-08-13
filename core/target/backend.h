#pragma once

#include "model.h"
#include "node.h"

#include <vector>

namespace cortex::target {

class Backend {
public:
    virtual ~Backend() = default;

    virtual NodeDescriptor Node() const = 0;
    virtual std::vector<TargetDescriptor> ListTargets() = 0;
};

} // namespace cortex::target

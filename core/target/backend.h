#pragma once

#include "model.h"
#include "node.h"
#include "session.h"

#include <memory>
#include <string>
#include <vector>

namespace cortex::target {

class Backend {
public:
    virtual ~Backend() = default;

    virtual NodeDescriptor Node() const = 0;
    virtual std::vector<TargetDescriptor> ListTargets() = 0;
    virtual SessionPtr Attach(const TargetDescriptor&, std::string* error = nullptr) {
        if (error) *error = "attach_not_supported";
        return {};
    }
};

} // namespace cortex::target

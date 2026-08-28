#pragma once

#include "backend.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cortex::target {

class Catalog {
public:
    bool AddBackend(std::shared_ptr<Backend> backend) {
        if (!backend) return false;
        const auto node = backend->Node();
        if (!node.Valid()) return false;
        for (const auto& existing : backends_) {
            if (existing->Node().id == node.id) return false;
        }
        backends_.push_back(std::move(backend));
        return true;
    }

    std::vector<NodeDescriptor> Nodes() const {
        std::vector<NodeDescriptor> result;
        result.reserve(backends_.size());
        for (const auto& backend : backends_) result.push_back(backend->Node());
        return result;
    }

    std::vector<TargetDescriptor> Targets() {
        std::vector<TargetDescriptor> result;
        for (const auto& backend : backends_) {
            auto discovered = backend->ListTargets();
            result.insert(result.end(), discovered.begin(), discovered.end());
        }
        return result;
    }

    std::optional<TargetDescriptor> FindTarget(const std::string& id) {
        for (auto& target : Targets()) {
            if (target.id == id) return target;
        }
        return std::nullopt;
    }

    SessionPtr Attach(const TargetDescriptor& target, std::string* error = nullptr) {
        for (const auto& backend : backends_) {
            if (backend->Node().id == target.nodeId) return backend->Attach(target, error);
        }
        if (error) *error = "backend_not_found";
        return {};
    }

    size_t BackendCount() const { return backends_.size(); }

private:
    std::vector<std::shared_ptr<Backend>> backends_;
};

} // namespace cortex::target

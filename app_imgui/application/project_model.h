#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct ProjectAddress {
    std::string name;
    std::string address;
    std::string type;
    std::string notes;
};

struct ProjectPointerPath {
    std::string name;
    std::string module;
    std::string baseOffset;
    std::string offsetsJson = "[]";
    std::string finalType;
    std::string notes;
};

struct ProjectNote {
    int id = -1;
    std::string text;
    std::string tagsJson = "[]";
};

class ProjectModel {
public:
    explicit ProjectModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);

    const std::vector<ProjectAddress>& Addresses() const { return addresses_; }
    const std::vector<ProjectPointerPath>& PointerPaths() const { return pointerPaths_; }
    const std::vector<ProjectNote>& Notes() const { return notes_; }

    bool SetAddress(const std::string& name, const std::string& address,
                    const std::string& type, const std::string& notes,
                    bool mutationAllowed, std::string* error = nullptr);
    bool DeleteAddress(const std::string& name, bool mutationAllowed,
                       std::string* error = nullptr);

    bool SetPointerPath(const std::string& name, const std::string& module,
                        const std::string& baseOffset, const std::string& offsetsJson,
                        const std::string& finalType, const std::string& notes,
                        bool mutationAllowed, std::string* error = nullptr);
    bool DeletePointerPath(const std::string& name, bool mutationAllowed,
                           std::string* error = nullptr);
    bool ResolvePointerPath(const std::string& name, std::string& address,
                            std::string* error = nullptr);

    bool AddNote(const std::string& text, const std::string& tagsJson,
                 bool mutationAllowed, std::string* error = nullptr);
    bool DeleteNote(int id, bool mutationAllowed, std::string* error = nullptr);

    bool FindAddress(const std::string& name, std::string& address) const;

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    std::vector<ProjectAddress> addresses_;
    std::vector<ProjectPointerPath> pointerPaths_;
    std::vector<ProjectNote> notes_;
};

} // namespace cortex::application

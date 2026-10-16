#pragma once

#include "runtime_model_base.h"

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

struct ProjectKnowledgeNode {
    std::string id;
    std::string kind;
    std::string statement;
    std::string status;
    std::string origin;
    int revision = 0;
    int evidenceCount = 0;
};

struct ProjectKnowledgeRelation {
    std::string from;
    std::string to;
    std::string type;
    std::string reference;
    std::string origin;
    bool causalVerified = false;
};

class ProjectModel : public RuntimeModelBase {
public:
    explicit ProjectModel(services::RuntimeTransport& payload) : RuntimeModelBase(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);

    const std::vector<ProjectAddress>& Addresses() const { return addresses_; }
    const std::vector<ProjectPointerPath>& PointerPaths() const { return pointerPaths_; }
    const std::vector<ProjectNote>& Notes() const { return notes_; }
    // Read-only project knowledge graph. Graph assertions remain unverified.
    bool RefreshKnowledgeGraph(const std::string& focus, int depth, int limit,
                               std::string* error = nullptr);
    const std::vector<ProjectKnowledgeNode>& KnowledgeNodes() const { return knowledgeNodes_; }
    const std::vector<ProjectKnowledgeRelation>& KnowledgeRelations() const { return knowledgeRelations_; }
    bool KnowledgeGraphLoaded() const { return knowledgeGraphLoaded_; }

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

    std::vector<ProjectAddress> addresses_;
    std::vector<ProjectPointerPath> pointerPaths_;
    std::vector<ProjectNote> notes_;
    std::vector<ProjectKnowledgeNode> knowledgeNodes_;
    std::vector<ProjectKnowledgeRelation> knowledgeRelations_;
    bool knowledgeGraphLoaded_ = false;
};

} // namespace cortex::application

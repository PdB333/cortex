#include "target/session_manager.h"

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace cortex::target;

class FakeSession final : public Session {
public:
    FakeSession(TargetDescriptor target, std::shared_ptr<bool> alive)
        : target_(std::move(target)), alive_(std::move(alive)) {}

    const TargetDescriptor& Target() const override { return target_; }
    const CapabilitySet& Capabilities() const override { return target_.capabilities; }
    bool Alive() const override { return alive_ && *alive_; }
    bool ReadMemory(uint64_t, void*, size_t, size_t*) const override { return false; }
    bool WriteMemory(uint64_t, const void*, size_t, size_t*) override { return false; }
    std::vector<MemoryRegion> MemoryRegions() const override { return {}; }

private:
    TargetDescriptor target_;
    std::shared_ptr<bool> alive_;
};

class FakeBackend final : public Backend {
public:
    FakeBackend(NodeDescriptor node, std::vector<TargetDescriptor> targets)
        : node_(std::move(node)), targets_(std::move(targets)) {
        for (const auto& target : targets_) alive_[target.id] = std::make_shared<bool>(true);
    }

    NodeDescriptor Node() const override { return node_; }
    std::vector<TargetDescriptor> ListTargets() override { return targets_; }

    SessionPtr Attach(const TargetDescriptor& target, std::string* error) override {
        const auto found = alive_.find(target.id);
        if (found == alive_.end()) {
            if (error) *error = "target_not_found";
            return {};
        }
        if (error) error->clear();
        return std::make_shared<FakeSession>(target, found->second);
    }

    void SetAlive(const std::string& id, bool alive) {
        const auto found = alive_.find(id);
        if (found != alive_.end()) *found->second = alive;
    }

private:
    NodeDescriptor node_;
    std::vector<TargetDescriptor> targets_;
    std::unordered_map<std::string, std::shared_ptr<bool>> alive_;
};

TargetDescriptor MakeTarget(const NodeDescriptor& node, std::uint64_t pid,
                            const char* name, std::uint64_t generation = 0) {
    TargetDescriptor target;
    target.id = MakeProcessTargetId(node.id, node.platform, pid);
    target.nodeId = node.id;
    target.name = name;
    target.platform = node.platform;
    target.architecture = node.architecture;
    target.kind = TargetKind::Process;
    target.processId = pid;
    target.generation = generation;
    target.capabilities = CapabilitySet{Capability::ProcessInfo, Capability::MemoryRead};
    return target;
}

} // namespace

int main() {
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    const auto node = MakeLocalNode("session-test", "Session Test", Platform::Windows, Architecture::X64);
    const auto first = MakeTarget(node, 101, "first.exe", 1001);
    const auto second = MakeTarget(node, 202, "second.exe", 2001);
    auto backend = std::make_shared<FakeBackend>(node, std::vector<TargetDescriptor>{first, second});

    Catalog catalog;
    check(catalog.AddBackend(backend), "fake backend accepted");
    SessionManager sessions(catalog);
    std::string error;

    check(sessions.Attach(first, &error), "attach first target");
    check(sessions.SessionCount() == 1, "one session after first attach");
    check(sessions.ActiveTargetId() == first.id, "first target active");

    check(sessions.Attach(second, &error), "attach second target");
    check(sessions.SessionCount() == 2, "both sessions remain attached");
    check(sessions.ActiveTargetId() == second.id, "second target becomes active");
    check(static_cast<bool>(sessions.Find(first.id)), "first session is retained");

    check(sessions.Activate(first.id), "switch back to retained first session");
    check(sessions.ActiveTargetId() == first.id, "first target active after switch");
    check(sessions.SessionCount() == 2, "switching does not detach second target");

    check(sessions.Detach(second.id), "detach inactive second target");
    check(!sessions.HasSession(second.id), "second target removed");
    check(sessions.ActiveTargetId() == first.id, "detaching inactive target keeps active target");

    check(sessions.Attach(second, &error), "reattach second target");
    backend->SetAlive(second.id, false);
    sessions.PruneDeadSessions();
    check(!sessions.HasSession(second.id), "dead target pruned");
    check(sessions.SessionCount() == 1, "live target retained after prune");
    check(sessions.ActiveTargetId().empty(), "active id cleared when active target dies");
    check(sessions.Activate(first.id), "retained live target can become active again");

    // PID reuse must not alias an old live Session. Same target id + a different
    // process-lifetime generation replaces the stale session atomically.
    TargetDescriptor recycled = first;
    recycled.generation = 1002;
    check(sessions.Attach(recycled, &error), "attach recycled PID generation");
    check(sessions.SessionCount() == 1, "recycled PID replaces instead of duplicating session");
    const auto recycledSession = sessions.Active();
    check(recycledSession && recycledSession->Target().generation == 1002,
          "active session carries the new process generation");

    sessions.DetachAll();
    check(sessions.SessionCount() == 0, "detach all clears every target");
    check(!sessions.HasActiveSession(), "detach all clears active target");

    if (failures) return 1;
    std::cout << "PASS: multi-target session manager and generation replacement\n";
    return 0;
}

#include "../core/target/backend.h"
#include "../core/target/catalog.h"
#include "../core/target/model.h"
#include "../core/target/node.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class PortableBackend final : public cortex::target::Backend {
public:
    PortableBackend(cortex::target::NodeDescriptor node,
                    std::vector<cortex::target::TargetDescriptor> targets)
        : node_(std::move(node)), targets_(std::move(targets)) {}

    cortex::target::NodeDescriptor Node() const override { return node_; }
    std::vector<cortex::target::TargetDescriptor> ListTargets() override { return targets_; }

private:
    cortex::target::NodeDescriptor node_;
    std::vector<cortex::target::TargetDescriptor> targets_;
};

} // namespace

int main() {
    using namespace cortex::target;
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    check(std::string(PlatformName(Platform::Linux)) == "linux", "linux name");
    check(std::string(PlatformName(Platform::PS4)) == "ps4", "ps4 name");
    check(std::string(ArchitectureName(Architecture::Arm64)) == "arm64", "arm64 name");

    CapabilitySet base{Capability::ProcessInfo, Capability::Modules};
    CapabilitySet extra{Capability::Diagnostics};
    const auto combined = base.Unite(extra);
    check(combined.ContainsAll(base), "union contains base capabilities");
    check(combined.Has(Capability::Diagnostics), "union contains added capability");
    check(combined.Intersect(base).Raw() == base.Raw(), "intersection is deterministic");

    const auto node = MakeLocalNode("portable", "Portable", Platform::Linux, Architecture::Arm64);
    check(node.Valid(), "portable node is valid");
    check(node.transport == NodeTransport::Local, "portable node transport is local");

    TargetDescriptor target;
    target.id = MakeProcessTargetId(node.id, node.platform, 7);
    target.nodeId = node.id;
    target.name = "sample";
    target.platform = node.platform;
    target.architecture = node.architecture;
    target.kind = TargetKind::Process;
    target.processId = 7;
    target.capabilities = combined;
    check(target.Valid(), "portable target is valid");
    check(target.id == "portable:linux:process:7", "portable target id is stable");

    Catalog catalog;
    check(catalog.AddBackend(std::make_shared<PortableBackend>(node, std::vector<TargetDescriptor>{target})),
          "portable backend is accepted");
    check(catalog.Nodes().size() == 1, "portable catalog exposes node");
    check(catalog.Targets().size() == 1, "portable catalog exposes target");
    const auto found = catalog.FindTarget(target.id);
    check(found.has_value() && found->architecture == Architecture::Arm64,
          "portable target lookup preserves architecture");

    if (failures) return 1;
    std::cout << "PASS: portable target model\n";
    return 0;
}

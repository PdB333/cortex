#include "../core/target/model.h"
#include "../core/target/node.h"
#include "../core/target/windows_local.h"

#include <windows.h>

#include <iostream>
#include <string>

int main() {
    using namespace cortex::target;
    int failures = 0;
    auto check = [&](bool value, const char* message) {
        if (!value) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    CapabilitySet set{Capability::ProcessInfo, Capability::Modules, Capability::Diagnostics};
    check(set.Has(Capability::ProcessInfo), "process info present");
    check(set.Has(Capability::Modules), "modules present");
    check(set.Has(Capability::Diagnostics), "diagnostics present");
    check(set.Names().size() == 3, "stable capability count");
    check(std::string(PlatformName(Platform::Windows)) == "windows", "windows platform name");
    check(std::string(PlatformName(Platform::Linux)) == "linux", "linux platform name");
    check(std::string(PlatformName(Platform::PS4)) == "ps4", "ps4 platform name");

    const auto node = MakeLocalNode("node-a", "Desktop", Platform::Windows, Architecture::X64);
    check(node.Valid(), "local node descriptor is valid");
    check(node.online, "local node starts online");
    check(node.transport == NodeTransport::Local, "local node transport is explicit");
    check(std::string(NodeTransportName(node.transport)) == "local", "transport name is stable");

    check(MakeProcessTargetId(node.id, node.platform, 42) == "node-a:windows:process:42",
          "stable target id");

    TargetDescriptor descriptor;
    descriptor.id = MakeProcessTargetId(node.id, node.platform, 42);
    descriptor.nodeId = node.id;
    descriptor.name = "sample";
    descriptor.platform = node.platform;
    descriptor.architecture = node.architecture;
    descriptor.kind = TargetKind::Process;
    descriptor.processId = 42;
    descriptor.capabilities = set;
    check(descriptor.Valid(), "descriptor validation");

    const auto localWindows = cortex::target::windows::DescribeLocalNode();
    check(localWindows.Valid(), "Windows local adapter returns a valid node");
    check(localWindows.platform == Platform::Windows, "Windows local adapter declares Windows");
    check(localWindows.architecture == cortex::target::windows::BuildArchitecture(),
          "Windows node architecture follows build architecture");

    const auto current = cortex::target::windows::DescribeProcess(
        localWindows, static_cast<uint64_t>(GetCurrentProcessId()), "capability_model_tests", set);
    check(current.Valid(), "Windows process adapter returns a valid target");
    check(current.nodeId == localWindows.id, "target belongs to local node");
    check(current.processId == static_cast<uint64_t>(GetCurrentProcessId()),
          "target process id is preserved");

    if (failures) return 1;
    std::cout << "PASS: target/node capability model\n";
    return 0;
}

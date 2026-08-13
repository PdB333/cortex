#include "../core/target/model.h"

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
    check(MakeProcessTargetId("node-a", Platform::Windows, 42) == "node-a:windows:process:42",
          "stable target id");

    TargetDescriptor descriptor;
    descriptor.id = "node-a:windows:process:42";
    descriptor.nodeId = "node-a";
    descriptor.name = "sample";
    descriptor.platform = Platform::Windows;
    descriptor.architecture = Architecture::X64;
    descriptor.kind = TargetKind::Process;
    descriptor.processId = 42;
    descriptor.capabilities = set;
    check(descriptor.Valid(), "descriptor validation");

    if (failures) return 1;
    std::cout << "PASS: capability model\n";
    return 0;
}

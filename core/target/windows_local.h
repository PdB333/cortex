#pragma once

#include "model.h"
#include "node.h"

#include <windows.h>

#include <string>
#include <utility>

namespace cortex::target::windows {

inline Architecture BuildArchitecture() {
    return sizeof(void*) == 8 ? Architecture::X64 : Architecture::X86;
}

inline NodeDescriptor DescribeLocalNode() {
    char computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(sizeof(computerName));
    std::string name = "Windows local node";
    if (GetComputerNameA(computerName, &size) && size > 0)
        name.assign(computerName, size);

    return MakeLocalNode("local", std::move(name), Platform::Windows, BuildArchitecture());
}

inline TargetDescriptor DescribeProcess(const NodeDescriptor& node,
                                        uint64_t processId,
                                        std::string processName,
                                        CapabilitySet capabilities = {}) {
    TargetDescriptor target;
    target.id = MakeProcessTargetId(node.id, Platform::Windows, processId);
    target.nodeId = node.id;
    target.name = std::move(processName);
    target.platform = Platform::Windows;
    target.architecture = node.architecture;
    target.kind = TargetKind::Process;
    target.processId = processId;
    target.capabilities = capabilities;
    return target;
}

} // namespace cortex::target::windows

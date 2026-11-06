#pragma once

// The process tree under a target (roadmap step 7): parent links and command
// lines, so a multi-process application shows as one tree. Electron and
// Chromium children carry their role in `--type=` (renderer, gpu-process,
// utility, crashpad-handler, zygote); the process without it at the top is
// the main (browser) process. Command lines are read with
// NtQueryInformationProcess, or from the process's PEB where that class is
// missing (PROCESS_VM_READ); nothing is injected or changed.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::target {

struct ProcessNode {
    uint64_t pid = 0;
    uint64_t parent = 0;
    uint64_t generation = 0;      // creation time; guards against PID reuse in parent links
    std::string name;
    std::string path;
    std::string commandLine;
    bool commandLineRead = false;  // false when the process could not be opened
};

// All processes on this machine (Windows), with command lines.
std::vector<ProcessNode> SnapshotProcesses();

// The Chromium/Electron role of a command line: the value of --type= (with
// utility sub-type, "utility:network.mojom.NetworkService"), or empty.
std::string ChromiumRole(const std::string& commandLine);

// {"root", "processes": [{pid, parent, depth, name, path, role, command_line}],
//  "electron": bool, "truncated"}: the root and its descendants in
// depth-first order. A child counts only if it started after its parent
// (a reused parent PID is not followed). Command lines are cut to 1 KiB.
nlohmann::json ProcessTreeJson(const std::vector<ProcessNode>& processes, uint64_t root, size_t maxProcesses = 512);

} // namespace cortex::target

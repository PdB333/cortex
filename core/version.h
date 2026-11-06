#pragma once

// The one place the Cortex version is written. cortex.exe --version, the
// desktop's Help menu and the MCP serverInfo read it; the release workflow
// checks that the built binary reports it.
namespace cortex {
inline constexpr const char* kVersion = "2.0.0";
}

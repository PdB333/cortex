#pragma once

// The per-user stores the desktop shares with `cortex.exe mcp` (whose
// defaults are the same folders): annotations, evidence and investigations.
// In the desktop the actor is the person ("desktop").

#include "../../core/artifacts/annotations.h"
#include "../../core/evidence/evidence.h"
#include "../../core/investigation/investigation.h"
#include "../../core/services/user_data.h"

#include <filesystem>
#include <string>

namespace cortex::desktop {
std::string ExecutableDirectory();
}

namespace cortex::ui {

inline std::filesystem::path DesktopDataDirectory() {
    return cortex::services::UserDataDirectory(std::filesystem::u8path(cortex::desktop::ExecutableDirectory()));
}

inline cortex::artifacts::AnnotationStore& DesktopAnnotations() {
    static cortex::artifacts::AnnotationStore store(DesktopDataDirectory() / "annotations");
    return store;
}

inline cortex::evidence::Store& DesktopEvidence() {
    static cortex::evidence::Store store(DesktopDataDirectory() / "evidence");
    return store;
}

inline cortex::investigation::Store& DesktopInvestigations() {
    static cortex::investigation::Store store(DesktopDataDirectory() / "investigations");
    return store;
}

} // namespace cortex::ui

#pragma once

#include "../../sdk/include/cortex/diag_protocol.h"

#include <windows.h>

namespace diagnostics {

bool SharedChannelInit();
void SharedChannelShutdown();
bool IsSharedChannelReady();
void SharedHeartbeat(const char* source);
void SharedPublishCrash(PEXCEPTION_POINTERS pointers);

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
const CortexDiagSharedState* SharedStateForTesting();
void ResetSharedChannel();
} // namespace testing
#endif

} // namespace diagnostics
